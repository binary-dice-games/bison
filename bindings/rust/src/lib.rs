// MIT License © 2025 Binary Dice Games
//! Rust bindings for [Bison](https://github.com/binary-dice-games/bison), a
//! C++20 library for serializing dynamic, self-describing objects to a
//! compact binary format, plus RMI (Remote Method Invocation) over TCP,
//! TLS, named-pipe, and stdio transports.
//!
//! This crate links directly against the precompiled `bison_abi` shared
//! library at build time (see `build.rs`), the same model
//! `bindings/cpp/` uses -- unlike the Python and C# bindings, which
//! `dlopen`/P-Invoke it at run time. [`sys`] is the hand-maintained raw FFI
//! layer; [`dynamic`] and [`rmi`] are the safe, idiomatic wrappers most
//! callers should use.
//!
//! # Quick start
//!
//! ```no_run
//! use bison::dynamic::Dynamic;
//!
//! let mut p = Dynamic::new("Player");
//! p.set("hp", 100).unwrap();
//! p.set("name", "hero").unwrap();
//! assert_eq!(p.get_int("hp").unwrap(), 100);
//! ```
//!
//! ```no_run
//! use bison::rmi::Client;
//!
//! let mut client = Client::standalone();
//! client.connect(None).unwrap();
//! let calc = client.instantiate("Calculator", "", None).unwrap();
//! let mut args = bison::dynamic::Dynamic::default();
//! args.set("a", 1.0f32).unwrap();
//! args.set("b", 2.0f32).unwrap();
//! let result = calc.call("add", Some(&args), -1).unwrap(); // calls the "add" remote method
//! println!("{}", result.get_float("result").unwrap());
//! ```

pub mod dynamic;
pub mod rmi;
pub mod sys;

pub use dynamic::{
    add_class, class_attributes, clear_registry, deserialize, find_class, from_json, from_yaml,
    instantiate, key, Attributes, BisonError, Dynamic, Value,
};
pub use rmi::{Client, Future, Proxy, RmiError, Server};
