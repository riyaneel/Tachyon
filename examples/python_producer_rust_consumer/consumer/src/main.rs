use std::time::Instant;
use tachyon_ipc::Bus;

const SOCKET_PATH: &str = "/tmp/tachyon_market.sock";

const TYPE_BID: u32 = 1;
const TYPE_ASK: u32 = 2;
const TYPE_SENTINEL: u32 = 0;

const REPORT_EVERY: u64 = 10_000;

fn main() {
    println!("[consumer] Connecting to {SOCKET_PATH} ...");
    let bus = Bus::connect(SOCKET_PATH).expect("failed to connect — is the producer running?");
    bus.set_polling_mode(1);
    println!("[consumer] Connected. Receiving ticks ...\n");

    let mut total = 0u64;
    let mut bids = 0u64;
    let mut asks = 0u64;
    let mut latency_sum = 0u64;
    let start = Instant::now();

    loop {
        let guard = match bus.acquire_rx(10_000) {
            Ok(g) => g,
            Err(e) => {
                eprintln!("[consumer] Bus error: {e}");
                break;
            }
        };

        let type_id = guard.type_id;

        // Sentinel — producer signals end of stream
        if type_id == TYPE_SENTINEL {
            guard.commit().unwrap();
            break;
        }

        let data = guard.data();
        let recv_ns = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos() as u64;

        if data.len() >= 32 {
            let base = data.as_ptr();
            let symbol = unsafe { std::ptr::read_unaligned(base as *const [u8; 8]) };
            let price = unsafe { std::ptr::read_unaligned(base.add(8) as *const f64) };
            let quantity = unsafe { std::ptr::read_unaligned(base.add(16) as *const u32) };
            let timestamp_ns = unsafe { std::ptr::read_unaligned(base.add(20) as *const u64) };

            let latency_ns = recv_ns.saturating_sub(timestamp_ns);
            latency_sum += latency_ns;

            match type_id {
                TYPE_BID => bids += 1,
                TYPE_ASK => asks += 1,
                _ => {}
            }

            total += 1;

            if total % REPORT_EVERY == 0 {
                let end = symbol.iter().position(|&b| b == 0).unwrap_or(8);
                let sym = std::str::from_utf8(&symbol[..end]).unwrap_or("?");
                let side = if type_id == TYPE_BID { "BID" } else { "ASK" };
                let avg_lat_us = latency_sum as f64 / REPORT_EVERY as f64 / 1000.0;
                println!(
                    "[consumer] {:>8} ticks | last: {} {} @ {:.2} qty={} | avg latency {:.1} µs",
                    total, sym, side, price, quantity, avg_lat_us,
                );
                latency_sum = 0;
            }
        }

        guard.commit().unwrap();
    }

    let elapsed = start.elapsed();
    println!("\n[consumer] Summary");
    println!("  Total ticks : {total}");
    println!("  BIDs        : {bids}");
    println!("  ASKs        : {asks}");
    println!("  Elapsed     : {:.2} ms", elapsed.as_secs_f64() * 1000.0);
    if elapsed.as_secs_f64() > 0.0 {
        println!(
            "  Throughput  : {:.0} ticks/sec",
            total as f64 / elapsed.as_secs_f64()
        );
    }
}
