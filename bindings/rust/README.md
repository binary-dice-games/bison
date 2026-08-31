# bison

Rust bindings for [Bison](https://github.com/binary-dice-games/bison), a
C++20 library for serializing dynamic, self-describing objects to a compact
binary format, plus RMI (Remote Method Invocation) over TCP, TLS, named-pipe,
and stdio transports.

This crate links directly against the precompiled `bison_abi` shared library
at build time (via `build.rs`) — the same model `bindings/cpp/` uses. It is
not published to crates.io; use it from a checkout of this repository.

## Build

Build `bison_abi` first (from the repo root):

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_abi
```

Then build the crate:

```bash
cd bindings/rust
cargo build
```

`build.rs` looks for `bison_abi` in this order: the `BISON_LIB` environment
variable (a full path to `libbison_abi.so`/`.dylib`/`bison_abi.dll`), then a
sibling `build/` directory next to `bindings/rust/`, then the system library
search path. Set `BISON_LIB` explicitly if the library isn't found:

```bash
export BISON_LIB=$(pwd)/../../build/libbison_abi.so   # Linux, from bindings/rust
```

## Quick start

```rust
use bison::dynamic::Dynamic;

let mut p = Dynamic::new("Player");
p.set("hp", 100).unwrap();
p.set("name", "hero").unwrap();
println!("{}", p.get_int("hp").unwrap());   // 100
```

```rust
use bison::rmi::Client;
use bison::dynamic::Dynamic;

let mut client = Client::standalone();
client.connect(None).unwrap();
let calc = client.instantiate("Calculator", "", None).unwrap();
let mut args = Dynamic::default();
args.set("a", 1.0f32).unwrap();
args.set("b", 2.0f32).unwrap();
let result = calc.call("add", Some(&args), -1).unwrap();   // calls the "add" remote method
println!("{}", result.get_float("result").unwrap());
```

## Examples and tests

```bash
cargo run --example bison_example
cargo run --example rmi_standalone_example
cargo run --example rmi_server_example -- --transport=tcp --port=7070   # separate terminal
cargo run --example rmi_client_example -- --transport=tcp --port=7070

cargo test
```

Full binding documentation (field/method access, RMI transports, auth,
serialization) lives in
[docs/bindings.md](https://github.com/binary-dice-games/bison/blob/main/docs/bindings.md#rust-bindingsrust).
