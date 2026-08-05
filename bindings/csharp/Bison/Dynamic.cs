// MIT License © 2025 Binary Dice Games
/**
 * @file Dynamic.cs
 * @brief RAII wrapper around `bison_c.h`'s `bison_handle` -- the C# analogue
 *        of `bison/dynamic.py`'s `Dynamic` class.
 *
 * Exposes the same three access styles the Python binding does:
 *   - Indexer:  `obj["field"] = value`, `obj[0]` (array-like index).
 *   - Dynamic:  `dynamic d = obj; d.Field = value; d.Method(a: 1, b: 2);`
 *     via `DynamicObject`, mirroring Python's `__getattr__`/`__setattr__`.
 *   - Typed:    `obj.Call("method", args)`, `obj.AddField(...)`, etc.
 *
 * Reference counting is handled automatically via `IDisposable`/a finalizer
 * safety net; callers never need to call `bison_release` themselves.
 */

using System.Dynamic;
using System.Runtime.InteropServices;

namespace Bdg.Bison;

/// <summary>A reference-counted Bison dynamic object.</summary>
public sealed class Dynamic : DynamicObject, IDisposable, IEnumerable<object?>
{
    // _owned=true  -> this instance holds a reference; Release() decrements it.
    // _owned=false -> non-owning view (e.g. self/params/result inside a method
    //                 callback, or a class-registry lookup); Release() is a no-op.
    private readonly bool _owned;
    private bool _released;
    private readonly List<object> _callbacks = new(); // keep native-callback delegates alive

    internal nint Handle { get; private set; }

    public Dynamic(string klassName = "")
    {
        _owned = true;
        Handle = Native.bison_create(klassName.Length > 0 ? Key.Of(klassName) : 0);
        if (Handle == nint.Zero)
        {
            throw new OutOfMemoryException("bison_create failed");
        }
    }

    internal Dynamic(nint handle, bool owned = true)
    {
        _owned = owned;
        Handle = handle;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    public void Release()
    {
        if (_owned && !_released)
        {
            _released = true;
            Native.bison_release(Handle);
            Handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Release();
        GC.SuppressFinalize(this);
    }

    ~Dynamic() => Release();

    public Dynamic AddRef()
    {
        var h = Native.bison_add_ref(Handle);
        if (h == nint.Zero)
        {
            throw new InvalidOperationException("bison_add_ref failed");
        }
        return new Dynamic(h);
    }

    public Dynamic Clone()
    {
        var h = Native.bison_clone(Handle);
        if (h == nint.Zero)
        {
            throw new InvalidOperationException("bison_clone failed");
        }
        return new Dynamic(h);
    }

    // ── Field access (indexer) ───────────────────────────────────────────────

    public object? this[string name]
    {
        get => Get(name);
        set => Set(name, value);
    }

    public object? this[int index]
    {
        get => GetAt(index);
        set => SetAt(index, value);
    }

    private void Set(string name, object? value)
    {
        var k = Key.Of(name);
        switch (value)
        {
            case bool b:
                BisonException.Check(Native.bison_set_bool(Handle, k, b ? 1 : 0), $"set_bool[{name}]");
                break;
            case int i:
                BisonException.Check(Native.bison_set_int(Handle, k, i), $"set_int[{name}]");
                break;
            case float f:
                BisonException.Check(Native.bison_set_float(Handle, k, f), $"set_float[{name}]");
                break;
            case double d:
                BisonException.Check(Native.bison_set_float(Handle, k, (float)d), $"set_float[{name}]");
                break;
            case string s:
                BisonException.Check(Native.bison_set_string(Handle, k, s), $"set_string[{name}]");
                break;
            case Dynamic dyn:
                BisonException.Check(Native.bison_set_object(Handle, k, dyn.Handle), $"set_object[{name}]");
                break;
            case null:
                BisonException.Check(Native.bison_set_object(Handle, k, nint.Zero), $"set_object_null[{name}]");
                break;
            case bool[] boolArr:
                {
                    var ints = Array.ConvertAll(boolArr, x => x ? 1 : 0);
                    BisonException.Check(Native.bison_set_vector_bool(Handle, k, ints, (nuint)ints.Length), $"set_vector_bool[{name}]");
                    break;
                }
            case int[] intArr:
                BisonException.Check(Native.bison_set_vector_int(Handle, k, intArr, (nuint)intArr.Length), $"set_vector_int[{name}]");
                break;
            case float[] floatArr:
                BisonException.Check(Native.bison_set_vector_float(Handle, k, floatArr, (nuint)floatArr.Length), $"set_vector_float[{name}]");
                break;
            case byte[] byteArr:
                BisonException.Check(Native.bison_set_vector_bytes(Handle, k, byteArr, (nuint)byteArr.Length), $"set_vector_bytes[{name}]");
                break;
            default:
                throw new ArgumentException($"Unsupported value type: {value.GetType()}");
        }
    }

    private object? Get(string name)
    {
        var k = Key.Of(name);

        if (Native.bison_get_int(Handle, k, out var vInt) == (int)BisonErrorCode.Ok)
        {
            return vInt;
        }
        if (Native.bison_get_float(Handle, k, out var vFloat) == (int)BisonErrorCode.Ok)
        {
            return vFloat;
        }
        if (Native.bison_get_bool(Handle, k, out var vBool) == (int)BisonErrorCode.Ok)
        {
            return vBool != 0;
        }
        if (TryGetString(k, out var vString))
        {
            return vString;
        }
        if (Native.bison_get_object(Handle, k, out var child) == (int)BisonErrorCode.Ok)
        {
            return child != nint.Zero ? new Dynamic(child) : null;
        }
        // bison_get_key() is checked last, matching every other cascade step
        // above: it only ever succeeds here for a field already holding a
        // key_t value -- bison_get_int() would already have auto-vivified a
        // genuinely absent field as int32 zero (see SetKey's doc comment).
        if (Native.bison_get_key(Handle, k, out var vKey) == (int)BisonErrorCode.Ok)
        {
            return vKey;
        }
        // Vector-typed fields are tried last of all, for the same reason
        // bison_get_key() is: a field never explicitly set as a vector
        // would already have been claimed (and auto-vivified) by
        // bison_get_int() above, so these four only ever succeed for a
        // field actually holding that vector variant.
        if (TryGetVector(k, out var vector))
        {
            return vector;
        }

        throw new KeyNotFoundException($"Field '{name}' not found or has an unsupported type");
    }

    private bool TryGetVector(uint k, out object? value)
    {
        if (TryGetVectorInt(k, out var vInt))
        {
            value = vInt;
            return true;
        }
        if (TryGetVectorFloat(k, out var vFloat))
        {
            value = vFloat;
            return true;
        }
        if (TryGetVectorBool(k, out var vBool))
        {
            value = vBool;
            return true;
        }
        if (TryGetVectorBytes(k, out var vBytes))
        {
            value = vBytes;
            return true;
        }
        value = null;
        return false;
    }

    private bool TryGetVectorInt(uint k, out int[]? value)
    {
        if (Native.bison_get_vector_int(Handle, k, null, 0, out var lenOut) != (int)BisonErrorCode.Ok)
        {
            value = null;
            return false;
        }
        var arr = new int[lenOut];
        if (lenOut > 0)
        {
            Native.bison_get_vector_int(Handle, k, arr, lenOut, out _);
        }
        value = arr;
        return true;
    }

    private bool TryGetVectorFloat(uint k, out float[]? value)
    {
        if (Native.bison_get_vector_float(Handle, k, null, 0, out var lenOut) != (int)BisonErrorCode.Ok)
        {
            value = null;
            return false;
        }
        var arr = new float[lenOut];
        if (lenOut > 0)
        {
            Native.bison_get_vector_float(Handle, k, arr, lenOut, out _);
        }
        value = arr;
        return true;
    }

    private bool TryGetVectorBool(uint k, out bool[]? value)
    {
        if (Native.bison_get_vector_bool(Handle, k, null, 0, out var lenOut) != (int)BisonErrorCode.Ok)
        {
            value = null;
            return false;
        }
        var arr = new int[lenOut];
        if (lenOut > 0)
        {
            Native.bison_get_vector_bool(Handle, k, arr, lenOut, out _);
        }
        value = Array.ConvertAll(arr, x => x != 0);
        return true;
    }

    private bool TryGetVectorBytes(uint k, out byte[]? value)
    {
        if (Native.bison_get_vector_bytes(Handle, k, null, 0, out var lenOut) != (int)BisonErrorCode.Ok)
        {
            value = null;
            return false;
        }
        var arr = new byte[lenOut];
        if (lenOut > 0)
        {
            Native.bison_get_vector_bytes(Handle, k, arr, lenOut, out _);
        }
        value = arr;
        return true;
    }

    private bool TryGetString(uint k, out string? value)
    {
        if (Native.bison_get_string(Handle, k, null, 0, out var lenOut) != (int)BisonErrorCode.Ok)
        {
            value = null;
            return false;
        }
        var buf = new byte[lenOut + 1];
        Native.bison_get_string(Handle, k, buf, (nuint)buf.Length, out _);
        value = System.Text.Encoding.UTF8.GetString(buf, 0, (int)lenOut);
        return true;
    }

    private object? GetAt(int index)
    {
        var idx = (nuint)index;

        if (Native.bison_get_int_at(Handle, idx, out var vInt) == (int)BisonErrorCode.Ok)
        {
            return vInt;
        }
        if (Native.bison_get_float_at(Handle, idx, out var vFloat) == (int)BisonErrorCode.Ok)
        {
            return vFloat;
        }
        if (Native.bison_get_bool_at(Handle, idx, out var vBool) == (int)BisonErrorCode.Ok)
        {
            return vBool != 0;
        }
        if (Native.bison_get_string_at(Handle, idx, null, 0, out var lenOut) == (int)BisonErrorCode.Ok)
        {
            var buf = new byte[lenOut + 1];
            Native.bison_get_string_at(Handle, idx, buf, (nuint)buf.Length, out _);
            return System.Text.Encoding.UTF8.GetString(buf, 0, (int)lenOut);
        }
        if (Native.bison_get_object_at(Handle, idx, out var child) == (int)BisonErrorCode.Ok)
        {
            return child != nint.Zero ? new Dynamic(child) : null;
        }
        // See Get()'s identical note on why bison_get_key() is tried last:
        // it only succeeds for an index already holding a key_t value.
        if (Native.bison_get_key_at(Handle, idx, out var vKey) == (int)BisonErrorCode.Ok)
        {
            return vKey;
        }

        throw new IndexOutOfRangeException($"Index {index} not found or has an unsupported type");
    }

    private void SetAt(int index, object? value)
    {
        var idx = (nuint)index;
        switch (value)
        {
            // bool is checked before int (a boxed bool would never match a
            // `case int` pattern, but listed first for clarity/symmetry with
            // Set()) so it round-trips as a real bison bool field via
            // bison_set_bool_at() -- not silently coerced to int32, the same
            // distinction bison_set_bool()/bison_set_int() already make for
            // named fields.
            case bool b:
                BisonException.Check(Native.bison_set_bool_at(Handle, idx, b ? 1 : 0), $"set_bool_at[{index}]");
                break;
            case int i:
                BisonException.Check(Native.bison_set_int_at(Handle, idx, i), $"set_int_at[{index}]");
                break;
            case float f:
                BisonException.Check(Native.bison_set_float_at(Handle, idx, f), $"set_float_at[{index}]");
                break;
            case double d:
                BisonException.Check(Native.bison_set_float_at(Handle, idx, (float)d), $"set_float_at[{index}]");
                break;
            case string s:
                BisonException.Check(Native.bison_set_string_at(Handle, idx, s), $"set_string_at[{index}]");
                break;
            case Dynamic dyn:
                BisonException.Check(Native.bison_set_object_at(Handle, idx, dyn.Handle), $"set_object_at[{index}]");
                break;
            case null:
                BisonException.Check(Native.bison_set_object_at(Handle, idx, nint.Zero), $"set_object_at_null[{index}]");
                break;
            default:
                throw new ArgumentException($"Unsupported value type for indexed field: {value?.GetType()}");
        }
    }

    // Note: no ContainsKey()/"exists" query is provided. The underlying C ABI
    // has no "field exists" call -- bison_get_int() et al. lazily create a
    // zero-valued field on first access (matching dynamic::operator[]
    // semantics in C++), so a containment check would itself mutate the
    // object. Use FieldAttributes()/MethodAttributes() (which do fail with
    // BISON_ERR_NOT_FOUND) where an existence check is needed.

    // ── bison::key_t-typed field access ──────────────────────────────────────
    //
    // A field the C++ side declares as `bison::key_t` (e.g. an object's
    // "id", or an enum-like selector) is a distinct bison field-variant type
    // from `int32_t` -- reading one is handled by Get()'s cascade above (its
    // final fallback, after int/float/bool/string/object all fail by type
    // mismatch), but writing one needs an explicit call: the indexer's
    // `Set(string, object?)` always writes a plain `int` as int32 (there is
    // no way to tell "this int should become a key_t field" from "this int
    // should become an int32 field" without one).

    /// <summary>Sets a <c>bison::key_t</c>-valued field (e.g. an object's
    /// <c>"id"</c>) to an already-hashed value.</summary>
    public void SetKey(string name, uint value) =>
        BisonException.Check(Native.bison_set_key(Handle, Key.Of(name), value), $"set_key[{name}]");

    /// <summary>Sets a <c>bison::key_t</c>-valued field to the hash of
    /// <paramref name="value"/>, the same way <c>"value"_key</c> would in
    /// C++.</summary>
    public void SetKey(string name, string value) => SetKey(name, Key.Of(value));

    /// <summary>Sets a <c>bison::key_t</c>-valued field by numeric index --
    /// the indexed counterpart to <see cref="SetKey(string, uint)"/>, for the
    /// same reason a bare <c>obj[index] = value</c> int assignment can't
    /// dispatch to this.</summary>
    public void SetKeyAt(int index, uint value) =>
        BisonException.Check(Native.bison_set_key_at(Handle, (nuint)index, value), $"set_key_at[{index}]");

    /// <summary>Overload of <see cref="SetKeyAt(int, uint)"/> taking a name
    /// to hash instead of an already-hashed value.</summary>
    public void SetKeyAt(int index, string value) => SetKeyAt(index, Key.Of(value));

    // ── Array-like helpers ────────────────────────────────────────────────────

    public int Size => (int)Native.bison_size(Handle);

    public IEnumerator<object?> GetEnumerator()
    {
        for (var i = 0; i < Size; i++)
        {
            yield return this[i];
        }
    }

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();

    // ── Methods ───────────────────────────────────────────────────────────────

    /// <summary><paramref name="fn"/> must populate <c>result</c> in place
    /// (it must not be disposed/released).</summary>
    public delegate void MethodCallback(Dynamic self, Dynamic parameters, Dynamic result);

    public void AddMethod(string name, MethodCallback fn)
    {
        void Trampoline(nint selfH, nint paramsH, nint resultH, nint _)
        {
            var selfDyn = new Dynamic(selfH, owned: false);
            var paramsDyn = new Dynamic(paramsH, owned: false);
            var resultDyn = new Dynamic(resultH, owned: false);
            try
            {
                fn(selfDyn, paramsDyn, resultDyn);
            }
            catch
            {
                // C ABI boundary: exceptions must not cross back into C++.
            }
        }

        NativeMethodFn native = Trampoline;
        _callbacks.Add(native); // keep alive for the object's lifetime
        var fnPtr = Marshal.GetFunctionPointerForDelegate(native);
        unsafe
        {
            BisonException.Check(Native.bison_add_method(Handle, Key.Of(name), fnPtr, nint.Zero, null), $"add_method({name})");
        }
    }

    public Dynamic Call(string name, Dynamic? parameters = null)
    {
        var ownsParams = parameters is null;
        parameters ??= new Dynamic();
        try
        {
            BisonException.Check(Native.bison_call(Handle, Key.Of(name), parameters.Handle, out var resultH), $"call({name})");
            return new Dynamic(resultH);
        }
        finally
        {
            if (ownsParams)
            {
                parameters.Release();
            }
        }
    }

    /// <summary>Convenience overload: builds a scratch <see cref="Dynamic"/>
    /// from <paramref name="kwargs"/> and calls <see cref="Call(string, Dynamic?)"/>.</summary>
    public Dynamic Call(string name, IDictionary<string, object?> kwargs)
    {
        using var args = new Dynamic();
        foreach (var (k, v) in kwargs)
        {
            args[k] = v;
        }
        return Call(name, args);
    }

    // ── Dynamic member access (the `dynamic obj; obj.Field` style) ──────────

    public override bool TryGetMember(GetMemberBinder binder, out object? result)
    {
        var name = binder.Name;
        var k = Key.Of(name);
        if (FieldExists(k))
        {
            result = this[name];
            return true;
        }
        if (MethodExists(k))
        {
            result = (Func<IDictionary<string, object?>?, Dynamic>)(kwargs => Call(name, kwargs ?? new Dictionary<string, object?>()));
            return true;
        }
        result = null;
        return false;
    }

    public override bool TrySetMember(SetMemberBinder binder, object? value)
    {
        this[binder.Name] = value;
        return true;
    }

    /// <summary>
    /// Backs calls like <c>dynamic calc = ...; calc.Add(a: 10, b: 32)</c>.
    /// Only named arguments are supported -- Bison methods take a single
    /// params object keyed by name, mirroring Python's <c>**kwargs</c>-only
    /// method-call convention; purely positional arguments are rejected.
    /// </summary>
    public override bool TryInvokeMember(InvokeMemberBinder binder, object?[] args, out object? result)
    {
        var argNames = binder.CallInfo.ArgumentNames;
        var positionalCount = args.Length - argNames.Count;
        if (positionalCount != 0)
        {
            throw new ArgumentException("Dynamic method calls only support named arguments, e.g. calc.Add(a: 1, b: 2)");
        }

        var kwargs = new Dictionary<string, object?>();
        for (var i = 0; i < argNames.Count; i++)
        {
            kwargs[argNames[i]] = args[i];
        }
        result = Call(binder.Name, kwargs);
        return true;
    }

    private bool FieldExists(uint k)
    {
        unsafe
        {
            NativeAttributes attrs;
            return Native.bison_get_field_attributes(Handle, k, &attrs) == (int)BisonErrorCode.Ok;
        }
    }

    private bool MethodExists(uint k)
    {
        unsafe
        {
            NativeAttributes attrs;
            return Native.bison_get_method_attributes(Handle, k, &attrs) == (int)BisonErrorCode.Ok;
        }
    }

    // ── Field / method attributes ────────────────────────────────────────────

    public Attributes FieldAttributes(string name)
    {
        unsafe
        {
            NativeAttributes attrs;
            BisonException.Check(Native.bison_get_field_attributes(Handle, Key.Of(name), &attrs), $"field_attributes({name})");
            return Attributes.FromNative(attrs);
        }
    }

    public Attributes MethodAttributes(string name)
    {
        unsafe
        {
            NativeAttributes attrs;
            BisonException.Check(Native.bison_get_method_attributes(Handle, Key.Of(name), &attrs), $"method_attributes({name})");
            return Attributes.FromNative(attrs);
        }
    }

    // ── Field registration with metadata ─────────────────────────────────────

    public void AddField(string name, object value, Attributes? meta = null)
    {
        var k = Key.Of(name);
        using var scope = meta?.ToNative();
        unsafe
        {
            NativeAttributes* metaPtr = null;
            var pinned = default(NativeAttributes);
            if (scope is not null)
            {
                pinned = scope.Native;
                metaPtr = &pinned;
            }

            switch (value)
            {
                case bool b:
                    BisonException.Check(Native.bison_add_field_bool(Handle, k, b ? 1 : 0, metaPtr), $"add_field_bool[{name}]");
                    break;
                case int i:
                    BisonException.Check(Native.bison_add_field_int(Handle, k, i, metaPtr), $"add_field_int[{name}]");
                    break;
                case float f:
                    BisonException.Check(Native.bison_add_field_float(Handle, k, f, metaPtr), $"add_field_float[{name}]");
                    break;
                case double d:
                    BisonException.Check(Native.bison_add_field_float(Handle, k, (float)d, metaPtr), $"add_field_float[{name}]");
                    break;
                case string s:
                    BisonException.Check(Native.bison_add_field_string(Handle, k, s, metaPtr), $"add_field_string[{name}]");
                    break;
                case bool[] boolArr:
                    {
                        var ints = Array.ConvertAll(boolArr, x => x ? 1 : 0);
                        BisonException.Check(
                            Native.bison_add_field_vector_bool(Handle, k, ints, (nuint)ints.Length, metaPtr), $"add_field_vector_bool[{name}]");
                        break;
                    }
                case int[] intArr:
                    BisonException.Check(
                        Native.bison_add_field_vector_int(Handle, k, intArr, (nuint)intArr.Length, metaPtr), $"add_field_vector_int[{name}]");
                    break;
                case float[] floatArr:
                    BisonException.Check(
                        Native.bison_add_field_vector_float(Handle, k, floatArr, (nuint)floatArr.Length, metaPtr), $"add_field_vector_float[{name}]");
                    break;
                case byte[] byteArr:
                    BisonException.Check(
                        Native.bison_add_field_vector_bytes(Handle, k, byteArr, (nuint)byteArr.Length, metaPtr), $"add_field_vector_bytes[{name}]");
                    break;
                default:
                    throw new ArgumentException($"Unsupported value type for AddField: {value.GetType()}");
            }
        }
    }

    /// <summary>Declares a new <c>bison::key_t</c>-valued field -- the
    /// <see cref="AddField"/> counterpart to <see cref="SetKey(string, uint)"/>,
    /// for the same reason <see cref="AddField"/> itself can't dispatch to
    /// this from a plain <c>int</c>. Fails with <see cref="BisonException"/>
    /// (<see cref="BisonErrorCode.Duplicate"/>) if the field already
    /// exists.</summary>
    public void AddFieldKey(string name, uint value, Attributes? meta = null)
    {
        var k = Key.Of(name);
        using var scope = meta?.ToNative();
        unsafe
        {
            NativeAttributes* metaPtr = null;
            var pinned = default(NativeAttributes);
            if (scope is not null)
            {
                pinned = scope.Native;
                metaPtr = &pinned;
            }
            BisonException.Check(Native.bison_add_field_key(Handle, k, value, metaPtr), $"add_field_key[{name}]");
        }
    }

    /// <summary>Overload of <see cref="AddFieldKey(string, uint, Attributes?)"/>
    /// taking a name to hash instead of an already-hashed value.</summary>
    public void AddFieldKey(string name, string value, Attributes? meta = null) => AddFieldKey(name, Key.Of(value), meta);

    // ── Serialization ─────────────────────────────────────────────────────────

    public string ToJson(int indent = 2)
    {
        BisonException.Check(Native.bison_to_json(Handle, indent, out var outPtr), "to_json");
        try
        {
            return Marshal.PtrToStringUTF8(outPtr) ?? "";
        }
        finally
        {
            Native.bison_free_string(outPtr);
        }
    }

    public string ToYaml()
    {
        BisonException.Check(Native.bison_to_yaml(Handle, out var outPtr), "to_yaml");
        try
        {
            return Marshal.PtrToStringUTF8(outPtr) ?? "";
        }
        finally
        {
            Native.bison_free_string(outPtr);
        }
    }

    public string Pretty(bool multiline = true, string indent = "  ")
    {
        var indentPtr = Marshal.StringToCoTaskMemUTF8(indent);
        try
        {
            unsafe
            {
                var opts = new NativePrintOptions { Multiline = multiline ? 1 : 0, Indent = indentPtr };
                BisonException.Check(Native.bison_print(Handle, &opts, out var outPtr), "pretty");
                try
                {
                    return Marshal.PtrToStringUTF8(outPtr) ?? "";
                }
                finally
                {
                    Native.bison_free_string(outPtr);
                }
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(indentPtr);
        }
    }

    /// <summary>Serializes to the compact binary wire format (see
    /// <c>FORMAT.md</c>) -- the counterpart to <see cref="Deserialize"/>.
    /// Field keys are encoded as their raw hash, so (unlike
    /// <see cref="ToJson"/>/<see cref="ToYaml"/>) this format is
    /// self-contained and needs no key-name map to round-trip.</summary>
    public byte[] Serialize()
    {
        BisonException.Check(Native.bison_serialize(Handle, out var outPtr, out var outLen), "serialize");
        try
        {
            var result = new byte[outLen];
            if (outLen > 0)
            {
                Marshal.Copy(outPtr, result, 0, (int)outLen);
            }
            return result;
        }
        finally
        {
            Native.bison_free_buffer(outPtr);
        }
    }

    public override string ToString() => Pretty();

    // ── Factory / registry functions ─────────────────────────────────────────

    /// <summary>Deserializes a buffer produced by <see cref="Serialize"/>.</summary>
    public static Dynamic Deserialize(byte[] data)
    {
        BisonException.Check(Native.bison_deserialize(data, (nuint)data.Length, out var h), "deserialize");
        return new Dynamic(h);
    }

    public static Dynamic FromJson(string text)
    {
        var h = Native.bison_from_json(text);
        if (h == nint.Zero)
        {
            throw new FormatException("FromJson: invalid or unsupported JSON");
        }
        return new Dynamic(h);
    }

    public static Dynamic FromYaml(string text)
    {
        var h = Native.bison_from_yaml(text);
        if (h == nint.Zero)
        {
            throw new FormatException("FromYaml: invalid or unsupported YAML");
        }
        return new Dynamic(h);
    }

    // bison_add_class() copies field/method data out of `prototype` into the
    // C++ registry -- it does not take ownership of the handle -- but a
    // registered method only copies the raw C function pointer, i.e. the
    // trampoline behind each MethodCallback. That trampoline is only kept
    // alive by managed references (Dynamic._callbacks); objects later
    // instantiated from the class (bison_instantiate) call back into it
    // directly. So every prototype ever registered must be kept alive for as
    // long as its class stays in the registry, or the trampoline gets
    // collected out from under the C++ side. This list mirrors the
    // registry's own lifetime (matches bison.dynamic._registered_prototypes).
    private static readonly List<Dynamic> RegisteredPrototypes = new();

    public static void AddClass(Dynamic prototype, string parentName = "", string nsName = "", Attributes? meta = null)
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        var parentKey = parentName.Length > 0 ? Key.Of(parentName) : 0;
        using var scope = meta?.ToNative();
        unsafe
        {
            NativeAttributes* metaPtr = null;
            var pinned = default(NativeAttributes);
            if (scope is not null)
            {
                pinned = scope.Native;
                metaPtr = &pinned;
            }
            BisonException.Check(Native.bison_add_class(nsKey, prototype.Handle, parentKey, metaPtr), $"add_class({parentName})");
        }
        RegisteredPrototypes.Add(prototype);
    }

    public static Dynamic? FindClass(string klassName, string nsName = "")
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        var h = Native.bison_find_class(nsKey, Key.Of(klassName));
        return h != nint.Zero ? new Dynamic(h, owned: false) : null;
    }

    public static Dynamic Instantiate(string klassName, string nsName = "")
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        var h = Native.bison_instantiate(nsKey, Key.Of(klassName));
        if (h == nint.Zero)
        {
            throw new BisonException(BisonErrorCode.NotFound, $"instantiate({klassName})");
        }
        return new Dynamic(h);
    }

    public static void ClearRegistry()
    {
        Native.bison_clear_registry();
        RegisteredPrototypes.Clear();
    }

    public static Attributes ClassAttributes(string klassName, string nsName = "")
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        unsafe
        {
            NativeAttributes attrs;
            BisonException.Check(Native.bison_get_class_attributes(nsKey, Key.Of(klassName), &attrs), $"class_attributes({klassName})");
            return Attributes.FromNative(attrs);
        }
    }
}
