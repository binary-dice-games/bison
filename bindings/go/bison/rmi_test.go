// MIT License © 2025 Binary Dice Games

// Tests for the bison package's RMI wrapper (Client/Server/Proxy/Future).
// Mirrors bindings/rust/tests/rmi_tests.rs's coverage and its rationale for
// serializing everything in this file.
//
// Most tests use NewStandaloneClient() (in-process dispatch) so the suite
// has no dependency on sockets/ports being available in the test
// environment. The tcpAuth tests are the exception -- Server.Listen's auth
// callback is evaluated during the OP_CONNECT handshake, which standalone
// sessions skip entirely, so those need a real TCP client/server round
// trip.
//
// Important: the underlying transport/library has a documented,
// pre-existing concurrency characteristic (reproduced independently of any
// binding, in plain concurrent Rust code -- see bindings/rust/tests/
// rmi_tests.rs) where concurrent RMI activity within one process can
// intermittently hang. `go test` already runs the tests in one package
// sequentially by default (nothing here calls t.Parallel()), so this
// should not be reachable in practice -- but every test in this file still
// acquires rmiTestLock first, both as cheap insurance against that default
// changing and because NewStandaloneClient() dispatches into the same
// process-wide class registry bison.dynamic's free functions use, which
// registry-touching tests would otherwise race each other's classes.
package bison

import (
	"sync"
	"sync/atomic"
	"testing"
)

var rmiTestLock sync.Mutex

func registerCalculator(t *testing.T) {
	t.Helper()
	proto, err := New("Calculator")
	if err != nil {
		t.Fatal(err)
	}
	err = proto.AddMethod("add", func(self, params, result *Dynamic) {
		a, _ := params.GetFloat("a")
		b, _ := params.GetFloat("b")
		_ = result.SetFloat("result", a+b)
	})
	if err != nil {
		t.Fatal(err)
	}
	err = proto.AddMethod("echo", func(self, params, result *Dynamic) {
		v, _ := params.GetInt("value")
		_ = result.SetInt("value", v)
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := AddClass(proto, "", "", nil); err != nil {
		t.Fatal(err)
	}
}

func withCalculator(t *testing.T, fn func()) {
	t.Helper()
	rmiTestLock.Lock()
	defer rmiTestLock.Unlock()
	ClearRegistry()
	registerCalculator(t)
	defer ClearRegistry()
	fn()
}

// ═════════════════════════════════════════════════════════════════════════
// Standalone RMI
// ═════════════════════════════════════════════════════════════════════════

func TestStandaloneInstantiateAndCall(t *testing.T) {
	withCalculator(t, func() {
		client, err := NewStandaloneClient()
		if err != nil {
			t.Fatal(err)
		}
		defer client.Close()
		if err := client.Connect(nil); err != nil {
			t.Fatal(err)
		}

		calc, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()

		params, _ := New("")
		defer params.Close()
		must(t, params.SetFloat("a", 10))
		must(t, params.SetFloat("b", 3))

		r, err := calc.Call("add", params, -1)
		if err != nil {
			t.Fatal(err)
		}
		defer r.Close()
		v, err := r.GetFloat("result")
		must(t, err)
		if v != 13 {
			t.Fatalf("result = %v", v)
		}
	})
}

func TestStandaloneGetSetFieldPatch(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))

		calc, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()

		patch, _ := New("")
		defer patch.Close()
		must(t, patch.SetString("label", "primary"))
		if err := calc.Set(patch, -1); err != nil {
			t.Fatal(err)
		}

		snapshot, err := calc.Get(nil, -1)
		if err != nil {
			t.Fatal(err)
		}
		defer snapshot.Close()
		v, err := snapshot.GetString("label")
		must(t, err)
		if v != "primary" {
			t.Fatalf("label = %q", v)
		}
	})
}

func TestStandaloneUnknownMethodErrors(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))
		calc, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()
		if _, err := calc.Call("does_not_exist", nil, -1); err == nil {
			t.Fatal("expected error")
		}
	})
}

func TestStandaloneMultipleProxiesIndependent(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))

		a, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer a.Close()
		b, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer b.Close()

		pa, _ := New("")
		defer pa.Close()
		must(t, pa.SetString("label", "a"))
		must(t, a.Set(pa, -1))

		pb, _ := New("")
		defer pb.Close()
		must(t, pb.SetString("label", "b"))
		must(t, b.Set(pb, -1))

		sa, err := a.Get(nil, -1)
		if err != nil {
			t.Fatal(err)
		}
		defer sa.Close()
		sb, err := b.Get(nil, -1)
		if err != nil {
			t.Fatal(err)
		}
		defer sb.Close()

		la, err := sa.GetString("label")
		must(t, err)
		if la != "a" {
			t.Fatalf("a.label = %q", la)
		}
		lb, err := sb.GetString("label")
		must(t, err)
		if lb != "b" {
			t.Fatalf("b.label = %q", lb)
		}
	})
}

func TestStandaloneAsyncCallRoundTrips(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))
		calc, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()

		params, _ := New("")
		defer params.Close()
		must(t, params.SetFloat("a", 4))
		must(t, params.SetFloat("b", 5))

		future, err := calc.CallAsync("add", params)
		if err != nil {
			t.Fatal(err)
		}
		if err := future.Wait(-1); err != nil {
			t.Fatal(err)
		}
		result, err := future.GetDynamic()
		if err != nil {
			t.Fatal(err)
		}
		defer result.Close()
		v, err := result.GetFloat("result")
		must(t, err)
		if v != 9 {
			t.Fatalf("result = %v", v)
		}
	})
}

func TestStandaloneInstantiateAsyncRoundTrips(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))

		future, err := client.InstantiateAsync("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		if err := future.Wait(-1); err != nil {
			t.Fatal(err)
		}
		calc, err := future.GetProxy()
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()

		params, _ := New("")
		defer params.Close()
		must(t, params.SetInt("value", 7))
		r, err := calc.Call("echo", params, -1)
		if err != nil {
			t.Fatal(err)
		}
		defer r.Close()
		v, err := r.GetInt("value")
		must(t, err)
		if v != 7 {
			t.Fatalf("value = %d", v)
		}
	})
}

func TestStandaloneFutureDoubleConsumeErrors(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))
		calc, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()

		params, _ := New("")
		defer params.Close()
		must(t, params.SetFloat("a", 1))
		must(t, params.SetFloat("b", 1))

		future, err := calc.CallAsync("add", params)
		if err != nil {
			t.Fatal(err)
		}
		must(t, future.Wait(-1))
		result, err := future.GetDynamic()
		if err != nil {
			t.Fatal(err)
		}
		defer result.Close()

		if _, err := future.GetDynamic(); err == nil {
			t.Fatal("expected error consuming an already-consumed future")
		}
	})
}

func TestStandaloneProxyEventRegistrationSucceeds(t *testing.T) {
	withCalculator(t, func() {
		client, _ := NewStandaloneClient()
		defer client.Close()
		must(t, client.Connect(nil))
		calc, err := client.Instantiate("Calculator", "", nil)
		if err != nil {
			t.Fatal(err)
		}
		defer calc.Close()

		// No server push in standalone mode fires this, but registration
		// itself must succeed and not panic -- exercises OnEvent's cgo
		// plumbing end-to-end.
		if err := calc.OnEvent("changed", func(params *Dynamic) {}); err != nil {
			t.Fatal(err)
		}
	})
}

// ═════════════════════════════════════════════════════════════════════════
// TCP + auth
// ═════════════════════════════════════════════════════════════════════════

var nextPort uint32 = 32000

func allocPort() uint16 {
	return uint16(atomic.AddUint32(&nextPort, 1))
}

func TestTCPNoAuthSetConnectSucceeds(t *testing.T) {
	rmiTestLock.Lock()
	defer rmiTestLock.Unlock()

	port := allocPort()
	server, err := NewTCPServer("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	if err := server.Listen(nil, nil); err != nil {
		t.Fatal(err)
	}

	client, err := NewTCPClient("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	if err := client.Connect(nil); err != nil {
		t.Fatal(err)
	}
}

func TestTCPAcceptingCallbackReceivesPayloadAndSetsIdentity(t *testing.T) {
	rmiTestLock.Lock()
	defer rmiTestLock.Unlock()

	port := allocPort()
	server, err := NewTCPServer("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()

	var mu sync.Mutex
	var seen string
	err = server.Listen(nil, func(payload *Dynamic) (bool, string) {
		username, _ := payload.GetString("username")
		mu.Lock()
		seen = username
		mu.Unlock()
		return true, "alice-id"
	})
	if err != nil {
		t.Fatal(err)
	}

	client, err := NewTCPClient("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	connectParams, _ := New("")
	defer connectParams.Close()
	must(t, connectParams.SetString("username", "alice"))
	if err := client.Connect(connectParams); err != nil {
		t.Fatal(err)
	}

	mu.Lock()
	got := seen
	mu.Unlock()
	if got != "alice" {
		t.Fatalf("seen = %q", got)
	}
}

func TestTCPRejectingCallbackFailsConnect(t *testing.T) {
	rmiTestLock.Lock()
	defer rmiTestLock.Unlock()

	port := allocPort()
	server, err := NewTCPServer("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	err = server.Listen(nil, func(payload *Dynamic) (bool, string) {
		return false, ""
	})
	if err != nil {
		t.Fatal(err)
	}

	client, err := NewTCPClient("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	if err := client.Connect(nil); err == nil {
		t.Fatal("expected connect to fail")
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Tracing (free functions)
// ═════════════════════════════════════════════════════════════════════════

func TestTracingFreeFunctionsDoNotPanic(t *testing.T) {
	// No active profiling recorder in these tests, so these are no-ops --
	// this just exercises the cgo plumbing end-to-end.
	TraceScopeBegin("test-scope")
	TraceInstant("test-instant")
	TraceCounterInt("test-counter-int", 1)
	TraceCounterDouble("test-counter-float", 1.5)
	TraceScopeEnd()
	_ = TraceIsActive()
}
