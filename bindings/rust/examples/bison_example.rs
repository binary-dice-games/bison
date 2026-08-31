// MIT License © 2025 Binary Dice Games
//! Detailed, runnable examples for the Bison dynamic-object Rust binding.
//!
//! Mirrors `bindings/python/examples/bison_example.py` feature-for-feature.
//!
//! Run with: `cargo run --example bison_example`

use std::sync::atomic::{AtomicU32, Ordering};

use bison::dynamic::{
    add_class, clear_registry, find_class, from_json, from_yaml, instantiate, key, Dynamic,
};

static SECTION_INDEX: AtomicU32 = AtomicU32::new(0);

fn section(title: &str) {
    let n = SECTION_INDEX.fetch_add(1, Ordering::Relaxed) + 1;
    println!("\n==========================================");
    println!("  {n}. {title}");
    println!("==========================================");
}

// ── Example 1: Hashing and keys ─────────────────────────────────────────────

fn example_hashing() {
    section("Hashing and keys");
    let k1 = key("velocity");
    let k2 = key("velocity");
    println!("key(\"velocity\") is stable: {}", k1 == k2);
    println!("High bit set on named key: {}", (k1 & 0x8000_0000) != 0);
    println!("\"velocity\" != \"score\": {}", k1 != key("score"));
}

// ── Example 2: Scalar field get / set ───────────────────────────────────────

fn example_scalar_fields() {
    section("Scalar field get / set");
    let mut h = Dynamic::new("Person");
    h.set("name", "Alice").unwrap();
    h.set("age", 30).unwrap();
    h.set("score", 9.5f32).unwrap();
    h.set("active", true).unwrap();

    println!("name   : {}", h.get_string("name").unwrap());
    println!("age    : {}", h.get_int("age").unwrap());
    println!("score  : {}", h.get_float("score").unwrap());
    println!("active : {}", h.get_bool("active").unwrap());
}

// ── Example 3: Nested objects ───────────────────────────────────────────────

fn example_nested_objects() {
    section("Nested objects");
    let mut person = Dynamic::new("Person");
    person.set("name", "Alice").unwrap();

    let mut address = Dynamic::default();
    address.set("street", "123 Main St").unwrap();
    address.set("city", "Springfield").unwrap();

    // set_object increments address's ref-count; the local `address` value
    // still owns its own independent handle and is dropped normally below.
    person.set_object("address", Some(&address)).unwrap();

    let addr_out = person.get_object("address").unwrap().unwrap();
    println!("city: {}", addr_out.get_string("city").unwrap());
}

// ── Example 4: Numeric (array-like) indexing ────────────────────────────────

fn example_numeric_indexing() {
    section("Numeric (array-like) indexing");
    let mut lst = Dynamic::default();
    lst.set_at(0, "red").unwrap();
    lst.set_at(1, "green").unwrap();
    lst.set_at(2, "blue").unwrap();
    println!("list size : {}", lst.size());
    println!("list[1]   : {}", lst.get_string_at(1).unwrap());

    let mut scores = Dynamic::default();
    scores.set_at(0, 10).unwrap();
    scores.set_at(1, 20).unwrap();
    scores.set_at(2, 30).unwrap();
    println!("scores[2] : {}", scores.get_int_at(2).unwrap());

    match scores.set_at(0, 1.1f32) {
        Ok(()) => println!("type mismatch at[0]: unexpected (no error raised)"),
        Err(e) => println!("type mismatch at[0]: BISON_ERR_TYPE (expected) -> {e}"),
    }

    let mut fscores = Dynamic::default();
    fscores.set_at(0, 1.1f32).unwrap();
    fscores.set_at(1, 2.2f32).unwrap();
    println!("fscores[0]: {}", fscores.get_float_at(0).unwrap());
}

// ── Example 5: Methods — attaching behaviour to objects ─────────────────────

fn example_methods() {
    section("Methods - attaching behaviour to objects");
    let mut calc = Dynamic::new("Calculator");
    calc.set("total", 0).unwrap();
    calc.add_method("add", |_self_obj, params, result| {
        let a = params.get_int("a").unwrap();
        let b = params.get_int("b").unwrap();
        result.set("value", a + b).unwrap();
    })
    .unwrap();
    calc.add_method("accumulate", |self_obj, params, result| {
        let total = self_obj.get_int("total").unwrap() + params.get_int("n").unwrap();
        result.set("total", total).unwrap();
    })
    .unwrap();

    let mut args = Dynamic::default();
    args.set("a", 10).unwrap();
    args.set("b", 32).unwrap();
    let total = calc.call("add", &args).unwrap();
    println!("10 + 32 = {}", total.get_int("value").unwrap());

    // `accumulate` above reads self_obj.total but the callback receives a
    // *non-owning view* of self, so mutating self.total there wouldn't
    // persist -- mirrors the other bindings' method-callback semantics
    // (the callback populates `result` in place). Drive the running total
    // from the caller side instead.
    let mut running = 0;
    for i in 1..=5 {
        running += i;
        calc.set("total", running).unwrap();
        let mut p = Dynamic::default();
        p.set("n", i).unwrap();
        let _ = calc.call("accumulate", &p).unwrap();
    }
    println!(
        "accumulated total (1+2+3+4+5): {}",
        calc.get_int("total").unwrap()
    );

    match calc.method_attributes("sqrt") {
        Ok(_) => println!("attribute access to unknown method: unexpected (no error raised)"),
        Err(e) => println!("attribute access to unknown method: not found (expected) -> {e}"),
    }

    let empty = Dynamic::default();
    match calc.call("sqrt", &empty) {
        Ok(_) => println!("call unknown method: unexpected (no error raised)"),
        Err(e) => println!("call unknown method: BISON_ERR_NOT_FOUND (expected) -> {e}"),
    }
}

// ── Example 6: Class hierarchy and inheritance ──────────────────────────────

fn example_inheritance() {
    section("Class hierarchy and inheritance");
    clear_registry();

    let mut shape = Dynamic::new("Shape");
    shape.set("color", "black").unwrap();
    shape
        .add_method("describe", |self_obj, _params, result| {
            let color = self_obj.get_string("color").unwrap();
            result.set("text", format!("{color} shape")).unwrap();
        })
        .unwrap();
    add_class(shape, "", "", None).unwrap();

    let mut circle = Dynamic::new("Circle");
    circle.set("radius", 1.0f32).unwrap();
    circle
        .add_method("area", |self_obj, _params, result| {
            let r = self_obj.get_float("radius").unwrap();
            result.set("area", std::f32::consts::PI * r * r).unwrap();
        })
        .unwrap();
    add_class(circle, "Shape", "", None).unwrap();

    let mut c = instantiate("Circle", "").unwrap();
    c.set("radius", 5.0f32).unwrap();
    c.set("color", "red").unwrap(); // overrides the inherited default

    let empty = Dynamic::default();
    let area_result = c.call("area", &empty).unwrap();
    println!(
        "Circle area (r=5): {:.4}",
        area_result.get_float("area").unwrap()
    );

    let desc_result = c.call("describe", &empty).unwrap();
    println!("Description: {}", desc_result.get_string("text").unwrap());

    let c2 = instantiate("Circle", "").unwrap();
    println!("Inherited color: {}", c2.get_string("color").unwrap());

    let dup = Dynamic::new("Shape");
    match add_class(dup, "", "", None) {
        Ok(()) => println!("Duplicate addClass rejected: false (unexpected)"),
        Err(_) => println!("Duplicate addClass rejected: true"),
    }

    let found = find_class("Shape", "");
    println!("Shape found in registry: {}", found.is_some()); // non-owning; do not release

    clear_registry();
}

// ── Example 7: Namespaces ───────────────────────────────────────────────────

fn example_namespaces() {
    section("Namespaces - class isolation by unit");
    clear_registry();

    let mut math_table = Dynamic::new("table");
    math_table.set("rows", 0).unwrap();
    math_table.set("cols", 0).unwrap();
    add_class(math_table, "", "math", None).unwrap();

    let mut ikea_table = Dynamic::new("table");
    ikea_table.set("legs", 4).unwrap();
    ikea_table.set("material", "wood").unwrap();
    add_class(ikea_table, "", "ikea", None).unwrap();

    println!("Registered 'table' in both 'math' and 'ikea' namespaces");

    let mut mt = instantiate("table", "math").unwrap();
    mt.set("rows", 10).unwrap();
    mt.set("cols", 5).unwrap();

    let mut it = instantiate("table", "ikea").unwrap();
    it.set("legs", 4).unwrap();
    it.set("material", "oak").unwrap();

    println!(
        "math::table rows={} cols={}",
        mt.get_int("rows").unwrap(),
        mt.get_int("cols").unwrap()
    );
    println!(
        "ikea::table legs={} material={}",
        it.get_int("legs").unwrap(),
        it.get_string("material").unwrap()
    );

    let mut furniture = Dynamic::new("Furniture");
    furniture.set("warranty", 5).unwrap();
    add_class(furniture, "", "ikea", None).unwrap();

    let mut sofa = Dynamic::new("Sofa");
    sofa.set("seats", 3).unwrap();
    add_class(sofa, "Furniture", "ikea", None).unwrap();

    let s = instantiate("Sofa", "ikea").unwrap();
    println!(
        "ikea::Sofa seats={} warranty={}",
        s.get_int("seats").unwrap(),
        s.get_int("warranty").unwrap()
    );

    clear_registry();
}

// ── Example 8: JSON import ──────────────────────────────────────────────────

fn example_json() {
    section("JSON import");
    let mut obj = from_json(
        r#"
        {
          "name":   "Alice",
          "age":    30,
          "score":  9.5,
          "active": true,
          "tags":   ["c++", "bison", "serialization"],
          "address": {"city": "Springfield", "zip": 12345}
        }
        "#,
    )
    .unwrap();
    println!("name   : {}", obj.get_string("name").unwrap());
    println!("age    : {}", obj.get_int("age").unwrap());
    println!("active : {}", obj.get_bool("active").unwrap());
    println!("score  : {}", obj.get_float("score").unwrap());

    let addr = obj.get_object("address").unwrap().unwrap();
    println!("city   : {}", addr.get_string("city").unwrap());

    let tags = obj.get_object("tags").unwrap().unwrap();
    println!("tags[0]: {}", tags.get_string_at(0).unwrap());
    println!("tags[2]: {}", tags.get_string_at(2).unwrap());
    println!("tag count: {}", tags.size());

    obj.set("name", "Bob").unwrap();
    println!("updated name: {}", obj.get_string("name").unwrap());
}

// ── Example 9: YAML import ───────────────────────────────────────────────────

fn example_yaml() {
    section("YAML import");
    let obj = from_yaml(
        "server:\n  host: localhost\n  port: 8080\ndebug: true\nthreshold: 0.75\ntags:\n  - yaml\n  - bison\n  - example\n",
    )
    .unwrap();

    let server = obj.get_object("server").unwrap().unwrap();
    println!("host      : {}", server.get_string("host").unwrap());
    println!("port      : {}", server.get_int("port").unwrap());

    println!("debug     : {}", obj.get_bool("debug").unwrap());
    println!("threshold : {:.2}", obj.get_float("threshold").unwrap());

    let tags = obj.get_object("tags").unwrap().unwrap();
    println!("tags[0]   : {}", tags.get_string_at(0).unwrap());
    println!("tags[2]   : {}", tags.get_string_at(2).unwrap());
    println!("tag count : {}", tags.size());
}

// ── Example 10: Reference counting and add_ref ──────────────────────────────

fn example_ref_counting() {
    section("Reference counting and add_ref");
    let mut h = Dynamic::default();
    h.set("x", 42).unwrap();

    let mut alias = h.add_ref();
    println!("alias sees x = {}", alias.get_int("x").unwrap());

    alias.set("x", 99).unwrap();
    println!(
        "original after alias mutate: x = {}",
        h.get_int("x").unwrap()
    );

    drop(alias);
    println!(
        "original after alias release: x = {}",
        h.get_int("x").unwrap()
    );

    drop(h);
    println!("Both handles released");
}

fn main() {
    example_hashing();
    example_scalar_fields();
    example_nested_objects();
    example_numeric_indexing();
    example_methods();
    example_inheritance();
    example_namespaces();
    example_json();
    example_yaml();
    example_ref_counting();
    println!("\nAll examples completed successfully.");
}
