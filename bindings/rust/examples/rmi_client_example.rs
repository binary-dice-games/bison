// MIT License © 2025 Binary Dice Games
//! RMI client example using the Bison Rust binding.
//!
//! Mirrors `bindings/python/examples/rmi_client_example.py`. Run
//! `rmi_server_example` (or any other Calculator server) with matching
//! flags before starting this client.
//!
//! Run with: `cargo run --example rmi_client_example -- [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]`

use bison::dynamic::Dynamic;
use bison::rmi::Client;

struct Args {
    transport: String,
    host: String,
    port: u16,
    name: String,
}

fn parse_args() -> Args {
    let mut args = Args {
        transport: "tcp".to_string(),
        host: "127.0.0.1".to_string(),
        port: 7070,
        name: String::new(),
    };
    for arg in std::env::args().skip(1) {
        if let Some(v) = arg.strip_prefix("--transport=") {
            args.transport = v.to_string();
        } else if let Some(v) = arg.strip_prefix("--host=") {
            args.host = v.to_string();
        } else if let Some(v) = arg.strip_prefix("--port=") {
            args.port = v.parse().expect("--port must be a valid u16");
        } else if let Some(v) = arg.strip_prefix("--name=") {
            args.name = v.to_string();
        }
    }
    args
}

fn main() {
    let args = parse_args();

    let mut client = if args.transport == "tcp" {
        Client::tcp(&args.host, args.port)
    } else {
        Client::pipe(&args.name)
    };
    client.connect(None).unwrap();

    let calc = client.instantiate("Calculator", "", None).unwrap();
    println!("[Client] connected");

    let mut args_ab = Dynamic::default();
    args_ab.set("a", 10.0f32).unwrap();
    args_ab.set("b", 3.0f32).unwrap();
    let r = calc.call("add", Some(&args_ab), -1).unwrap();
    println!(
        "[Client] add(10, 3) = {:.0}",
        r.get_float("result").unwrap()
    );

    let mut args_ab = Dynamic::default();
    args_ab.set("a", 100.0f32).unwrap();
    args_ab.set("b", 21.0f32).unwrap();
    let r = calc.call("subtract", Some(&args_ab), -1).unwrap();
    println!(
        "[Client] subtract(100, 21) = {:.0}",
        r.get_float("result").unwrap()
    );

    let mut args_ab = Dynamic::default();
    args_ab.set("a", 7.0f32).unwrap();
    args_ab.set("b", 6.0f32).unwrap();
    let r = calc.call("multiply", Some(&args_ab), -1).unwrap();
    println!(
        "[Client] multiply(7, 6) = {:.0}",
        r.get_float("result").unwrap()
    );

    let mut args_ab = Dynamic::default();
    args_ab.set("a", 42.0f32).unwrap();
    args_ab.set("b", 2.0f32).unwrap();
    let r = calc.call("divide", Some(&args_ab), -1).unwrap();
    println!(
        "[Client] divide(42, 2) = {:.0}",
        r.get_float("result").unwrap()
    );

    drop(calc);
    client.disconnect().unwrap();

    println!("[Client] done.");
}
