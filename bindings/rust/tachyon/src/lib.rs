mod bus;
mod error;

pub use bus::{BatchIter, Bus, RxBatchGuard, RxGuard, RxMsgView, TxGuard};
pub use error::TachyonError;

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;
    use std::time::Duration;

    const CAPACITY: usize = 1 << 16; // 64KB

    fn sock(name: &str) -> String {
        format!("/tmp/tachyon_rust_{name}.sock")
    }

    fn cleanup(path: &str) {
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn test_basic_send_recv() {
        let path = sock("basic");
        cleanup(&path);

        let path_srv = path.clone();
        let srv = thread::spawn(move || {
            let bus = Bus::listen(&path_srv, CAPACITY).unwrap();
            let guard = bus.acquire_rx(10_000).unwrap();
            assert_eq!(guard.type_id, 42);
            assert_eq!(guard.data(), b"hello_tachyon");
            guard.commit().unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let bus = Bus::connect(&path).unwrap();
        bus.send(b"hello_tachyon", 42).unwrap();

        srv.join().unwrap();
        cleanup(&path);
    }

    #[test]
    fn test_zero_copy_tx() {
        let path = sock("zc_tx");
        cleanup(&path);
        let payload = b"zero_copy_tx_data";

        let path_srv = path.clone();
        let srv = thread::spawn(move || {
            let bus = Bus::listen(&path_srv, CAPACITY).unwrap();
            let guard = bus.acquire_rx(10_000).unwrap();
            assert_eq!(guard.type_id, 7);
            assert_eq!(guard.data(), b"zero_copy_tx_data");
            guard.commit().unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let bus = Bus::connect(&path).unwrap();
        let guard = bus.acquire_tx(payload.len()).unwrap();
        guard.write(payload);
        guard.commit(payload.len(), 7).unwrap();

        srv.join().unwrap();
        cleanup(&path);
    }

    #[test]
    fn test_zero_copy_rx() {
        let path = sock("zc_rx");
        cleanup(&path);

        let path_srv = path.clone();
        let srv = thread::spawn(move || {
            let bus = Bus::listen(&path_srv, CAPACITY).unwrap();
            let guard = bus.acquire_rx(10_000).unwrap();
            // data() gives a zero-copy slice tied to guard's lifetime,
            // borrow checker prevents access after commit().
            let data = guard.data();
            assert_eq!(data, b"zero_copy_rx");
            assert_eq!(guard.actual_size, 12);
            guard.commit().unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let bus = Bus::connect(&path).unwrap();
        bus.send(b"zero_copy_rx", 0).unwrap();

        srv.join().unwrap();
        cleanup(&path);
    }

    #[test]
    fn test_batch_drain() {
        let path = sock("batch");
        cleanup(&path);

        let path_srv = path.clone();
        let srv = thread::spawn(move || {
            let bus = Bus::listen(&path_srv, CAPACITY).unwrap();
            let batch = bus.drain_batch(64, 10_000).unwrap();
            assert_eq!(batch.len(), 5);
            for (i, msg) in batch.iter().enumerate() {
                assert_eq!(msg.type_id(), i as u32);
                assert_eq!(msg.actual_size(), 4);
                let val = u32::from_le_bytes(msg.data().try_into().unwrap());
                assert_eq!(val, i as u32 * 10);
            }
            batch.commit().unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let bus = Bus::connect(&path).unwrap();
        // commit_unflushed batches all 5 messages, single flush at the end.
        for i in 0u32..5 {
            let val = (i * 10).to_le_bytes();
            let guard = bus.acquire_tx(4).unwrap();
            guard.write(&val);
            guard.commit_unflushed(4, i).unwrap();
        }
        bus.flush();

        srv.join().unwrap();
        cleanup(&path);
    }

    #[test]
    fn test_txguard_drop_rollback() {
        let path = sock("rollback");
        cleanup(&path);

        let path_srv = path.clone();
        let srv = thread::spawn(move || {
            let bus = Bus::listen(&path_srv, CAPACITY).unwrap();

            // Drop without commit is now a true rollback, no phantom message.
            let guard = bus.acquire_rx(10_000).unwrap();
            assert_eq!(guard.type_id, 99);
            assert_eq!(guard.data(), b"committed");
            guard.commit().unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let bus = Bus::connect(&path).unwrap();

        // Drop without explicit commit → rollback, no message published.
        {
            let _guard = bus.acquire_tx(32).unwrap();
        }
        bus.flush();

        bus.send(b"committed", 99).unwrap();

        srv.join().unwrap();
        cleanup(&path);
    }

    #[test]
    fn test_batch_into_iterator() {
        let path = sock("into_iter");
        cleanup(&path);

        let path_srv = path.clone();
        let srv = thread::spawn(move || {
            let bus = Bus::listen(&path_srv, CAPACITY).unwrap();
            let batch = bus.drain_batch(16, 10_000).unwrap();

            let payloads: Vec<Vec<u8>> = (&batch)
                .into_iter()
                .map(|msg| msg.data().to_vec())
                .collect();

            assert_eq!(payloads.len(), 3);
            assert_eq!(payloads[0], b"a");
            assert_eq!(payloads[1], b"bb");
            assert_eq!(payloads[2], b"ccc");

            batch.commit().unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let bus = Bus::connect(&path).unwrap();
        // Single flush after all three guarantees drain_batch sees them together.
        for (data, tid) in [(b"a".as_ref(), 0u32), (b"bb", 1), (b"ccc", 2)] {
            let guard = bus.acquire_tx(data.len()).unwrap();
            guard.write(data);
            guard.commit_unflushed(data.len(), tid).unwrap();
        }
        bus.flush();

        srv.join().unwrap();
        cleanup(&path);
    }
}
