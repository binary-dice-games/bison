using Bdg.Bison;
using Xunit;

namespace Bdg.Bison.Tests;

/// <summary>
/// xUnit tests for the Bison C# binding.
/// </summary>
/// <remarks>
/// Build <c>bison_c</c> first, then run from the <c>bindings/csharp/tests</c> directory:
/// <code>
/// cmake -B ../../build -DPACKAGE_TESTS=ON
/// cmake --build ../../build --config Debug --target bison_c
/// dotnet test
/// </code>
/// </remarks>
public class BisonTests
{
    // ═════════════════════════════════════════════════════════════════════════
    // 1. Lifecycle
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void CreateSucceeds()
    {
        var obj = new Dynamic();
        Assert.NotNull(obj);
        obj.Dispose();
    }

    [Fact]
    public void UsingBlockDisposes()
    {
        using var obj = new Dynamic();
        obj.SetInt("x", 1);
        // Dispose is called by the using block; no exception expected.
    }

    [Fact]
    public void DisposeIsIdempotent()
    {
        var obj = new Dynamic();
        obj.Dispose();
        obj.Dispose(); // must not throw
    }

    [Fact]
    public void AddRefReturnsNewHandle()
    {
        using var obj = new Dynamic();
        obj.SetInt("v", 99);
        using var ref2 = obj.AddRef();
        Assert.Equal(99, ref2.GetInt("v"));
    }

    [Fact]
    public void AddRefSharedMutation()
    {
        using var obj = new Dynamic();
        obj.SetInt("n", 10);
        using var ref2 = obj.AddRef();
        obj.SetInt("n", 20);
        // Both handles share the same underlying object.
        Assert.Equal(20, ref2.GetInt("n"));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 2. Named field setters / getters
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void IntRoundTrip()
    {
        using var obj = new Dynamic();
        obj.SetInt("score", 42);
        Assert.Equal(42, obj.GetInt("score"));
    }

    [Fact]
    public void FloatRoundTrip()
    {
        using var obj = new Dynamic();
        obj.SetFloat("ratio", 3.14f);
        Assert.Equal(3.14f, obj.GetFloat("ratio"), precision: 4);
    }

    [Fact]
    public void BoolRoundTripTrue()
    {
        using var obj = new Dynamic();
        obj.SetBool("flag", true);
        Assert.True(obj.GetBool("flag"));
    }

    [Fact]
    public void BoolRoundTripFalse()
    {
        using var obj = new Dynamic();
        obj.SetBool("flag", false);
        Assert.False(obj.GetBool("flag"));
    }

    [Fact]
    public void StringRoundTrip()
    {
        using var obj = new Dynamic();
        obj.SetString("name", "alice");
        Assert.Equal("alice", obj.GetString("name"));
    }

    [Fact]
    public void NestedObjectRoundTrip()
    {
        using var obj = new Dynamic();
        using (var child = new Dynamic())
        {
            child.SetInt("x", 7);
            obj.SetObject("inner", child);
        }

        using var inner = obj.GetObject("inner")!;
        Assert.Equal(7, inner.GetInt("x"));
    }

    [Fact]
    public void TypeMismatchThrows()
    {
        using var obj = new Dynamic();
        obj.SetInt("n", 1);
        // Attempting to re-set as float must throw BISON_ERR_TYPE.
        var ex = Assert.Throws<BisonError>(() => obj.SetFloat("n", 3.14f));
        Assert.Equal(BisonError.ErrType, ex.Code);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 3. Indexed (array-like) fields
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void IntAtRoundTrip()
    {
        using var arr = new Dynamic();
        arr.SetIntAt(0, 10);
        arr.SetIntAt(1, 20);
        arr.SetIntAt(2, 30);
        Assert.Equal(10, arr.GetIntAt(0));
        Assert.Equal(20, arr.GetIntAt(1));
        Assert.Equal(30, arr.GetIntAt(2));
    }

    [Fact]
    public void FloatAtRoundTrip()
    {
        using var arr = new Dynamic();
        arr.SetFloatAt(0, 1.5f);
        Assert.Equal(1.5f, arr.GetFloatAt(0), precision: 4);
    }

    [Fact]
    public void StringAtRoundTrip()
    {
        using var arr = new Dynamic();
        arr.SetStringAt(0, "hello");
        Assert.Equal("hello", arr.GetStringAt(0));
    }

    [Fact]
    public void SizeReflectsElements()
    {
        using var arr = new Dynamic();
        Assert.Equal(0L, arr.Size);
        arr.SetIntAt(0, 1);
        arr.SetIntAt(1, 2);
        Assert.Equal(2L, arr.Size);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 4. Import helpers
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void FromJsonFlat()
    {
        using var d = Dynamic.FromJson("{\"x\": 1, \"y\": 2}");
        Assert.Equal(1, d.GetInt("x"));
        Assert.Equal(2, d.GetInt("y"));
    }

    [Fact]
    public void FromJsonNested()
    {
        using var d = Dynamic.FromJson("{\"a\": {\"b\": 3}}");
        using var inner = d.GetObject("a")!;
        Assert.Equal(3, inner.GetInt("b"));
    }

    [Fact]
    public void FromJsonInvalidThrows()
    {
        Assert.Throws<BisonError>(() => Dynamic.FromJson("{broken"));
    }

    [Fact]
    public void FromYamlFlat()
    {
        using var d = Dynamic.FromYaml("x: 10\nname: test\n");
        Assert.Equal(10, d.GetInt("x"));
        Assert.Equal("test", d.GetString("name"));
    }

    [Fact]
    public void FromYamlBool()
    {
        using var d = Dynamic.FromYaml("flag: true\n");
        Assert.True(d.GetBool("flag"));
    }

    [Fact]
    public void FromYamlSequence()
    {
        using var d = Dynamic.FromYaml("- 1\n- 2\n- 3\n");
        Assert.Equal(3L, d.Size);
        Assert.Equal(1, d.GetIntAt(0));
    }

    [Fact]
    public void FromYamlInvalidThrows()
    {
        Assert.Throws<BisonError>(() => Dynamic.FromYaml("{broken"));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 5. BisonKey utility
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void KeyHighBitAlwaysSet()
    {
        Assert.True((BisonKey.Of("hello") & 0x80000000u) != 0);
    }

    [Fact]
    public void KeyIsDeterministic()
    {
        Assert.Equal(BisonKey.Of("foo"), BisonKey.Of("foo"));
    }

    [Fact]
    public void KeyDifferentStringsDiffer()
    {
        Assert.NotEqual(BisonKey.Of("alpha"), BisonKey.Of("beta"));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 6. Class registry
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void AddClassDoesNotCrash()
    {
        // Unique name per run to avoid global registry conflicts.
        string className = "CSharpTestShape_" + GetHashCode();
        using var proto = new Dynamic(className);
        proto.SetInt("sides", 3);
        try { proto.AddClass(); }
        catch (BisonError e)
        {
            // BISON_ERR_DUPLICATE is acceptable on repeated runs.
            Assert.Equal(BisonError.ErrDuplicate, e.Code);
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 7. Methods
    // ═════════════════════════════════════════════════════════════════════════

    [Fact]
    public void AddAndCallMethod()
    {
        using var obj = new Dynamic();
        obj.SetInt("n", 0);

        obj.AddMethod("inc", (self, _params) =>
        {
            int n = self.GetInt("n");
            self.SetInt("n", n + 1);
            return new Dynamic();
        });

        using var p = new Dynamic();
        using (obj.Call("inc", p)) { /* result unused */ }
        using (obj.Call("inc", p)) { /* result unused */ }

        Assert.Equal(2, obj.GetInt("n"));
    }

    [Fact]
    public void CallMissingMethodThrows()
    {
        using var obj = new Dynamic();
        using var p   = new Dynamic();
        var ex = Assert.Throws<BisonError>(() => obj.Call("no_such_method", p));
        Assert.Equal(BisonError.ErrNotFound, ex.Code);
    }

    [Fact]
    public void AddDuplicateMethodThrows()
    {
        using var obj = new Dynamic();
        obj.AddMethod("fn", (_, __) => new Dynamic());
        var ex = Assert.Throws<BisonError>(
            () => obj.AddMethod("fn", (_, __) => new Dynamic()));
        Assert.Equal(BisonError.ErrDuplicate, ex.Code);
    }
}
