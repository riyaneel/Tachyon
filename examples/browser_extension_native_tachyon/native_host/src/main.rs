use std::fs::File;
use std::io::{self, ErrorKind, Read, Write};
use std::os::fd::FromRawFd;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use tachyon_ipc::{Bus, make_type_id};

const CAPACITY: usize = 1 << 20;
const PAYLOAD_SIZE: usize = 16;
const REQ_TYPE_ID: u16 = 1;
const RESP_TYPE_ID: u16 = 2;
const SPIN_THRESHOLD: u32 = u32::MAX;

#[derive(Clone, Copy)]
struct Request {
    id: u64,
    value: u32,
}

struct TachyonWorker {
    requests: Bus,
    replies: Bus,
}

fn main() -> io::Result<()> {
    let worker = TachyonWorker::start()?;
    // SAFETY: Chrome launches the native host with owned stdin/stdout pipes.
    // Wrapping fd 0/1 directly avoids stdio locking around the hot framing loop.
    let mut input = unsafe { File::from_raw_fd(0) };
    let mut output = unsafe { File::from_raw_fd(1) };
    let mut message = Vec::with_capacity(256);
    let mut response = Vec::with_capacity(128);

    while read_native_message(&mut input, &mut message)? {
        response.clear();
        response.extend_from_slice(&[0; 4]);
        match parse_request(&message) {
            Some(request) => {
                let value = worker.round_trip(request)?;
                append_response(&mut response, request.id, value);
            }
            None => {
                response.extend_from_slice(b"{\"id\":0,\"error\":\"invalid fixed-shape request\"}");
            }
        }
        write_native_frame(&mut output, &mut response)?;
    }

    Ok(())
}

impl TachyonWorker {
    fn start() -> io::Result<Self> {
        let unique = unique_name();
        let req_path = format!("/tmp/tachyon_native_msg_req_{unique}.sock");
        let resp_path = format!("/tmp/tachyon_native_msg_resp_{unique}.sock");
        let worker_req_path = req_path.clone();
        let worker_resp_path = resp_path.clone();

        thread::spawn(move || {
            if let Err(err) = run_tachyon_echo_worker(&worker_req_path, &worker_resp_path) {
                let _ = writeln!(io::stderr(), "tachyon worker failed: {err:?}");
            }
        });

        let requests = connect_with_retry(&req_path)?;
        let replies = Bus::listen(&resp_path, CAPACITY)
            .map_err(|err| io::Error::other(format!("listen replies: {err:?}")))?;
        requests.set_polling_mode(1);
        replies.set_polling_mode(1);

        Ok(Self { requests, replies })
    }

    fn round_trip(&self, request: Request) -> io::Result<u32> {
        let mut tx = self
            .requests
            .acquire_tx(PAYLOAD_SIZE)
            .map_err(|err| io::Error::other(format!("tachyon acquire tx: {err:?}")))?;
        unsafe {
            let slot = tx.as_mut_slice();
            slot[..8].copy_from_slice(&request.id.to_le_bytes());
            slot[8..12].copy_from_slice(&request.value.to_le_bytes());
            slot[12..16].copy_from_slice(&(REQ_TYPE_ID as u32).to_le_bytes());
        }
        tx.commit(PAYLOAD_SIZE, make_type_id(1, REQ_TYPE_ID))
            .map_err(|err| io::Error::other(format!("tachyon commit tx: {err:?}")))?;

        let rx = self
            .replies
            .acquire_rx(SPIN_THRESHOLD)
            .map_err(|err| io::Error::other(format!("tachyon acquire rx: {err:?}")))?;
        let data = rx.data();
        if data.len() != PAYLOAD_SIZE {
            return Err(io::Error::new(
                ErrorKind::InvalidData,
                "unexpected Tachyon reply size",
            ));
        }
        let mut id = [0u8; 8];
        let mut value = [0u8; 4];
        id.copy_from_slice(&data[..8]);
        value.copy_from_slice(&data[8..12]);
        if u64::from_le_bytes(id) != request.id {
            return Err(io::Error::new(
                ErrorKind::InvalidData,
                "unexpected Tachyon reply id",
            ));
        }
        let value = u32::from_le_bytes(value);
        rx.commit()
            .map_err(|err| io::Error::other(format!("tachyon commit rx: {err:?}")))?;
        Ok(value)
    }
}

fn run_tachyon_echo_worker(req_path: &str, resp_path: &str) -> io::Result<()> {
    let requests = Bus::listen(req_path, CAPACITY)
        .map_err(|err| io::Error::other(format!("listen requests: {err:?}")))?;
    let replies = connect_with_retry(resp_path)?;
    requests.set_polling_mode(1);
    replies.set_polling_mode(1);

    loop {
        let rx = requests
            .acquire_rx(SPIN_THRESHOLD)
            .map_err(|err| io::Error::other(format!("worker acquire rx: {err:?}")))?;
        let data = rx.data();
        if data.len() == PAYLOAD_SIZE {
            let mut id = [0u8; 8];
            let mut value = [0u8; 4];
            id.copy_from_slice(&data[..8]);
            value.copy_from_slice(&data[8..12]);

            let reply_id = u64::from_le_bytes(id);
            let reply_value = u32::from_le_bytes(value).wrapping_add(1);
            let mut tx = replies
                .acquire_tx(PAYLOAD_SIZE)
                .map_err(|err| io::Error::other(format!("worker acquire tx: {err:?}")))?;
            unsafe {
                let slot = tx.as_mut_slice();
                slot[..8].copy_from_slice(&reply_id.to_le_bytes());
                slot[8..12].copy_from_slice(&reply_value.to_le_bytes());
                slot[12..16].copy_from_slice(&(RESP_TYPE_ID as u32).to_le_bytes());
            }
            tx.commit(PAYLOAD_SIZE, make_type_id(1, RESP_TYPE_ID))
                .map_err(|err| io::Error::other(format!("worker commit tx: {err:?}")))?;
        }
        rx.commit()
            .map_err(|err| io::Error::other(format!("worker commit rx: {err:?}")))?;
    }
}

fn connect_with_retry(path: &str) -> io::Result<Bus> {
    let started = Instant::now();
    loop {
        match Bus::connect(path) {
            Ok(bus) => return Ok(bus),
            Err(err) if started.elapsed() < Duration::from_secs(5) => {
                let _ = err;
                thread::sleep(Duration::from_millis(1));
            }
            Err(err) => return Err(io::Error::other(format!("connect {path}: {err:?}"))),
        }
    }
}

fn read_native_message<R: Read>(input: &mut R, buffer: &mut Vec<u8>) -> io::Result<bool> {
    let mut length = [0u8; 4];
    match input.read_exact(&mut length) {
        Ok(()) => {}
        Err(err) if err.kind() == ErrorKind::UnexpectedEof => return Ok(false),
        Err(err) => return Err(err),
    }

    let length = u32::from_ne_bytes(length) as usize;
    if length > 64 * 1024 * 1024 {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "native message exceeds Chrome input limit",
        ));
    }
    buffer.resize(length, 0);
    input.read_exact(buffer)?;
    Ok(true)
}

fn write_native_frame<W: Write>(output: &mut W, frame: &mut [u8]) -> io::Result<()> {
    let message_len = frame.len().saturating_sub(4);
    if message_len > 1024 * 1024 {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            "native response exceeds Chrome output limit",
        ));
    }
    frame[..4].copy_from_slice(&(message_len as u32).to_ne_bytes());
    output.write_all(frame)?;
    output.flush()
}

fn parse_request(message: &[u8]) -> Option<Request> {
    if !message.starts_with(b"{\"id\":") {
        return None;
    }

    let mut idx = 6;
    let id = parse_u64_at(message, &mut idx)?;
    if message.get(idx..idx + 9)? != b",\"value\":" {
        return None;
    }
    idx += 9;
    let value = parse_u64_at(message, &mut idx)? as u32;
    if message.get(idx) != Some(&b'}') {
        return None;
    }
    Some(Request { id, value })
}

fn parse_u64_at(message: &[u8], idx: &mut usize) -> Option<u64> {
    let mut value = 0u64;
    let start = *idx;
    while *idx < message.len() && message[*idx].is_ascii_digit() {
        value = value * 10 + (message[*idx] - b'0') as u64;
        *idx += 1;
    }
    (*idx > start).then_some(value)
}

fn append_response(response: &mut Vec<u8>, id: u64, value: u32) {
    response.extend_from_slice(b"{\"id\":");
    append_u64(response, id);
    response.extend_from_slice(b",\"value\":");
    append_u64(response, value as u64);
    response.push(b'}');
}

fn append_u64(out: &mut Vec<u8>, mut value: u64) {
    let mut digits = [0u8; 20];
    let mut idx = digits.len();
    loop {
        idx -= 1;
        digits[idx] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    out.extend_from_slice(&digits[idx..]);
}

fn unique_name() -> String {
    let pid = std::process::id();
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    format!("{pid}_{nanos}")
}
