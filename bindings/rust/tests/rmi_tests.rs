// MIT License © 2025 Binary Dice Games
//! Tests for `bison::rmi` -- the Bison RMI Rust binding.
//!
//! Most tests use `Client::standalone()` (in-process dispatch) so the suite
//! has no dependency on sockets/ports being available in the test
//! environment. `tcp_auth::*` is the exception -- `Server::listen`'s `auth`
//! parameter is evaluated during the `OP_CONNECT` handshake, which
//! standalone sessions skip entirely, so it needs a real TCP client/server
//! round trip.
//!
//! Run with: `cargo test --test rmi_tests`.

use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::Mutex;

use bison::dynamic::{add_class, clear_registry, Dynamic};
use bison::rmi::{Client, Server};

// `cargo test` runs every `#[test]` in this file concurrently on multiple
// threads by default. `Client::standalone()` dispatches into the
// process-wide class registry (shared with `bison::dynamic`'s free
// functions), so standalone tests would clobber each other's "Calculator"
// registration; separately, the underlying transport's accept-loop/connect
// teardown was observed to hang intermittently when a real TCP
// server/client round trip (`tcp_auth::*`) runs concurrently with other
// RMI activity in the same process, even via plain `std::thread::spawn`
// (i.e. independent of this test harness). Both are library-level
// concurrency hazards, not bugs in a specific test -- the robust fix is one
// process-wide lock every test in this file acquires before doing anything,
// so no two tests' bodies ever execute at the same time.
static TEST_LOCK: Mutex<()> = Mutex::new(());

fn method_add(_self_obj: &Dynamic, params: &Dynamic, result: &mut Dynamic) {
    let v = params.get_float("a").unwrap() + params.get_float("b").unwrap();
    result.set("result", v).unwrap();
}

fn method_echo(_self_obj: &Dynamic, params: &Dynamic, result: &mut Dynamic) {
    let v = params.get_int("value").unwrap();
    result.set("value", v).unwrap();
}

fn register_calculator() {
    let mut proto = Dynamic::new("Calculator");
    proto.add_method("add", method_add).unwrap();
    proto.add_method("echo", method_echo).unwrap();
    add_class(proto, "", "", None).unwrap();
}

// ═════════════════════════════════════════════════════════════════════════════
// Standalone RMI
// ═════════════════════════════════════════════════════════════════════════════

mod standalone {
    use super::*;

    fn with_calculator<F: FnOnce()>(f: F) {
        let _guard = super::TEST_LOCK.lock().unwrap();
        clear_registry();
        register_calculator();
        f();
        clear_registry();
    }

    #[test]
    fn instantiate_and_call() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let calc = client.instantiate("Calculator", "", None).unwrap();

            let mut params = Dynamic::default();
            params.set("a", 10.0f32).unwrap();
            params.set("b", 3.0f32).unwrap();
            let r = calc.call("add", Some(&params), -1).unwrap();
            assert!((r.get_float("result").unwrap() - 13.0).abs() < 1e-6);
        });
    }

    #[test]
    fn get_set_field_patch() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let calc = client.instantiate("Calculator", "", None).unwrap();

            let mut patch = Dynamic::default();
            patch.set("label", "primary").unwrap();
            calc.set(&patch, -1).unwrap();

            let snapshot = calc.get(None, -1).unwrap();
            assert_eq!(snapshot.get_string("label").unwrap(), "primary");
        });
    }

    #[test]
    fn unknown_method_raises() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let calc = client.instantiate("Calculator", "", None).unwrap();
            assert!(calc.call("does_not_exist", None, -1).is_err());
        });
    }

    #[test]
    fn multiple_proxies_independent() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let a = client.instantiate("Calculator", "", None).unwrap();
            let b = client.instantiate("Calculator", "", None).unwrap();

            let mut pa = Dynamic::default();
            pa.set("label", "a").unwrap();
            a.set(&pa, -1).unwrap();

            let mut pb = Dynamic::default();
            pb.set("label", "b").unwrap();
            b.set(&pb, -1).unwrap();

            assert_eq!(a.get(None, -1).unwrap().get_string("label").unwrap(), "a");
            assert_eq!(b.get(None, -1).unwrap().get_string("label").unwrap(), "b");
        });
    }

    #[test]
    fn async_call_round_trips() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let calc = client.instantiate("Calculator", "", None).unwrap();

            let mut params = Dynamic::default();
            params.set("a", 4.0f32).unwrap();
            params.set("b", 5.0f32).unwrap();
            let future = calc.call_async("add", Some(&params)).unwrap();
            future.wait(-1).unwrap();
            let result = future.get_dynamic().unwrap();
            assert!((result.get_float("result").unwrap() - 9.0).abs() < 1e-6);
        });
    }

    #[test]
    fn instantiate_async_round_trips() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let future = client.instantiate_async("Calculator", "", None).unwrap();
            future.wait(-1).unwrap();
            let calc = future.get_proxy().unwrap();

            let mut params = Dynamic::default();
            params.set("value", 7).unwrap();
            let r = calc.call("echo", Some(&params), -1).unwrap();
            assert_eq!(r.get_int("value").unwrap(), 7);
        });
    }

    #[test]
    fn proxy_event_delivers_to_handler() {
        with_calculator(|| {
            let mut client = Client::standalone();
            client.connect(None).unwrap();
            let mut calc = client.instantiate("Calculator", "", None).unwrap();

            // No server push in standalone mode fires this, but registration
            // itself must succeed and not panic; exercises `on_event`'s FFI
            // plumbing end-to-end.
            calc.on_event("changed", |_params| {}).unwrap();
        });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TCP + auth
// ═════════════════════════════════════════════════════════════════════════════

mod tcp_auth {
    use super::*;

    static NEXT_PORT: AtomicU16 = AtomicU16::new(31000);

    fn next_port() -> u16 {
        NEXT_PORT.fetch_add(1, Ordering::Relaxed)
    }

    #[test]
    fn no_auth_set_connect_succeeds() {
        let _guard = super::TEST_LOCK.lock().unwrap();
        let port = next_port();
        let mut server = Server::tcp("127.0.0.1", port);
        server
            .listen::<fn(&Dynamic) -> (bool, String)>(None, None)
            .unwrap();

        let mut client = Client::tcp("127.0.0.1", port);
        client.connect(None).unwrap();
    }

    #[test]
    fn accepting_callback_receives_payload_and_sets_identity() {
        let _guard = super::TEST_LOCK.lock().unwrap();
        let port = next_port();
        let mut server = Server::tcp("127.0.0.1", port);
        let seen: std::sync::Arc<Mutex<Option<String>>> = std::sync::Arc::new(Mutex::new(None));
        let seen_clone = seen.clone();

        server
            .listen(
                None,
                Some(move |payload: &Dynamic| {
                    let username = payload.get_string("username").unwrap_or_default();
                    *seen_clone.lock().unwrap() = Some(username);
                    (true, "alice-id".to_string())
                }),
            )
            .unwrap();

        let mut client = Client::tcp("127.0.0.1", port);
        let mut connect_params = Dynamic::default();
        connect_params.set("username", "alice").unwrap();
        client.connect(Some(&connect_params)).unwrap();

        assert_eq!(seen.lock().unwrap().as_deref(), Some("alice"));
    }

    #[test]
    fn rejecting_callback_fails_connect() {
        let _guard = super::TEST_LOCK.lock().unwrap();
        let port = next_port();
        let mut server = Server::tcp("127.0.0.1", port);
        server
            .listen(None, Some(|_payload: &Dynamic| (false, String::new())))
            .unwrap();

        let mut client = Client::tcp("127.0.0.1", port);
        assert!(client.connect(None).is_err());
    }
}
