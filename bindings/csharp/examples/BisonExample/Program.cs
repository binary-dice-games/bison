// MIT License © 2025 Binary Dice Games
/**
 * @file Program.cs
 * @brief Detailed, runnable examples for the Bison dynamic-object C# binding.
 *
 * Mirrors bindings/python/examples/bison_example.py feature-for-feature,
 * using the same three access styles the binding supports: `dynamic`
 * attribute access (`d.Field = value`), the indexer (`d["field"] = value`),
 * and the fully-typed API (`d.Call(...)`, `d.AddField(...)`).
 *
 * Run with:  dotnet run --project bindings/csharp/examples/BisonExample
 */

using Bdg.Bison;

var sectionIndex = 0;

void Section(string title)
{
    sectionIndex++;
    Console.WriteLine();
    Console.WriteLine("==========================================");
    Console.WriteLine($"  {sectionIndex}. {title}");
    Console.WriteLine("==========================================");
}

// ── Example 1: Hashing and keys ─────────────────────────────────────────────

void ExampleHashing()
{
    Section("Hashing and keys");
    var k1 = Key.Of("velocity");
    var k2 = Key.Of("velocity");
    Console.WriteLine($"Key.Of(\"velocity\") is stable: {k1 == k2}");
    Console.WriteLine($"High bit set on named key: {(k1 & 0x80000000) != 0}");
    Console.WriteLine($"\"velocity\" != \"score\": {k1 != Key.Of("score")}");
}

// ── Example 2: Scalar field get / set ───────────────────────────────────────

void ExampleScalarFields()
{
    Section("Scalar field get / set");
    dynamic h = new Dynamic("Person");
    try
    {
        h.name = "Alice";
        h.age = 30;
        h.score = 9.5f;
        h.active = true;

        Console.WriteLine($"name   : {h.name}");
        Console.WriteLine($"age    : {h.age}");
        Console.WriteLine($"score  : {h.score}");
        Console.WriteLine($"active : {h.active}");
    }
    finally
    {
        ((Dynamic)h).Release();
    }
}

// ── Example 3: Nested objects ────────────────────────────────────────────────

void ExampleNestedObjects()
{
    Section("Nested objects");
    var person = new Dynamic("Person");
    person["name"] = "Alice";

    var address = new Dynamic();
    address["street"] = "123 Main St";
    address["city"] = "Springfield";

    person["address"] = address; // bison_set_object increments address's ref-count
    address.Release(); // we can release our own copy

    var addrOut = (Dynamic)person["address"]!;
    Console.WriteLine($"city: {addrOut["city"]}");
    addrOut.Release();
    person.Release();
}

// ── Example 4: Numeric (array-like) indexing ─────────────────────────────────

void ExampleNumericIndexing()
{
    Section("Numeric (array-like) indexing");
    using (var list = new Dynamic())
    {
        list[0] = "red";
        list[1] = "green";
        list[2] = "blue";
        Console.WriteLine($"list size : {list.Size}");
        Console.WriteLine($"list[1]   : {list[1]}");
    }

    using (var scores = new Dynamic())
    {
        scores[0] = 10;
        scores[1] = 20;
        scores[2] = 30;
        Console.WriteLine($"scores[2] : {scores[2]}");

        try
        {
            scores[0] = 1.1f; // type-locked: int slot rejects float
            Console.WriteLine("type mismatch at[0]: unexpected (no error raised)");
        }
        catch (BisonException e)
        {
            Console.WriteLine($"type mismatch at[0]: BISON_ERR_TYPE (expected) -> {e.Message}");
        }
    }

    using var fscores = new Dynamic();
    fscores[0] = 1.1f;
    fscores[1] = 2.2f;
    Console.WriteLine($"fscores[0]: {fscores[0]}");
}

// ── Example 5: Methods — attaching behaviour to objects ─────────────────────

void ExampleMethods()
{
    Section("Methods - attaching behaviour to objects");
    var calc = new Dynamic("Calculator");
    calc["total"] = 0;
    calc.AddMethod("add", (self, p, result) => result["value"] = (int)p["a"]! + (int)p["b"]!);
    calc.AddMethod("accumulate", (self, p, result) =>
    {
        var total = (int)self["total"]! + (int)p["n"]!;
        self["total"] = total;
        result["total"] = total;
    });

    var total = calc.Call("add", new Dictionary<string, object?> { ["a"] = 10, ["b"] = 32 });
    Console.WriteLine($"10 + 32 = {total["value"]}");
    total.Release();

    for (var i = 1; i <= 5; i++)
    {
        var res = calc.Call("accumulate", new Dictionary<string, object?> { ["n"] = i });
        res.Release();
    }

    Console.WriteLine($"accumulated total (1+2+3+4+5): {calc["total"]}");

    try
    {
        calc.Call("sqrt");
        Console.WriteLine("call unknown method: unexpected (no error raised)");
    }
    catch (BisonException e)
    {
        Console.WriteLine($"call unknown method: BISON_ERR_NOT_FOUND (expected) -> {e.Message}");
    }

    calc.Release();
}

// ── Example 6: Class hierarchy and inheritance ───────────────────────────────

void ExampleInheritance()
{
    Section("Class hierarchy and inheritance");
    Dynamic.ClearRegistry();

    var shape = new Dynamic("Shape");
    shape["color"] = "black";
    shape.AddMethod("describe", (self, p, result) => result["text"] = $"{self["color"]} shape");
    Dynamic.AddClass(shape);
    shape.Release();

    var circle = new Dynamic("Circle");
    circle["radius"] = 1.0f;
    circle.AddMethod("area", (self, p, result) =>
    {
        var r = (float)self["radius"]!;
        result["area"] = 3.14159265f * r * r;
    });
    Dynamic.AddClass(circle, parentName: "Shape");
    circle.Release();

    var c = Dynamic.Instantiate("Circle");
    c["radius"] = 5.0f;
    c["color"] = "red"; // overrides the inherited default

    var areaResult = c.Call("area");
    Console.WriteLine($"Circle area (r=5): {(float)areaResult["area"]!:F4}");
    areaResult.Release();

    var descResult = c.Call("describe");
    Console.WriteLine($"Description: {descResult["text"]}");
    descResult.Release();

    var c2 = Dynamic.Instantiate("Circle");
    Console.WriteLine($"Inherited color: {c2["color"]}");
    c2.Release();

    var dup = new Dynamic("Shape");
    try
    {
        Dynamic.AddClass(dup);
        Console.WriteLine("Duplicate AddClass rejected: False (unexpected)");
    }
    catch (BisonException)
    {
        Console.WriteLine("Duplicate AddClass rejected: True");
    }
    dup.Release();

    var found = Dynamic.FindClass("Shape");
    Console.WriteLine($"Shape found in registry: {found is not null}"); // non-owning; do not release

    c.Release();
    Dynamic.ClearRegistry();
}

// ── Example 7: Namespaces ────────────────────────────────────────────────────

void ExampleNamespaces()
{
    Section("Namespaces - class isolation by unit");
    Dynamic.ClearRegistry();

    var mathTable = new Dynamic("table");
    mathTable["rows"] = 0;
    mathTable["cols"] = 0;
    Dynamic.AddClass(mathTable, nsName: "math");
    mathTable.Release();

    var ikeaTable = new Dynamic("table");
    ikeaTable["legs"] = 4;
    ikeaTable["material"] = "wood";
    Dynamic.AddClass(ikeaTable, nsName: "ikea");
    ikeaTable.Release();

    Console.WriteLine("Registered 'table' in both 'math' and 'ikea' namespaces");

    var mt = Dynamic.Instantiate("table", nsName: "math");
    mt["rows"] = 10;
    mt["cols"] = 5;

    var it = Dynamic.Instantiate("table", nsName: "ikea");
    it["legs"] = 4;
    it["material"] = "oak";

    Console.WriteLine($"math::table rows={mt["rows"]} cols={mt["cols"]}");
    Console.WriteLine($"ikea::table legs={it["legs"]} material={it["material"]}");

    var furniture = new Dynamic("Furniture");
    furniture["warranty"] = 5;
    Dynamic.AddClass(furniture, nsName: "ikea");
    furniture.Release();

    var sofa = new Dynamic("Sofa");
    sofa["seats"] = 3;
    Dynamic.AddClass(sofa, parentName: "Furniture", nsName: "ikea");
    sofa.Release();

    var s = Dynamic.Instantiate("Sofa", nsName: "ikea");
    Console.WriteLine($"ikea::Sofa seats={s["seats"]} warranty={s["warranty"]}");

    s.Release();
    mt.Release();
    it.Release();
    Dynamic.ClearRegistry();
}

// ── Example 8: JSON import ────────────────────────────────────────────────────

void ExampleJson()
{
    Section("JSON import");
    var obj = Dynamic.FromJson("""
        {
          "name":   "Alice",
          "age":    30,
          "score":  9.5,
          "active": true,
          "tags":   ["c++", "bison", "serialization"],
          "address": {"city": "Springfield", "zip": 12345}
        }
        """);
    Console.WriteLine($"name   : {obj["name"]}");
    Console.WriteLine($"age    : {obj["age"]}");
    Console.WriteLine($"active : {obj["active"]}");
    Console.WriteLine($"score  : {obj["score"]}");

    var addr = (Dynamic)obj["address"]!;
    Console.WriteLine($"city   : {addr["city"]}");
    addr.Release();

    var tags = (Dynamic)obj["tags"]!;
    Console.WriteLine($"tags[0]: {tags[0]}");
    Console.WriteLine($"tags[2]: {tags[2]}");
    Console.WriteLine($"tag count: {tags.Size}");
    tags.Release();

    obj["name"] = "Bob";
    Console.WriteLine($"updated name: {obj["name"]}");

    obj.Release();
}

// ── Example 9: YAML import ───────────────────────────────────────────────────

void ExampleYaml()
{
    Section("YAML import");
    var obj = Dynamic.FromYaml(
        "server:\n" +
        "  host: localhost\n" +
        "  port: 8080\n" +
        "debug: true\n" +
        "threshold: 0.75\n" +
        "tags:\n" +
        "  - yaml\n" +
        "  - bison\n" +
        "  - example\n");

    var server = (Dynamic)obj["server"]!;
    Console.WriteLine($"host      : {server["host"]}");
    Console.WriteLine($"port      : {server["port"]}");
    server.Release();

    Console.WriteLine($"debug     : {obj["debug"]}");
    Console.WriteLine($"threshold : {(float)obj["threshold"]!:F2}");

    var tags = (Dynamic)obj["tags"]!;
    Console.WriteLine($"tags[0]   : {tags[0]}");
    Console.WriteLine($"tags[2]   : {tags[2]}");
    Console.WriteLine($"tag count : {tags.Size}");
    tags.Release();

    obj.Release();
}

// ── Example 10: Reference counting and AddRef ────────────────────────────────

void ExampleRefCounting()
{
    Section("Reference counting and AddRef");
    var h = new Dynamic();
    h["x"] = 42;

    var alias = h.AddRef();
    Console.WriteLine($"alias sees x = {alias["x"]}");

    alias["x"] = 99;
    Console.WriteLine($"original after alias mutate: x = {h["x"]}");

    alias.Release();
    Console.WriteLine($"original after alias release: x = {h["x"]}");

    h.Release();
    Console.WriteLine("Both handles released");
}

ExampleHashing();
ExampleScalarFields();
ExampleNestedObjects();
ExampleNumericIndexing();
ExampleMethods();
ExampleInheritance();
ExampleNamespaces();
ExampleJson();
ExampleYaml();
ExampleRefCounting();
Console.WriteLine();
Console.WriteLine("All examples completed successfully.");
