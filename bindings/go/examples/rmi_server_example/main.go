// MIT License © 2025 Binary Dice Games

// RMI server example using the Bison Go binding. Mirrors
// bindings/python/examples/rmi_server_example.py /
// bindings/rust/examples/rmi_server_example.rs. Command-line flags match
// the -transport/-host/-port/-name convention used across the other
// examples.
//
// Run with: go run ./examples/rmi_server_example [-transport=tcp|pipe] [-host=HOST] [-port=PORT] [-name=PATH]
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"

	"github.com/binary-dice-games/bison/bindings/go/bison"
)

func check(err error) {
	if err != nil {
		panic(err)
	}
}

func methodAdd(self, params, result *bison.Dynamic) {
	a, _ := params.GetFloat("a")
	b, _ := params.GetFloat("b")
	_ = result.SetFloat("result", a+b)
}

func methodSubtract(self, params, result *bison.Dynamic) {
	a, _ := params.GetFloat("a")
	b, _ := params.GetFloat("b")
	_ = result.SetFloat("result", a-b)
}

func methodMultiply(self, params, result *bison.Dynamic) {
	a, _ := params.GetFloat("a")
	b, _ := params.GetFloat("b")
	_ = result.SetFloat("result", a*b)
}

func methodDivide(self, params, result *bison.Dynamic) {
	a, _ := params.GetFloat("a")
	b, _ := params.GetFloat("b")
	if b == 0 {
		_ = result.SetString("error", "division by zero")
		_ = result.SetFloat("result", 0)
		return
	}
	_ = result.SetFloat("result", a/b)
}

func registerCalculator() {
	proto, err := bison.New("Calculator")
	check(err)
	check(proto.AddMethod("add", methodAdd))
	check(proto.AddMethod("subtract", methodSubtract))
	check(proto.AddMethod("multiply", methodMultiply))
	check(proto.AddMethod("divide", methodDivide))
	check(bison.AddClass(proto, "", "", nil))
}

func main() {
	transport := flag.String("transport", "tcp", "transport: tcp|pipe")
	host := flag.String("host", "0.0.0.0", "bind host (tcp transport)")
	port := flag.Int("port", 7070, "bind port (tcp transport)")
	name := flag.String("name", "", "pipe/socket path (pipe transport)")
	flag.Parse()

	registerCalculator()

	var server *bison.Server
	var err error
	if *transport == "pipe" {
		server, err = bison.NewPipeServer(*name)
	} else {
		server, err = bison.NewTCPServer(*host, uint16(*port))
	}
	check(err)
	defer server.Close()

	check(server.Listen(nil, nil))
	if *transport == "pipe" {
		fmt.Printf("[Server] Calculator listening on pipe %s\n", *name)
	} else {
		fmt.Printf("[Server] Calculator listening on %s:%d\n", *host, *port)
	}
	fmt.Println("[Server] Press Enter to stop...")

	reader := bufio.NewReader(os.Stdin)
	_, _ = reader.ReadString('\n')

	server.Stop()
	fmt.Println("[Server] stopped.")
}
