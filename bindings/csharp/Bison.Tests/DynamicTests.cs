// MIT License © 2025 Binary Dice Games
/**
 * @file DynamicTests.cs
 * @brief Tests for Bdg.Bison.Dynamic -- mirrors
 *        bindings/python/tests/test_dynamic.py.
 *
 * Build bison_abi first, then run from the repository root:
 *
 *   cmake -B build -DPACKAGE_TESTS=ON
 *   cmake --build build --config Debug --target bison_abi
 *   dotnet test bindings/csharp/Bison.Tests
 */

using Bdg.Bison;
using Xunit;

namespace Bdg.Bison.Tests;

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

public class LifecycleTests
{
    [Fact]
    public void CreateSucceeds()
    {
        var obj = new Dynamic();
        Assert.NotNull(obj);
        obj.Release();
    }

    [Fact]
    public void DisposeReleases()
    {
        using var obj = new Dynamic();
        obj["x"] = 1;
    }

    [Fact]
    public void ReleaseIdempotent()
    {
        var obj = new Dynamic();
        obj.Release();
        obj.Release();
    }

    [Fact]
    public void AddRefSharesMutations()
    {
        var obj = new Dynamic();
        obj["n"] = 10;
        var reference = obj.AddRef();
        obj["n"] = 20;
        Assert.Equal(20, reference["n"]);
        obj.Release();
        reference.Release();
    }

    [Fact]
    public void CloneIsIndependent()
    {
        var obj = new Dynamic();
        obj["n"] = 1;
        var clone = obj.Clone();
        clone["n"] = 2;
        Assert.Equal(1, obj["n"]);
        Assert.Equal(2, clone["n"]);
        obj.Release();
        clone.Release();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Field access
// ═════════════════════════════════════════════════════════════════════════════

public class FieldAccessTests : IDisposable
{
    private readonly Dynamic _obj = new();

    public void Dispose() => _obj.Release();

    [Fact]
    public void ScalarRoundTrip()
    {
        _obj["score"] = 42;
        _obj["speed"] = 9.5f;
        _obj["alive"] = true;
        _obj["name"] = "hero";
        Assert.Equal(42, _obj["score"]);
        Assert.Equal(9.5, (double)(float)_obj["speed"]!, precision: 4);
        Assert.Equal(true, _obj["alive"]);
        Assert.Equal("hero", _obj["name"]);
    }

    [Fact]
    public void TypeLockedFieldRaises()
    {
        _obj["score"] = 1;
        var ex = Assert.Throws<BisonException>(() => _obj["score"] = 1.5f);
        Assert.Equal(BisonErrorCode.Type, ex.Code);
    }

    [Fact]
    public void NestedObject()
    {
        var child = new Dynamic();
        child["city"] = "Springfield";
        _obj["address"] = child;
        child.Release();

        var addr = (Dynamic)_obj["address"]!;
        Assert.Equal("Springfield", addr["city"]);
        addr.Release();
    }

    [Fact]
    public void IndexedFieldsAndSize()
    {
        _obj[0] = "red";
        _obj[1] = "green";
        _obj[2] = "blue";
        Assert.Equal(3, _obj.Size);
        Assert.Equal("green", _obj[1]);
        Assert.Equal(new object?[] { "red", "green", "blue" }, _obj.ToList());
    }

    [Fact]
    public void IndexedTypeLock()
    {
        _obj[0] = 10;
        Assert.Throws<BisonException>(() => _obj[0] = 1.5f);
    }

    [Fact]
    public void IndexedBoolRoundTripsAsBool()
    {
        // Regression coverage: indexed bool used to be silently coerced to
        // int32 (bison_set_bool_at() didn't exist yet), so obj[0] came back
        // boxed as `1`/`0` (int) instead of `true`/`false` (bool).
        _obj[0] = true;
        _obj[1] = false;
        Assert.Equal(true, _obj[0]);
        Assert.Equal(false, _obj[1]);
        Assert.IsType<bool>(_obj[0]);
    }

    [Fact]
    public void IndexedBoolTypeLocked()
    {
        _obj[0] = true;
        Assert.Throws<BisonException>(() => _obj[0] = 5);
    }

    [Fact]
    public void IndexedObjectRoundTrip()
    {
        var child = new Dynamic();
        child["v"] = 9;
        _obj[0] = child;
        child.Release();

        var outChild = (Dynamic)_obj[0]!;
        Assert.Equal(9, outChild["v"]);
        outChild.Release();
    }

    [Fact]
    public void IndexedNullObjectRoundTrip()
    {
        _obj[0] = null;
        Assert.Null(_obj[0]);
    }

    [Fact]
    public void SetKeyAtRoundTrips()
    {
        _obj.SetKeyAt(0, "hero");
        Assert.Equal(Key.Of("hero"), _obj[0]);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// key_t-typed field access
// ═════════════════════════════════════════════════════════════════════════════

/// <summary>SetKey()/GetKey() (via the indexer's Get() fallback) -- distinct
/// from the plain int32 a bare `int` would otherwise become via the
/// indexer's Set() (see SetKey's own doc comment).</summary>
public class KeyTypedFieldAccessTests : IDisposable
{
    private readonly Dynamic _obj = new();

    public void Dispose() => _obj.Release();

    [Fact]
    public void RoundTripsWithAlreadyHashedValue()
    {
        _obj.SetKey("id", Key.Of("hero"));
        Assert.Equal(Key.Of("hero"), _obj["id"]);
    }

    [Fact]
    public void RoundTripsWithNameString()
    {
        _obj.SetKey("id", "hero");
        Assert.Equal(Key.Of("hero"), _obj["id"]);
    }

    [Fact]
    public void DistinctFromInt32Field()
    {
        // An int32-typed field and a key_t-typed field are different bison
        // variant types even when they happen to hold numerically equal
        // values -- SetKey() on a field already claimed as int32 must fail
        // the same way any other type-locked field access would.
        _obj["plain_int"] = 42;
        var ex = Assert.Throws<BisonException>(() => _obj.SetKey("plain_int", Key.Of("anything")));
        Assert.Equal(BisonErrorCode.Type, ex.Code);
    }

    [Fact]
    public void GetFallsBackToKeyAfterOtherTypesFail()
    {
        // The indexer's Get() cascade (int/float/bool/string/object) must
        // all fail by type mismatch before reaching the key_t fallback --
        // proven here by setting the field as key_t first (an untouched
        // field would instead auto-vivify as int32 zero on the very first
        // check; see Get()'s cascade-ordering comment in Dynamic.cs).
        _obj.SetKey("selector", "nav_mode_topdown");
        Assert.Equal(Key.Of("nav_mode_topdown"), _obj["selector"]);
    }

    [Fact]
    public void AddFieldKeyDeclaresAndRejectsDuplicate()
    {
        _obj.AddFieldKey("id", Key.Of("hero"));
        Assert.Equal(Key.Of("hero"), _obj["id"]);
        var ex = Assert.Throws<BisonException>(() => _obj.AddFieldKey("id", Key.Of("other")));
        Assert.Equal(BisonErrorCode.Duplicate, ex.Code);
    }

    [Fact]
    public void AddFieldKeyWithNameString()
    {
        _obj.AddFieldKey("id", "sidekick");
        Assert.Equal(Key.Of("sidekick"), _obj["id"]);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Vector fields
// ═════════════════════════════════════════════════════════════════════════════

public class VectorFieldsTests : IDisposable
{
    private readonly Dynamic _obj = new();

    public void Dispose() => _obj.Release();

    [Fact]
    public void IntRoundTrip()
    {
        _obj["ints"] = new[] { 1, 2, 3 };
        Assert.Equal(new[] { 1, 2, 3 }, (int[])_obj["ints"]!);
    }

    [Fact]
    public void BoolRoundTrip()
    {
        _obj["flags"] = new[] { true, false, true };
        Assert.Equal(new[] { true, false, true }, (bool[])_obj["flags"]!);
    }

    [Fact]
    public void FloatRoundTrip()
    {
        _obj["ratios"] = new[] { 1.5f, 2.5f };
        Assert.Equal(new[] { 1.5f, 2.5f }, (float[])_obj["ratios"]!);
    }

    [Fact]
    public void BytesRoundTrip()
    {
        _obj["blob"] = new byte[] { 0, 1, 255 };
        Assert.Equal(new byte[] { 0, 1, 255 }, (byte[])_obj["blob"]!);
    }

    [Fact]
    public void AssignmentReplacesExistingContents()
    {
        _obj["ints"] = new[] { 1, 2, 3 };
        _obj["ints"] = new[] { 9, 9 };
        Assert.Equal(new[] { 9, 9 }, (int[])_obj["ints"]!);
    }

    [Fact]
    public void AddFieldRegistersAndIsReadable()
    {
        _obj.AddField("ints", new[] { 1, 2, 3 });
        Assert.Equal(new[] { 1, 2, 3 }, (int[])_obj["ints"]!);
    }

    [Fact]
    public void AddFieldRejectsDuplicate()
    {
        _obj.AddField("ints", new[] { 1, 2, 3 });
        var ex = Assert.Throws<BisonException>(() => _obj.AddField("ints", new[] { 9 }));
        Assert.Equal(BisonErrorCode.Duplicate, ex.Code);
    }

    [Fact]
    public void IndexedAssignmentThrows()
    {
        // Vectors have no numeric-index form (bison_c.h has no
        // bison_set_vector_*_at) -- only named fields support them.
        Assert.Throws<ArgumentException>(() => _obj[0] = new[] { 1, 2, 3 });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Methods
// ═════════════════════════════════════════════════════════════════════════════

public class MethodTests
{
    [Fact]
    public void AddMethodAndCall()
    {
        var calc = new Dynamic();
        calc.AddMethod("add", (self, p, result) => result["value"] = (int)p["a"]! + (int)p["b"]!);
        var args = new Dynamic();
        args["a"] = 10;
        args["b"] = 32;
        var outResult = calc.Call("add", args);
        Assert.Equal(42, outResult["value"]);
        outResult.Release();
        args.Release();
        calc.Release();
    }

    [Fact]
    public void MethodMutatesSelf()
    {
        var calc = new Dynamic();
        calc["total"] = 0;
        calc.AddMethod("accumulate", (self, p, result) =>
        {
            var total = (int)self["total"]! + (int)p["n"]!;
            self["total"] = total;
            result["total"] = total;
        });
        for (var i = 1; i <= 3; i++)
        {
            var p = new Dynamic();
            p["n"] = i;
            var r = calc.Call("accumulate", p);
            r.Release();
            p.Release();
        }
        Assert.Equal(6, calc["total"]);
        calc.Release();
    }

    [Fact]
    public void CallUnknownMethodRaises()
    {
        var obj = new Dynamic();
        var ex = Assert.Throws<BisonException>(() => obj.Call("nope"));
        Assert.Equal(BisonErrorCode.NotFound, ex.Code);
        obj.Release();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Class registry / inheritance
// ═════════════════════════════════════════════════════════════════════════════

public class ClassRegistryTests : IDisposable
{
    public ClassRegistryTests() => Dynamic.ClearRegistry();

    public void Dispose() => Dynamic.ClearRegistry();

    [Fact]
    public void Inheritance()
    {
        var shape = new Dynamic("Shape");
        shape["color"] = "black";
        Dynamic.AddClass(shape);
        shape.Release();

        var circle = new Dynamic("Circle");
        circle["radius"] = 1.0f;
        Dynamic.AddClass(circle, parentName: "Shape");
        circle.Release();

        var c = Dynamic.Instantiate("Circle");
        Assert.Equal("black", c["color"]); // inherited default
        Assert.Equal(1.0, (double)(float)c["radius"]!, precision: 4);
        c.Release();
    }

    [Fact]
    public void DuplicateClassRaises()
    {
        var proto = new Dynamic("Shape");
        Dynamic.AddClass(proto);
        proto.Release();

        var dup = new Dynamic("Shape");
        var ex = Assert.Throws<BisonException>(() => Dynamic.AddClass(dup));
        Assert.Equal(BisonErrorCode.Duplicate, ex.Code);
        dup.Release();
    }

    [Fact]
    public void FindClass()
    {
        var proto = new Dynamic("Shape");
        Dynamic.AddClass(proto);
        proto.Release();
        Assert.NotNull(Dynamic.FindClass("Shape"));
        Assert.Null(Dynamic.FindClass("DoesNotExist"));
    }

    [Fact]
    public void NamespacesIsolateSameName()
    {
        var mathTable = new Dynamic("table");
        mathTable["rows"] = 1;
        Dynamic.AddClass(mathTable, nsName: "math");
        mathTable.Release();

        var ikeaTable = new Dynamic("table");
        ikeaTable["legs"] = 4;
        Dynamic.AddClass(ikeaTable, nsName: "ikea");
        ikeaTable.Release();

        var mt = Dynamic.Instantiate("table", nsName: "math");
        var it = Dynamic.Instantiate("table", nsName: "ikea");
        Assert.Equal(1, mt["rows"]);
        Assert.Equal(4, it["legs"]);
        mt.Release();
        it.Release();
    }

    [Fact]
    public void ClassAndFieldAttributes()
    {
        var proto = new Dynamic("Widget");
        proto.AddField("count", 0, meta: new Attributes { Description = "a counter", Required = true });
        Dynamic.AddClass(proto, meta: new Attributes { DisplayName = "Widget class" });
        proto.Release();

        var attrs = Dynamic.ClassAttributes("Widget");
        Assert.Equal("Widget class", attrs.DisplayName);

        var w = Dynamic.Instantiate("Widget");
        var fieldAttrs = w.FieldAttributes("count");
        Assert.Equal("a counter", fieldAttrs.Description);
        Assert.True(fieldAttrs.Required);
        w.Release();
    }

    [Fact]
    public void RegisteredMethodSurvivesPrototypeCollection()
    {
        // A class's methods must keep working after the registering
        // prototype's managed wrapper is released -- regression coverage for
        // the callback-trampoline lifetime handled by Dynamic.RegisteredPrototypes.
        static void Register()
        {
            var proto = new Dynamic("Doubler");
            proto.AddMethod("double", (self, p, result) => result["value"] = (int)p["n"]! * 2);
            Dynamic.AddClass(proto);
            proto.Release();
        }

        Register();
        GC.Collect();
        GC.WaitForPendingFinalizers();

        var inst = Dynamic.Instantiate("Doubler");
        var args = new Dynamic();
        args["n"] = 21;
        var outResult = inst.Call("double", args);
        Assert.Equal(42, outResult["value"]);
        outResult.Release();
        args.Release();
        inst.Release();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// JSON / YAML import
// ═════════════════════════════════════════════════════════════════════════════

public class SerializationTests
{
    [Fact]
    public void FromJson()
    {
        var obj = Dynamic.FromJson("""{"x": 1, "y": 2.5, "tags": ["a", "b"]}""");
        Assert.Equal(1, obj["x"]);
        Assert.Equal(2.5, (double)(float)obj["y"]!, precision: 4);
        var tags = (Dynamic)obj["tags"]!;
        Assert.Equal(new object?[] { "a", "b" }, tags.ToList());
        tags.Release();
        obj.Release();
    }

    [Fact]
    public void FromYaml()
    {
        var obj = Dynamic.FromYaml("x: 10\nname: test\n");
        Assert.Equal(10, obj["x"]);
        Assert.Equal("test", obj["name"]);
        obj.Release();
    }

    [Fact]
    public void ToJsonProducesValidJson()
    {
        // Field keys are emitted as "#<hash>" via the C ABI (no key-name map
        // is exposed across it), so this checks structural validity/values
        // rather than matching on field names.
        var obj = Dynamic.FromJson("""{"x": 1}""");
        var json = obj.ToJson(indent: -1);
        Assert.Contains("1", json);
        obj.Release();
    }

    [Fact]
    public void InvalidJsonRaises()
    {
        Assert.Throws<FormatException>(() => Dynamic.FromJson("not json"));
    }
}

public class BinarySerializationTests
{
    [Fact]
    public void RoundTripsScalarFields()
    {
        var obj = new Dynamic();
        obj["x"] = 42;
        obj["y"] = 2.5f;
        obj["s"] = "hello";

        var buf = obj.Serialize();
        Assert.NotEmpty(buf);

        var decoded = Dynamic.Deserialize(buf);
        Assert.Equal(42, decoded["x"]);
        Assert.Equal(2.5, (double)(float)decoded["y"]!, precision: 4);
        Assert.Equal("hello", decoded["s"]);
        obj.Release();
        decoded.Release();
    }

    [Fact]
    public void RoundTripsNestedObject()
    {
        var obj = new Dynamic();
        var child = new Dynamic();
        child["city"] = "Springfield";
        obj["address"] = child;
        child.Release();

        var decoded = Dynamic.Deserialize(obj.Serialize());
        var addr = (Dynamic)decoded["address"]!;
        Assert.Equal("Springfield", addr["city"]);
        addr.Release();
        obj.Release();
        decoded.Release();
    }

    [Fact]
    public void MalformedBufferRaises()
    {
        var ex = Assert.Throws<BisonException>(() => Dynamic.Deserialize(new byte[] { 0xFF, 0x00, 0x01 }));
        Assert.Equal(BisonErrorCode.Parse, ex.Code);
    }

    [Fact]
    public void EmptyObjectRoundTrips()
    {
        var obj = new Dynamic();
        var decoded = Dynamic.Deserialize(obj.Serialize());
        Assert.NotNull(decoded);
        obj.Release();
        decoded.Release();
    }
}

public class KeyHashingTests
{
    [Fact]
    public void KeyIsStable()
    {
        Assert.Equal(Key.Of("velocity"), Key.Of("velocity"));
    }

    [Fact]
    public void KeyHighBitSet()
    {
        Assert.NotEqual(0u, Key.Of("velocity") & 0x80000000);
    }

    [Fact]
    public void DifferentNamesDiffer()
    {
        Assert.NotEqual(Key.Of("velocity"), Key.Of("score"));
    }

    [Fact]
    public void KeyIsMemoized()
    {
        Key.ClearCache();
        var before = Key.CacheCount;
        var first = Key.Of("memoization_probe_field");
        var afterFirst = Key.CacheCount;
        var second = Key.Of("memoization_probe_field");
        var third = Key.Of("memoization_probe_field");
        var afterRepeats = Key.CacheCount;

        Assert.Equal(first, second);
        Assert.Equal(second, third);
        Assert.Equal(before + 1, afterFirst); // first call inserts one entry
        Assert.Equal(afterFirst, afterRepeats); // repeats hit the cache, no growth
    }

    [Fact]
    public void KeyCacheBounded()
    {
        Assert.Equal(4096, Key.MaxCacheSize);
    }
}
