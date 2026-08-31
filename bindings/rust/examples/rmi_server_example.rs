// MIT License © 2025 Binary Dice Games
//! RMI server example using the Bison Rust binding.
//!
//! Mirrors `bindings/python/examples/rmi_server_example.py`. Command-line
//! flags match the `--transport`/`--host`/`--port`/`--name` convention used
//! across the other examples.
//!
//! Run with: `cargo run --example rmi_server_example -- [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]`

use bison::dynamic::{add_class, Dynamic};
use bison::rmi::Server;

fn method_add(_self_obj: &Dynamic, params: &Dynamic, result: &mut Dynamic) {
    let r = params.get_float("a").unwrap() + params.get_float("b").unwrap();
    result.set("result", r).unwrap();
}

fn method_subtract(_self_obj: &Dynamic, params: &Dynamic, result: &mut Dynamic) {
    let r = params.get_float("a").unwrap() - params.get_float("b").unwrap();
    result.set("result", r).unwrap();
}

fn method_multiply(_self_obj: &Dynamic, params: &Dynamic, result: &mut Dynamic) {
    let r = params.get_float("a").unwrap() * params.get_float("b").unwrap();
    result.set("result", r).unwrap();
}

fn method_divide(_self_obj: &Dynamic, params: &Dynamic, result: &mut Dynamic) {
    let a = params.get_float("a").unwrap();
    let b = params.get_float("b").unwrap();
    if b == 0.0 {
        result.set("error", "division by zero").unwrap();
        result.set("result", 0.0f32).unwrap();
    } else {
        result.set("result", a / b).unwrap();
    }
}

fn register_calculator() {
    let mut proto = Dynamic::new("Calculator");
    proto.add_method("add", method_add).unwrap();
    proto.add_method("subtract", method_subtract).unwrap();
    proto.add_method("multiply", method_multiply).unwrap();
    proto.add_method("divide", method_divide).unwrap();
    add_class(proto, "", "", None).unwrap();
}

struct Args {
    transport: String,
    host: String,
    port: u16,
    name: String,
}

fn parse_args() -> Args {
    let mut args = Args {
        transport: "tcp".to_string(),
        host: "0.0.0.0".to_string(),
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

    register_calculator();

    let mut server = if args.transport == "tcp" {
        Server::tcp(&args.host, args.port)
    } else {
        Server::pipe(&args.name)
    };

    server
        .listen::<fn(&Dynamic) -> (bool, String)>(None, None)
        .unwrap();
    if args.transport == "pipe" {
        println!("[Server] Calculator listening on pipe {}", args.name);
    } else {
        println!(
            "[Server] Calculator listening on {}:{}",
            args.host, args.port
        );
    }
    println!("[Server] Press Enter to stop...");

    let mut line = String::new();
    let _ = std::io::stdin().read_line(&mut line);

    server.stop();
    println!("[Server] stopped.");
}
