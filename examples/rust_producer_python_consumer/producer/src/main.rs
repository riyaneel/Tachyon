use std::time::Instant;
use tachyon_ipc::{Bus, TachyonError};

const SOCKET_PATH: &str = "/tmp/tachyon_inference.sock";
const ITERATIONS: usize = 500_000;
const BATCH_SIZE: usize = 32;

const TYPE_FEATURES: u32 = 1;
const TYPE_SENTINEL: u32 = 0;

/// Feature vector: 256 × f32 = 1024 bytes.
/// Values are synthetic — incrementing floats seeded by frame index.
fn make_features(frame: usize, buf: &mut [f32; 256]) {
    let base = frame as f32;
    for (i, v) in buf.iter_mut().enumerate() {
        *v = base + i as f32 * 0.001;
    }
}

fn acquire_with_backpressure(bus: &Bus, size: usize) -> tachyon_ipc::TxGuard<'_> {
    loop {
        match bus.acquire_tx(size) {
            Ok(guard) => return guard,
            Err(TachyonError::BufferFull) => {
                // Consumer is slower than producer — flush pending
                // messages to unblock the ring buffer and retry.
                bus.flush();
                std::hint::spin_loop();
            }
            Err(e) => panic!("acquire_tx failed: {e}"),
        }
    }
}

fn main() {
    println!("[producer] Connecting to {SOCKET_PATH} ...");
    println!("[producer] Waiting for Python consumer to listen ...");

    let bus = {
        let mut result = Err(TachyonError::NetworkError);
        for attempt in 1..=100 {
            result = Bus::connect(SOCKET_PATH);
            if result.is_ok() {
                break;
            }
            if attempt == 1 {
                println!("[producer] Consumer not ready, retrying ...");
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        result.expect("failed to connect after 100 attempts — is the consumer running?")
    };

    println!(
        "[producer] Connected. Sending {ITERATIONS} feature vectors \
         (batch={BATCH_SIZE}) ..."
    );

    let mut features = [0f32; 256];
    let frame_bytes = size_of::<[f32; 256]>(); // 1024

    let start = Instant::now();

    for i in 0..ITERATIONS {
        make_features(i, &mut features);

        let mut guard = acquire_with_backpressure(&bus, frame_bytes);

        // SAFETY: guard points to a writable SHM slot of at least frame_bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(
                features.as_ptr() as *const u8,
                guard.as_mut_slice().as_mut_ptr(),
                frame_bytes,
            );
        }

        guard
            .commit_unflushed(frame_bytes, TYPE_FEATURES)
            .expect("commit failed");

        if (i + 1) % BATCH_SIZE == 0 {
            bus.flush();
        }
    }

    // Sentinel — signals consumer to stop gracefully.
    let mut sentinel = acquire_with_backpressure(&bus, frame_bytes);
    unsafe {
        std::ptr::write_bytes(sentinel.as_mut_slice().as_mut_ptr(), 0, frame_bytes);
    }
    sentinel
        .commit_unflushed(frame_bytes, TYPE_SENTINEL)
        .expect("sentinel commit failed");
    bus.flush();

    let elapsed = start.elapsed();
    println!(
        "[producer] Done. {} frames in {:.1} ms ({:.0} frames/sec)",
        ITERATIONS,
        elapsed.as_secs_f64() * 1000.0,
        ITERATIONS as f64 / elapsed.as_secs_f64(),
    );
}
