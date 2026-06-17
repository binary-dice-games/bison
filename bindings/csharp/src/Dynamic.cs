using System.Runtime.InteropServices;
using System.Text;

namespace Bdg.Bison;

/// <summary>
/// A reference-counted dynamic object backed by <c>libbison_c</c>.
/// </summary>
/// <remarks>
/// <para>
/// <see cref="Dynamic"/> wraps an opaque <c>bison_handle</c> and provides a
/// .NET-idiomatic interface for reading and writing fields, calling methods,
/// and managing object lifetime.
/// </para>
///
/// <para>
/// Every <see cref="Dynamic"/> holds an owning reference.  Dispose it via
/// <c>using</c> or an explicit <see cref="Dispose"/> call when it is no longer
/// needed.
/// </para>
/// </remarks>
/// <example>
/// <code>
/// using var obj = new Dynamic();
/// obj.SetInt("hp",     100);
/// obj.SetFloat("speed", 9.5f);
/// obj.SetBool("alive", true);
/// obj.SetString("name", "hero");
///
/// Console.WriteLine($"hp    = {obj.GetInt("hp")}");
/// Console.WriteLine($"speed = {obj.GetFloat("speed"):F2}");
/// Console.WriteLine($"alive = {obj.GetBool("alive")}");
/// Console.WriteLine($"name  = {obj.GetString("name")}");
/// </code>
/// </example>
public sealed class Dynamic : IDisposable
{
    // ── Internal state ───────────────────────────────────────────────────────

    private IntPtr _handle;
    private readonly bool _owned;

    // Keep native delegates alive for the lifetime of this object.
    private readonly List<Native.MethodDelegate> _callbacks = new();

    // ── Constructors ─────────────────────────────────────────────────────────

    /// <summary>Create a new anonymous dynamic object.</summary>
    public Dynamic()
    {
        _handle = Native.bison_create(0u);
        if (_handle == IntPtr.Zero) throw new OutOfMemoryException("bison_create failed");
        _owned = true;
    }

    /// <summary>Create a new dynamic object belonging to the named class.</summary>
    /// <param name="className">Class name; the FNV-1a hash is computed automatically.</param>
    public Dynamic(string className)
    {
        _handle = Native.bison_create(BisonKey.Of(className));
        if (_handle == IntPtr.Zero) throw new OutOfMemoryException("bison_create failed");
        _owned = true;
    }

    /// <summary>Internal: adopt a pre-allocated native handle.</summary>
    internal Dynamic(IntPtr handle, bool owned)
    {
        _handle = handle;
        _owned  = owned;
    }

    // ── IDisposable / lifecycle ───────────────────────────────────────────────

    /// <summary>
    /// Release the native handle.  Safe to call multiple times; subsequent
    /// calls are no-ops.
    /// </summary>
    public void Dispose()
    {
        if (_owned && _handle != IntPtr.Zero)
        {
            Native.bison_release(_handle);
            _handle = IntPtr.Zero;
        }
    }

    /// <summary>
    /// Increment the reference count and return a new <see cref="Dynamic"/>
    /// sharing ownership of the same object.
    /// </summary>
    /// <remarks>The caller is responsible for disposing the returned instance.</remarks>
    public Dynamic AddRef()
    {
        var h = Native.bison_add_ref(_handle);
        if (h == IntPtr.Zero) throw new InvalidOperationException("bison_add_ref failed");
        return new Dynamic(h, true);
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private static void Check(int rc, string context)
    {
        if (rc != Native.BISON_OK) throw new BisonError(rc, context);
    }

    private static uint Key(string name) => BisonKey.Of(name);

    internal IntPtr Handle => _handle;

    // ── Named field setters ───────────────────────────────────────────────────

    /// <summary>Set an <c>int32_t</c> field by name.</summary>
    public void SetInt(string name, int value) =>
        Check(Native.bison_set_int(_handle, Key(name), value), $"SetInt({name})");

    /// <summary>Set a <c>float</c> field by name.</summary>
    public void SetFloat(string name, float value) =>
        Check(Native.bison_set_float(_handle, Key(name), value), $"SetFloat({name})");

    /// <summary>Set a <c>bool</c> field by name.</summary>
    public void SetBool(string name, bool value) =>
        Check(Native.bison_set_bool(_handle, Key(name), value ? 1 : 0), $"SetBool({name})");

    /// <summary>Set a <c>std::string</c> field by name.</summary>
    public void SetString(string name, string value) =>
        Check(Native.bison_set_string(_handle, Key(name), value), $"SetString({name})");

    /// <summary>
    /// Set a nested <see cref="Dynamic"/> field by name.
    /// The native ref-count of <paramref name="value"/> is incremented.
    /// </summary>
    public void SetObject(string name, Dynamic? value)
    {
        var vPtr = value?._handle ?? IntPtr.Zero;
        Check(Native.bison_set_object(_handle, Key(name), vPtr), $"SetObject({name})");
    }

    // ── Indexed field setters ─────────────────────────────────────────────────

    /// <summary>Set an <c>int32_t</c> field at a zero-based numeric index.</summary>
    public void SetIntAt(int index, int value) =>
        Check(Native.bison_set_int_at(_handle, (UIntPtr)index, value),
              $"SetIntAt[{index}]");

    /// <summary>Set a <c>float</c> field at a zero-based numeric index.</summary>
    public void SetFloatAt(int index, float value) =>
        Check(Native.bison_set_float_at(_handle, (UIntPtr)index, value),
              $"SetFloatAt[{index}]");

    /// <summary>Set a string field at a zero-based numeric index.</summary>
    public void SetStringAt(int index, string value) =>
        Check(Native.bison_set_string_at(_handle, (UIntPtr)index, value),
              $"SetStringAt[{index}]");

    // ── Named field getters ───────────────────────────────────────────────────

    /// <summary>Read an <c>int32_t</c> field by name.</summary>
    /// <exception cref="BisonError">If the field is missing or has the wrong type.</exception>
    public int GetInt(string name)
    {
        Check(Native.bison_get_int(_handle, Key(name), out int v), $"GetInt({name})");
        return v;
    }

    /// <summary>Read a <c>float</c> field by name.</summary>
    /// <exception cref="BisonError">If the field is missing or has the wrong type.</exception>
    public float GetFloat(string name)
    {
        Check(Native.bison_get_float(_handle, Key(name), out float v), $"GetFloat({name})");
        return v;
    }

    /// <summary>Read a <c>bool</c> field by name.</summary>
    /// <exception cref="BisonError">If the field is missing or has the wrong type.</exception>
    public bool GetBool(string name)
    {
        Check(Native.bison_get_bool(_handle, Key(name), out int v), $"GetBool({name})");
        return v != 0;
    }

    /// <summary>Read a <c>std::string</c> field by name.</summary>
    /// <exception cref="BisonError">If the field is missing or has the wrong type.</exception>
    public string GetString(string name)
    {
        uint k = Key(name);

        // First call: query length.
        Check(Native.bison_get_string(_handle, k, IntPtr.Zero, UIntPtr.Zero, out var lenOut),
              $"GetString({name}) length query");

        int len = (int)(ulong)lenOut;
        if (len == 0) return string.Empty;

        // Second call: read data.
        IntPtr buf = Marshal.AllocHGlobal(len + 1);
        try
        {
            Check(Native.bison_get_string(_handle, k, buf, (UIntPtr)(len + 1), out _),
                  $"GetString({name})");
            return Marshal.PtrToStringUTF8(buf, len) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    /// <summary>
    /// Read a nested <see cref="Dynamic"/> object field by name.
    /// The returned instance (ref-count 1) must be disposed by the caller.
    /// </summary>
    /// <exception cref="BisonError">If the field is missing or has the wrong type.</exception>
    public Dynamic? GetObject(string name)
    {
        Check(Native.bison_get_object(_handle, Key(name), out IntPtr childHandle),
              $"GetObject({name})");
        if (childHandle == IntPtr.Zero) return null;
        return new Dynamic(childHandle, true);
    }

    // ── Indexed field getters ─────────────────────────────────────────────────

    /// <summary>Read an <c>int32_t</c> field at a zero-based numeric index.</summary>
    public int GetIntAt(int index)
    {
        Check(Native.bison_get_int_at(_handle, (UIntPtr)index, out int v),
              $"GetIntAt[{index}]");
        return v;
    }

    /// <summary>Read a <c>float</c> field at a zero-based numeric index.</summary>
    public float GetFloatAt(int index)
    {
        Check(Native.bison_get_float_at(_handle, (UIntPtr)index, out float v),
              $"GetFloatAt[{index}]");
        return v;
    }

    /// <summary>Read a string field at a zero-based numeric index.</summary>
    public string GetStringAt(int index)
    {
        Check(Native.bison_get_string_at(_handle, (UIntPtr)index,
                  IntPtr.Zero, UIntPtr.Zero, out var lenOut),
              $"GetStringAt[{index}] length query");

        int len = (int)(ulong)lenOut;
        if (len == 0) return string.Empty;

        IntPtr buf = Marshal.AllocHGlobal(len + 1);
        try
        {
            Check(Native.bison_get_string_at(_handle, (UIntPtr)index,
                      buf, (UIntPtr)(len + 1), out _),
                  $"GetStringAt[{index}]");
            return Marshal.PtrToStringUTF8(buf, len) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buf);
        }
    }

    /// <summary>Return the number of array-like (numeric-key) elements.</summary>
    public long Size => (long)(ulong)Native.bison_size(_handle);

    // ── Methods ───────────────────────────────────────────────────────────────

    /// <summary>
    /// Register a .NET delegate as a named method on this object.
    /// </summary>
    /// <remarks>
    /// The delegate must have the signature
    /// <c>Dynamic? Method(Dynamic self, Dynamic params)</c>.
    /// The returned <see cref="Dynamic"/> (if non-null) should be disposed by
    /// the delegate.  The delegate reference is kept alive for the lifetime of
    /// this object.
    /// </remarks>
    /// <param name="name">Method name.</param>
    /// <param name="fn">Delegate implementing the method.</param>
    /// <exception cref="BisonError">If the name is already registered.</exception>
    public void AddMethod(string name, Func<Dynamic, Dynamic, Dynamic?> fn)
    {
        Native.MethodDelegate cb = (selfPtr, paramsPtr, resultPtr, user) =>
        {
            var selfDyn   = new Dynamic(selfPtr,   false);
            var paramsDyn = new Dynamic(paramsPtr, false);
            var resultDyn = new Dynamic(resultPtr, false);
            try
            {
                using Dynamic? ret = fn(selfDyn, paramsDyn);
                if (ret is not null)
                {
                    // Copy indexed fields from ret into result (best effort).
                    long sz = ret.Size;
                    for (int i = 0; i < sz; i++)
                    {
                        try { resultDyn.SetIntAt(i, ret.GetIntAt(i)); continue; }
                        catch (BisonError) { /* wrong type, try next */ }
                        try { resultDyn.SetFloatAt(i, ret.GetFloatAt(i)); continue; }
                        catch (BisonError) { /* wrong type, try next */ }
                        try { resultDyn.SetStringAt(i, ret.GetStringAt(i)); }
                        catch (BisonError) { /* give up */ }
                    }
                }
            }
            catch { /* Swallow .NET exceptions – cannot propagate across C ABI. */ }
        };
        _callbacks.Add(cb);  // keep alive
        Check(Native.bison_add_method(_handle, Key(name), cb, IntPtr.Zero, IntPtr.Zero),
              $"AddMethod({name})");
    }

    /// <summary>
    /// Invoke a named method on this object.
    /// </summary>
    /// <remarks>
    /// The returned <see cref="Dynamic"/> (ref-count 1) must be disposed by
    /// the caller.
    /// </remarks>
    /// <param name="name">Method name.</param>
    /// <param name="params">Argument object (may be an empty <see cref="Dynamic"/>).</param>
    /// <returns>Return value of the method.</returns>
    /// <exception cref="BisonError">If the method is not found.</exception>
    public Dynamic Call(string name, Dynamic @params)
    {
        Check(Native.bison_call(_handle, Key(name), @params._handle, out IntPtr resultHandle),
              $"Call({name})");
        return new Dynamic(resultHandle, true);
    }

    // ── Class registry helpers ────────────────────────────────────────────────

    /// <summary>
    /// Register this object as a class prototype in the global registry.
    /// </summary>
    /// <param name="parentName">
    /// Parent class name (pass <c>""</c> or <c>null</c> for root classes).
    /// </param>
    /// <exception cref="BisonError">If the class is already registered.</exception>
    public void AddClass(string? parentName = null)
    {
        uint parentKey = string.IsNullOrEmpty(parentName)
                         ? 0u : BisonKey.Of(parentName);
        Check(Native.bison_add_class(0u, _handle, parentKey, IntPtr.Zero), "AddClass");
    }

    // ── Factory methods ───────────────────────────────────────────────────────

    /// <summary>
    /// Parse a JSON string and return the root object as a <see cref="Dynamic"/>.
    /// The caller is responsible for disposing the result.
    /// </summary>
    /// <param name="json">Valid JSON string.</param>
    /// <exception cref="BisonError">If the JSON is invalid.</exception>
    public static Dynamic FromJson(string json)
    {
        var h = Native.bison_from_json(json);
        if (h == IntPtr.Zero) throw new BisonError(Native.BISON_ERR_PARSE, "FromJson");
        return new Dynamic(h, true);
    }

    /// <summary>
    /// Parse a YAML string and return the root object as a <see cref="Dynamic"/>.
    /// The caller is responsible for disposing the result.
    /// </summary>
    /// <param name="yaml">Valid YAML string (UTF-8).</param>
    /// <exception cref="BisonError">If the YAML is invalid.</exception>
    public static Dynamic FromYaml(string yaml)
    {
        var h = Native.bison_from_yaml(yaml);
        if (h == IntPtr.Zero) throw new BisonError(Native.BISON_ERR_PARSE, "FromYaml");
        return new Dynamic(h, true);
    }

    // ── ToString ──────────────────────────────────────────────────────────────

    /// <inheritdoc/>
    public override string ToString() =>
        _handle == IntPtr.Zero
        ? "Dynamic(disposed)"
        : $"Dynamic(handle=0x{_handle:x}, size={Size})";
}
