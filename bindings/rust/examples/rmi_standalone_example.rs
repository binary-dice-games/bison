// MIT License © 2025 Binary Dice Games
//! Standalone RMI example using the Bison Rust binding.
//!
//! Mirrors `bindings/python/examples/rmi_standalone_example.py`: no separate
//! server process -- `Client::standalone()` dispatches directly to the
//! local (in-process) class registry. Three threads each instantiate a
//! remote Calculator, perform several operations concurrently, then clean up.
//!
//! Run with: `cargo run --example rmi_standalone_example`

use std::sync::Mutex;

use bison::dynamic::{add_class, Dynamic};
use bison::rmi::Client;

static PRINT_LOCK: Mutex<()> = Mutex::new(());

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

fn run_client(client_id: i32) {
    let mut client = Client::standalone();
    client.connect(None).unwrap();
    let calc = client.instantiate("Calculator", "", None).unwrap();
    {
        let _g = PRINT_LOCK.lock().unwrap();
        println!("[Client {client_id}] connected");
    }

    let a = 10.0 * client_id as f32;
    let mut args = Dynamic::default();
    args.set("a", a).unwrap();
    args.set("b", 3.0f32).unwrap();
    let r = calc.call("add", Some(&args), -1).unwrap();
    {
        let _g = PRINT_LOCK.lock().unwrap();
        println!(
            "[Client {client_id}] add({:.0}, 3) = {:.0}",
            a,
            r.get_float("result").unwrap()
        );
    }

    let b = 7.0 * client_id as f32;
    let mut args = Dynamic::default();
    args.set("a", 100.0f32).unwrap();
    args.set("b", b).unwrap();
    let r = calc.call("subtract", Some(&args), -1).unwrap();
    {
        let _g = PRINT_LOCK.lock().unwrap();
        println!(
            "[Client {client_id}] subtract(100, {:.0}) = {:.0}",
            b,
            r.get_float("result").unwrap()
        );
    }

    let v = client_id as f32;
    let mut args = Dynamic::default();
    args.set("a", v).unwrap();
    args.set("b", v).unwrap();
    let r = calc.call("multiply", Some(&args), -1).unwrap();
    {
        let _g = PRINT_LOCK.lock().unwrap();
        println!(
            "[Client {client_id}] multiply({:.0}, {:.0}) = {:.0}",
            v,
            v,
            r.get_float("result").unwrap()
        );
    }

    let b = client_id as f32; // non-zero since client_id >= 1
    let mut args = Dynamic::default();
    args.set("a", 42.0f32).unwrap();
    args.set("b", b).unwrap();
    let r = calc.call("divide", Some(&args), -1).unwrap();
    {
        let _g = PRINT_LOCK.lock().unwrap();
        println!(
            "[Client {client_id}] divide(42, {:.0}) = {:.0}",
            b,
            r.get_float("result").unwrap()
        );
    }

    drop(calc);
    client.disconnect().unwrap();

    let _g = PRINT_LOCK.lock().unwrap();
    println!("[Client {client_id}] done.");
}

fn main() {
    register_calculator();
    println!("[Server] RMI Calculator registered (standalone in-process mode).");

    let handles: Vec<_> = (1..4)
        .map(|i| std::thread::spawn(move || run_client(i)))
        .collect();
    for h in handles {
        h.join().unwrap();
    }

    println!("[Server] all clients done.");
}
