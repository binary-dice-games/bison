# bison

Go bindings for [Bison](https://github.com/binary-dice-games/bison), a
C++20 library for serializing dynamic, self-describing objects to a compact
binary format, plus RMI (Remote Method Invocation) over TCP, TLS,
named-pipe, and stdio transports.

This module links directly against the precompiled `bison_abi` shared
library at build time via `cgo` (see `bison/native.go`) — the same model
`bindings/cpp/` and `bindings/rust/` use. It is not published as a Go
module elsewhere; use it from a checkout of this repository (import path
`github.com/binary-dice-games/bison/bindings/go/bison`).

## Build

Build `bison_abi` first (from the repo root):

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_abi
```

Then build the module:

```bash
cd bindings/go
go build ./...
```

`bison/native.go`'s `#cgo` directives resolve `bison_c.h`/`rmi_c.h` and
`libbison_abi` against this checkout's sibling `include/` and `build/`
directories by default. To build against a `bison_abi` installed elsewhere,
set `CGO_CFLAGS`/`CGO_LDFLAGS` before building — the Go toolchain merges
env-supplied flags with the `#cgo` directives automatically (the Go-native
equivalent of every other binding's `BISON_LIB` override):

```bash
export CGO_CFLAGS="-I/path/to/bison/include"
export CGO_LDFLAGS="-L/path/to/bison/build -lbison_abi -Wl,-rpath,/path/to/bison/build"
go build ./...
```

Requires `CGO_ENABLED=1` (the default) and a C compiler on `PATH`.

## Quick start

```go
p, _ := bison.New("Player")
defer p.Close()
p.SetInt("hp", 100)
p.SetString("name", "hero")
hp, _ := p.GetInt("hp") // 100
```

```go
client, _ := bison.NewStandaloneClient()
defer client.Close()
client.Connect(nil)
calc, _ := client.Instantiate("Calculator", "", nil)
defer calc.Close()
args, _ := bison.New("")
defer args.Close()
args.SetFloat("a", 1.0)
args.SetFloat("b", 2.0)
result, _ := calc.Call("add", args, -1) // calls the "add" remote method
defer result.Close()
sum, _ := result.GetFloat("result")
```

## Examples and tests

```bash
go run ./examples/bison_example
go run ./examples/rmi_standalone_example
go run ./examples/rmi_server_example -transport=tcp -port=7070   # separate terminal
go run ./examples/rmi_client_example -transport=tcp -port=7070

go test ./...
```

Full binding documentation (field/method access, RMI transports, auth,
serialization) lives in
[docs/bindings.md](https://github.com/binary-dice-games/bison/blob/main/docs/bindings.md#go-bindingsgo).
