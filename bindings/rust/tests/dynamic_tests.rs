// MIT License © 2025 Binary Dice Games
//! Tests for `bison::dynamic` -- the Bison dynamic-object Rust binding.
//!
//! Mirrors `bindings/python/tests/test_dynamic.py`'s coverage. Run with:
//! `cargo test --test dynamic_tests`.

use std::sync::Mutex;

use bison::dynamic::{
    add_class, class_attributes, clear_registry, deserialize, find_class, from_json, from_yaml,
    instantiate, key, Attributes, BisonError, Dynamic, Value,
};

// `clear_registry()`/`add_class()` touch a process-wide C registry shared by
// every test in this binary; `cargo test` runs tests concurrently on
// multiple threads by default, so registry-touching tests must serialize
// against each other or they race each other's classes.
static REGISTRY_LOCK: Mutex<()> = Mutex::new(());

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn create_succeeds() {
    let _obj = Dynamic::default();
}

#[test]
fn add_ref_shares_mutations() {
    let mut obj = Dynamic::default();
    obj.set("n", 10).unwrap();
    let mut r#ref = obj.add_ref();
    obj.set("n", 20).unwrap();
    assert_eq!(r#ref.get_int("n").unwrap(), 20);
    // Explicit drop to make the shared-ownership window clear; not required.
    drop(obj);
    r#ref.set("n", 30).unwrap();
    assert_eq!(r#ref.get_int("n").unwrap(), 30);
}

#[test]
fn clone_is_independent() {
    let mut obj = Dynamic::default();
    obj.set("n", 1).unwrap();
    let mut clone = obj.clone();
    clone.set("n", 2).unwrap();
    assert_eq!(obj.get_int("n").unwrap(), 1);
    assert_eq!(clone.get_int("n").unwrap(), 2);
}

// ═════════════════════════════════════════════════════════════════════════════
// Field access
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn scalar_round_trip() {
    let mut obj = Dynamic::default();
    obj.set("score", 42).unwrap();
    obj.set("speed", 9.5f32).unwrap();
    obj.set("alive", true).unwrap();
    obj.set("name", "hero").unwrap();

    assert_eq!(obj.get_int("score").unwrap(), 42);
    assert!((obj.get_float("speed").unwrap() - 9.5).abs() < 1e-6);
    assert!(obj.get_bool("alive").unwrap());
    assert_eq!(obj.get_string("name").unwrap(), "hero");
}

#[test]
fn type_locked_field_raises() {
    let mut obj = Dynamic::default();
    obj.set("score", 1).unwrap();
    let err = obj.set("score", 1.5f32).unwrap_err();
    assert_eq!(err.code, -2); // BISON_ERR_TYPE
}

#[test]
fn nested_object() {
    let mut obj = Dynamic::default();
    let mut child = Dynamic::default();
    child.set("city", "Springfield").unwrap();
    obj.set_object("address", Some(&child)).unwrap();

    let addr = obj.get_object("address").unwrap().unwrap();
    assert_eq!(addr.get_string("city").unwrap(), "Springfield");
}

#[test]
fn indexed_fields_and_size() {
    let mut obj = Dynamic::default();
    obj.set_at(0, "red").unwrap();
    obj.set_at(1, "green").unwrap();
    obj.set_at(2, "blue").unwrap();
    assert_eq!(obj.size(), 3);
    assert_eq!(obj.get_string_at(1).unwrap(), "green");

    let values: Vec<String> = obj
        .iter()
        .map(|v| match v.unwrap() {
            Value::Str(s) => s,
            other => panic!("expected Str, got {other:?}"),
        })
        .collect();
    assert_eq!(values, vec!["red", "green", "blue"]);
}

#[test]
fn indexed_type_lock() {
    let mut obj = Dynamic::default();
    obj.set_at(0, 10).unwrap();
    assert!(obj.set_at(0, 1.5f32).is_err());
}

#[test]
fn indexed_bool_round_trips_as_bool() {
    let mut obj = Dynamic::default();
    obj.set_at(0, true).unwrap();
    obj.set_at(1, false).unwrap();
    assert!(matches!(obj.get_at(0).unwrap(), Value::Bool(true)));
    assert!(matches!(obj.get_at(1).unwrap(), Value::Bool(false)));
}

#[test]
fn indexed_bool_type_locked() {
    let mut obj = Dynamic::default();
    obj.set_at(0, true).unwrap();
    assert!(obj.set_at(0, 5).is_err());
}

#[test]
fn indexed_object_round_trip() {
    let mut obj = Dynamic::default();
    let mut child = Dynamic::default();
    child.set("v", 9).unwrap();
    obj.set_object_at(0, Some(&child)).unwrap();

    let out = obj.get_object_at(0).unwrap().unwrap();
    assert_eq!(out.get_int("v").unwrap(), 9);
}

#[test]
fn indexed_null_object_round_trip() {
    let mut obj = Dynamic::default();
    obj.set_object_at(0, None).unwrap();
    assert!(obj.get_object_at(0).unwrap().is_none());
}

#[test]
fn set_key_at_round_trips() {
    let mut obj = Dynamic::default();
    obj.set_key_at(0, key("hero")).unwrap();
    assert_eq!(obj.get_key_at(0).unwrap(), key("hero"));
}

// ═════════════════════════════════════════════════════════════════════════════
// key_t-typed field access
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn key_round_trips_with_hashed_value() {
    let mut obj = Dynamic::default();
    obj.set_key("id", key("hero")).unwrap();
    assert_eq!(obj.get_key("id").unwrap(), key("hero"));
}

#[test]
fn key_distinct_from_int32_field() {
    let mut obj = Dynamic::default();
    obj.set("plain_int", 42).unwrap();
    let err = obj.set_key("plain_int", key("anything")).unwrap_err();
    assert_eq!(err.code, -2); // BISON_ERR_TYPE
}

#[test]
fn getitem_falls_back_to_key_after_other_types_fail() {
    let mut obj = Dynamic::default();
    obj.set_key("selector", key("nav_mode_topdown")).unwrap();
    match obj.get("selector").unwrap() {
        Value::Key(k) => assert_eq!(k, key("nav_mode_topdown")),
        other => panic!("expected Value::Key, got {other:?}"),
    }
}

#[test]
fn add_field_key_declares_and_rejects_duplicate() {
    let mut obj = Dynamic::default();
    obj.add_field_key("id", key("hero"), None).unwrap();
    assert_eq!(obj.get_key("id").unwrap(), key("hero"));
    let err = obj.add_field_key("id", key("other"), None).unwrap_err();
    assert_eq!(err.code, -4); // BISON_ERR_DUPLICATE
}

// ═════════════════════════════════════════════════════════════════════════════
// Vector fields
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn vector_int_round_trip() {
    let mut obj = Dynamic::default();
    obj.set("ints", vec![1, 2, 3]).unwrap();
    assert_eq!(obj.get_vector_int("ints").unwrap(), vec![1, 2, 3]);
}

#[test]
fn vector_bool_round_trip() {
    let mut obj = Dynamic::default();
    obj.set("flags", vec![true, false, true]).unwrap();
    assert_eq!(
        obj.get_vector_bool("flags").unwrap(),
        vec![true, false, true]
    );
}

#[test]
fn vector_float_round_trip() {
    let mut obj = Dynamic::default();
    obj.set("ratios", vec![1.5f32, 2.5f32]).unwrap();
    assert_eq!(obj.get_vector_float("ratios").unwrap(), vec![1.5, 2.5]);
}

#[test]
fn vector_bytes_round_trip() {
    let mut obj = Dynamic::default();
    obj.set("blob", vec![0u8, 1, 255]).unwrap();
    assert_eq!(obj.get_vector_bytes("blob").unwrap(), vec![0u8, 1, 255]);
}

#[test]
fn vector_assignment_replaces_existing_contents() {
    let mut obj = Dynamic::default();
    obj.set("ints", vec![1, 2, 3]).unwrap();
    obj.set("ints", vec![9, 9]).unwrap();
    assert_eq!(obj.get_vector_int("ints").unwrap(), vec![9, 9]);
}

#[test]
fn vector_empty_round_trips() {
    let mut obj = Dynamic::default();
    obj.set("empty", Vec::<i32>::new()).unwrap();
    assert_eq!(obj.get_vector_int("empty").unwrap(), Vec::<i32>::new());
}

#[test]
fn add_field_vector_registers_and_is_readable() {
    let mut obj = Dynamic::default();
    obj.add_field("ints", vec![1, 2, 3], None).unwrap();
    assert_eq!(obj.get_vector_int("ints").unwrap(), vec![1, 2, 3]);
}

#[test]
fn add_field_vector_rejects_duplicate() {
    let mut obj = Dynamic::default();
    obj.add_field("ints", vec![1, 2, 3], None).unwrap();
    let err = obj.add_field("ints", vec![9], None).unwrap_err();
    assert_eq!(err.code, -4); // BISON_ERR_DUPLICATE
}

#[test]
fn add_field_bytes() {
    let mut obj = Dynamic::default();
    obj.add_field("blob", vec![1u8, 2, 3], None).unwrap();
    assert_eq!(obj.get_vector_bytes("blob").unwrap(), vec![1u8, 2, 3]);
}

// ═════════════════════════════════════════════════════════════════════════════
// Methods
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn add_method_and_call() {
    let mut calc = Dynamic::default();
    calc.add_method("add", |_self_obj, params, result| {
        let v = params.get_int("a").unwrap() + params.get_int("b").unwrap();
        result.set("value", v).unwrap();
    })
    .unwrap();

    let mut args = Dynamic::default();
    args.set("a", 10).unwrap();
    args.set("b", 32).unwrap();
    let out = calc.call("add", &args).unwrap();
    assert_eq!(out.get_int("value").unwrap(), 42);
}

#[test]
fn call_unknown_method_raises() {
    let obj = Dynamic::default();
    let empty = Dynamic::default();
    let err = obj.call("nope", &empty).unwrap_err();
    assert_eq!(err.code, -3); // BISON_ERR_NOT_FOUND
}

// ═════════════════════════════════════════════════════════════════════════════
// Class registry / inheritance
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn class_inheritance() {
    let _guard = REGISTRY_LOCK.lock().unwrap();
    clear_registry();
    let mut shape = Dynamic::new("Shape_rs_inherit");
    shape.set("color", "black").unwrap();
    add_class(shape, "", "", None).unwrap();

    let mut circle = Dynamic::new("Circle_rs_inherit");
    circle.set("radius", 1.0f32).unwrap();
    add_class(circle, "Shape_rs_inherit", "", None).unwrap();

    let c = instantiate("Circle_rs_inherit", "").unwrap();
    assert_eq!(c.get_string("color").unwrap(), "black"); // inherited default
    assert!((c.get_float("radius").unwrap() - 1.0).abs() < 1e-6);
    clear_registry();
}

#[test]
fn duplicate_class_raises() {
    let _guard = REGISTRY_LOCK.lock().unwrap();
    clear_registry();
    let proto = Dynamic::new("Shape_rs_dup");
    add_class(proto, "", "", None).unwrap();

    let dup = Dynamic::new("Shape_rs_dup");
    let err = add_class(dup, "", "", None).unwrap_err();
    assert_eq!(err.code, -4); // BISON_ERR_DUPLICATE
    clear_registry();
}

#[test]
fn find_class_lookup() {
    let _guard = REGISTRY_LOCK.lock().unwrap();
    clear_registry();
    let proto = Dynamic::new("Shape_rs_find");
    add_class(proto, "", "", None).unwrap();
    assert!(find_class("Shape_rs_find", "").is_some());
    assert!(find_class("DoesNotExist_rs", "").is_none());
    clear_registry();
}

#[test]
fn namespaces_isolate_same_name() {
    let _guard = REGISTRY_LOCK.lock().unwrap();
    clear_registry();
    let mut math_table = Dynamic::new("table_rs");
    math_table.set("rows", 1).unwrap();
    add_class(math_table, "", "math_rs", None).unwrap();

    let mut ikea_table = Dynamic::new("table_rs");
    ikea_table.set("legs", 4).unwrap();
    add_class(ikea_table, "", "ikea_rs", None).unwrap();

    let mt = instantiate("table_rs", "math_rs").unwrap();
    let it = instantiate("table_rs", "ikea_rs").unwrap();
    assert_eq!(mt.get_int("rows").unwrap(), 1);
    assert_eq!(it.get_int("legs").unwrap(), 4);
    clear_registry();
}

#[test]
fn class_and_field_attributes() {
    let _guard = REGISTRY_LOCK.lock().unwrap();
    clear_registry();
    let mut proto = Dynamic::new("Widget_rs");
    proto
        .add_field(
            "count",
            0,
            Some(&Attributes {
                description: Some("a counter".to_string()),
                required: true,
                ..Default::default()
            }),
        )
        .unwrap();
    add_class(
        proto,
        "",
        "",
        Some(&Attributes {
            display_name: Some("Widget class".to_string()),
            ..Default::default()
        }),
    )
    .unwrap();

    let attrs = class_attributes("Widget_rs", "").unwrap();
    assert_eq!(attrs.display_name.as_deref(), Some("Widget class"));

    let w = instantiate("Widget_rs", "").unwrap();
    let field_attrs = w.field_attributes("count").unwrap();
    assert_eq!(field_attrs.description.as_deref(), Some("a counter"));
    assert!(field_attrs.required);
    clear_registry();
}

#[test]
fn registered_method_survives_prototype_drop() {
    // A class's methods must keep working after the registering prototype's
    // Rust value goes out of scope -- regression coverage for the
    // trampoline-lifetime concern `add_method`'s doc comment describes.
    let _guard = REGISTRY_LOCK.lock().unwrap();
    clear_registry();
    {
        let mut proto = Dynamic::new("Doubler_rs");
        proto
            .add_method("double", |_self_obj, params, result| {
                result
                    .set("value", params.get_int("n").unwrap() * 2)
                    .unwrap();
            })
            .unwrap();
        add_class(proto, "", "", None).unwrap();
        // `proto` (and its callback closure) goes out of scope here -- but
        // `add_class` moved it into a process-wide keep-alive list.
    }

    let inst = instantiate("Doubler_rs", "").unwrap();
    let mut args = Dynamic::default();
    args.set("n", 21).unwrap();
    let out = inst.call("double", &args).unwrap();
    assert_eq!(out.get_int("value").unwrap(), 42);
    clear_registry();
}

// ═════════════════════════════════════════════════════════════════════════════
// JSON / YAML import
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn from_json_parses() {
    let obj = from_json(r#"{"x": 1, "y": 2.5, "tags": ["a", "b"]}"#).unwrap();
    assert_eq!(obj.get_int("x").unwrap(), 1);
    assert!((obj.get_float("y").unwrap() - 2.5).abs() < 1e-6);
    let tags = obj.get_object("tags").unwrap().unwrap();
    assert_eq!(tags.get_string_at(0).unwrap(), "a");
    assert_eq!(tags.get_string_at(1).unwrap(), "b");
}

#[test]
fn from_yaml_parses() {
    let obj = from_yaml("x: 10\nname: test\n").unwrap();
    assert_eq!(obj.get_int("x").unwrap(), 10);
    assert_eq!(obj.get_string("name").unwrap(), "test");
}

#[test]
fn to_json_produces_valid_json() {
    let obj = from_json(r#"{"x": 1}"#).unwrap();
    let s = obj.to_json(-1).unwrap();
    assert!(s.contains('1'));
}

#[test]
fn invalid_json_raises() {
    assert!(from_json("not json").is_err());
}

// ═════════════════════════════════════════════════════════════════════════════
// Binary serialization
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn binary_round_trips_scalar_fields() {
    let mut obj = Dynamic::default();
    obj.set("x", 42).unwrap();
    obj.set("y", 2.5f32).unwrap();
    obj.set("s", "hello").unwrap();

    let buf = obj.serialize().unwrap();
    assert!(!buf.is_empty());

    let decoded = deserialize(&buf).unwrap();
    assert_eq!(decoded.get_int("x").unwrap(), 42);
    assert!((decoded.get_float("y").unwrap() - 2.5).abs() < 1e-6);
    assert_eq!(decoded.get_string("s").unwrap(), "hello");
}

#[test]
fn binary_round_trips_nested_object() {
    let mut obj = Dynamic::default();
    let mut child = Dynamic::default();
    child.set("city", "Springfield").unwrap();
    obj.set_object("address", Some(&child)).unwrap();

    let decoded = deserialize(&obj.serialize().unwrap()).unwrap();
    let addr = decoded.get_object("address").unwrap().unwrap();
    assert_eq!(addr.get_string("city").unwrap(), "Springfield");
}

#[test]
fn binary_malformed_buffer_raises() {
    let err: BisonError = deserialize(&[0xff, 0x00, 0x01]).unwrap_err();
    assert_eq!(err.code, -6); // BISON_ERR_PARSE
}

#[test]
fn binary_empty_object_round_trips() {
    let obj = Dynamic::default();
    let decoded = deserialize(&obj.serialize().unwrap()).unwrap();
    assert_eq!(decoded.size(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Key hashing
// ═════════════════════════════════════════════════════════════════════════════

#[test]
fn key_is_stable() {
    assert_eq!(key("velocity"), key("velocity"));
}

#[test]
fn key_high_bit_set() {
    assert!(key("velocity") & 0x8000_0000 != 0);
}

#[test]
fn key_different_names_differ() {
    assert_ne!(key("velocity"), key("score"));
}
