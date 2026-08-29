// MIT License © 2025 Binary Dice Games

// RMI client example using the Bison Go binding. Mirrors
// bindings/python/examples/rmi_client_example.py /
// bindings/rust/examples/rmi_client_example.rs. Run rmi_server_example (or
// any other Calculator server) with matching flags before starting this
// client.
//
// Run with: go run ./examples/rmi_client_example [-transport=tcp|pipe] [-host=HOST] [-port=PORT] [-name=PATH]
package main

import (
	"flag"
	"fmt"

	"github.com/binary-dice-games/bison/bindings/go/bison"
)

func check(err error) {
	if err != nil {
		panic(err)
	}
}

func main() {
	transport := flag.String("transport", "tcp", "transport: tcp|pipe")
	host := flag.String("host", "127.0.0.1", "server host (tcp transport)")
	port := flag.Int("port", 7070, "server port (tcp transport)")
	name := flag.String("name", "", "pipe/socket path (pipe transport)")
	flag.Parse()

	var client *bison.Client
	var err error
	if *transport == "pipe" {
		client, err = bison.NewPipeClient(*name)
	} else {
		client, err = bison.NewTCPClient(*host, uint16(*port))
	}
	check(err)
	defer client.Close()
	check(client.Connect(nil))

	calc, err := client.Instantiate("Calculator", "", nil)
	check(err)
	fmt.Println("[Client] connected")

	argsAB, _ := bison.NewAnonymous()
	check(argsAB.SetFloat("a", 10))
	check(argsAB.SetFloat("b", 3))
	r, err := calc.Call("add", argsAB, -1)
	check(err)
	res, _ := r.GetFloat("result")
	fmt.Printf("[Client] add(10, 3) = %.0f\n", res)
	argsAB.Close()
	r.Close()

	argsAB, _ = bison.NewAnonymous()
	check(argsAB.SetFloat("a", 100))
	check(argsAB.SetFloat("b", 21))
	r, err = calc.Call("subtract", argsAB, -1)
	check(err)
	res, _ = r.GetFloat("result")
	fmt.Printf("[Client] subtract(100, 21) = %.0f\n", res)
	argsAB.Close()
	r.Close()

	argsAB, _ = bison.NewAnonymous()
	check(argsAB.SetFloat("a", 7))
	check(argsAB.SetFloat("b", 6))
	r, err = calc.Call("multiply", argsAB, -1)
	check(err)
	res, _ = r.GetFloat("result")
	fmt.Printf("[Client] multiply(7, 6) = %.0f\n", res)
	argsAB.Close()
	r.Close()

	argsAB, _ = bison.NewAnonymous()
	check(argsAB.SetFloat("a", 42))
	check(argsAB.SetFloat("b", 2))
	r, err = calc.Call("divide", argsAB, -1)
	check(err)
	res, _ = r.GetFloat("result")
	fmt.Printf("[Client] divide(42, 2) = %.0f\n", res)
	argsAB.Close()
	r.Close()

	check(calc.Close())
	check(client.Disconnect())

	fmt.Println("[Client] done.")
}
