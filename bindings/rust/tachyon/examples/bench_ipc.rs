use std::thread;
use std::time::Instant;
use tachyon_ipc::Bus;

const CAPACITY: usize = 1 << 24; // 16MB
const WARMUP: usize = 1_000;
const ITERATIONS: usize = 100_000;
const MSG_SIZE: usize = 32;
const SOCK_PING: &str = "/tmp/tachyon_rust_bench_ping.sock";
const SOCK_PONG: &str = "/tmp/tachyon_rust_bench_pong.sock";

fn cleanup() {
    let _ = std::fs::remove_file(SOCK_PING);
    let _ = std::fs::remove_file(SOCK_PONG);
}

fn connect_with_retry(path: &str) -> Bus {
    for _ in 0..200 {
        match Bus::connect(path) {
            Ok(bus) => return bus,
            Err(_) => std::thread::sleep(std::time::Duration::from_millis(10)),
        }
    }
    panic!("Failed to connect to {path} after 200 retries");
}

fn main() {
    cleanup();

    println!("--- Tachyon Rust Bench ---");
    println!("Mode:      Ping-Pong RTT");
    println!("Payload:   {MSG_SIZE} bytes");
    println!("Samples:   {ITERATIONS}");

    let server = thread::spawn(|| {
        let rx_bus = Bus::listen(SOCK_PING, CAPACITY).unwrap();
        let tx_bus = Bus::listen(SOCK_PONG, CAPACITY).unwrap();

        for _ in 0..(WARMUP + ITERATIONS) {
            let rx_guard = rx_bus.acquire_rx(u32::MAX).unwrap();
            let len = rx_guard.actual_size;
            let type_id = rx_guard.type_id;
            let tx_guard = tx_bus.acquire_tx(len).unwrap();
            tx_guard.write(rx_guard.data()); // SHM to SHM, single copy
            rx_guard.commit().unwrap();
            tx_guard.commit(len, type_id).unwrap();
        }
    });

    // Give the server time to enter listen(SOCK_PING).
    thread::sleep(std::time::Duration::from_millis(50));

    // Connecting PING unblocks the server, which then calls listen(SOCK_PONG).
    let tx_bus = connect_with_retry(SOCK_PING);
    let rx_bus = connect_with_retry(SOCK_PONG);

    let payload = vec![0xABu8; MSG_SIZE];
    let mut latencies = Vec::with_capacity(ITERATIONS);

    // Warmup
    for _ in 0..WARMUP {
        tx_bus.send(&payload, 1).unwrap();
        let g = rx_bus.acquire_rx(10_000).unwrap();
        g.commit().unwrap();
    }

    println!("\nRunning {ITERATIONS} iterations...");
    let bench_start = Instant::now();

    for _ in 0..ITERATIONS {
        let t0 = Instant::now();
        tx_bus.send(&payload, 1).unwrap();
        let guard = rx_bus.acquire_rx(u32::MAX).unwrap();
        guard.commit().unwrap();
        latencies.push(t0.elapsed().as_nanos() as u64);
    }

    let total_elapsed = bench_start.elapsed();
    server.join().unwrap();

    latencies.sort_unstable();
    let n = latencies.len();

    println!("\n[ RTT Latency Percentiles (ns) ]");
    println!("Min:     {:>8} ns", latencies[0]);
    println!("p50:     {:>8} ns  (Median)", latencies[n / 2]);
    println!("p90:     {:>8} ns", latencies[n * 90 / 100]);
    println!("p99:     {:>8} ns", latencies[n * 99 / 100]);
    println!("p99.9:   {:>8} ns  (Tail)", latencies[n * 999 / 1000]);
    println!("p99.99:  {:>8} ns", latencies[n * 9999 / 10000]);
    println!("Max:     {:>8} ns", latencies[n - 1]);
    println!(
        "\nTotal:      {:.2} ms",
        total_elapsed.as_secs_f64() * 1000.0
    );
    println!(
        "Throughput: {:.2} K RTT/sec",
        ITERATIONS as f64 / total_elapsed.as_secs_f64() / 1000.0
    );

    cleanup();
}
