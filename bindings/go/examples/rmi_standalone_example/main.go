// MIT License © 2025 Binary Dice Games

// Standalone RMI example using the Bison Go binding. Mirrors
// bindings/python/examples/rmi_standalone_example.py /
// bindings/rust/examples/rmi_standalone_example.rs: no separate server
// process -- bison.NewStandaloneClient() dispatches directly to the local
// (in-process) class registry. Three goroutines each instantiate a remote
// Calculator, perform several operations concurrently, then clean up.
//
// Run with: go run ./examples/rmi_standalone_example
package main

import (
	"fmt"
	"sync"

	"github.com/binary-dice-games/bison/bindings/go/bison"
)

var printLock sync.Mutex

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

func runClient(clientID int32) {
	client, err := bison.NewStandaloneClient()
	check(err)
	defer client.Close()
	check(client.Connect(nil))

	calc, err := client.Instantiate("Calculator", "", nil)
	check(err)
	defer calc.Close()

	printLock.Lock()
	fmt.Printf("[Client %d] connected\n", clientID)
	printLock.Unlock()

	a := 10.0 * float32(clientID)
	args, _ := bison.NewAnonymous()
	check(args.SetFloat("a", a))
	check(args.SetFloat("b", 3))
	r, err := calc.Call("add", args, -1)
	check(err)
	args.Close()
	res, _ := r.GetFloat("result")
	printLock.Lock()
	fmt.Printf("[Client %d] add(%.0f, 3) = %.0f\n", clientID, a, res)
	printLock.Unlock()
	r.Close()

	b := 7.0 * float32(clientID)
	args, _ = bison.NewAnonymous()
	check(args.SetFloat("a", 100))
	check(args.SetFloat("b", b))
	r, err = calc.Call("subtract", args, -1)
	check(err)
	args.Close()
	res, _ = r.GetFloat("result")
	printLock.Lock()
	fmt.Printf("[Client %d] subtract(100, %.0f) = %.0f\n", clientID, b, res)
	printLock.Unlock()
	r.Close()

	v := float32(clientID)
	args, _ = bison.NewAnonymous()
	check(args.SetFloat("a", v))
	check(args.SetFloat("b", v))
	r, err = calc.Call("multiply", args, -1)
	check(err)
	args.Close()
	res, _ = r.GetFloat("result")
	printLock.Lock()
	fmt.Printf("[Client %d] multiply(%.0f, %.0f) = %.0f\n", clientID, v, v, res)
	printLock.Unlock()
	r.Close()

	divB := float32(clientID) // non-zero since clientID >= 1
	args, _ = bison.NewAnonymous()
	check(args.SetFloat("a", 42))
	check(args.SetFloat("b", divB))
	r, err = calc.Call("divide", args, -1)
	check(err)
	args.Close()
	res, _ = r.GetFloat("result")
	printLock.Lock()
	fmt.Printf("[Client %d] divide(42, %.0f) = %.0f\n", clientID, divB, res)
	printLock.Unlock()
	r.Close()

	check(client.Disconnect())

	printLock.Lock()
	fmt.Printf("[Client %d] done.\n", clientID)
	printLock.Unlock()
}

func main() {
	registerCalculator()
	fmt.Println("[Server] RMI Calculator registered (standalone in-process mode).")

	var wg sync.WaitGroup
	for i := int32(1); i < 4; i++ {
		wg.Add(1)
		go func(id int32) {
			defer wg.Done()
			runClient(id)
		}(i)
	}
	wg.Wait()

	fmt.Println("[Server] all clients done.")
}
