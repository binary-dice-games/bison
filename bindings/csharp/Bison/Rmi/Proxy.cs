// MIT License © 2025 Binary Dice Games
/**
 * @file Proxy.cs
 * @brief A live handle to a remote (or in-process standalone) object.
 */

using System.Dynamic;

namespace Bdg.Bison.Rmi;

/// <summary>
/// A live handle to a remote (or in-process standalone) object.
/// <c>proxy["field"]</c> / <c>proxy["field"] = value</c> project/patch a
/// single field; via <c>dynamic</c>, <c>proxy.SomeMethod(a: 1, b: 2)</c>
/// invokes a remote method by name.
/// </summary>
public sealed class Proxy : DynamicObject, IDisposable
{
    private nint _handle;
    private readonly List<object> _callbacks = new(); // keep native-callback delegates alive

    internal Proxy(nint handle) => _handle = handle;

    public void Release()
    {
        if (_handle != nint.Zero)
        {
            Native.rmi_proxy_release(_handle);
            _handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Release();
        GC.SuppressFinalize(this);
    }

    ~Proxy() => Release();

    // ── Remote field access ──────────────────────────────────────────────────

    /// <summary>Fetches a full snapshot, or a projected subset of fields.</summary>
    public Dynamic Get(object? projection = null, long timeoutMs = -1)
    {
        using var scope = ParamsMarshal.From(projection);
        RmiException.Check(Native.rmi_proxy_get(_handle, scope.Handle, out var outHandle, timeoutMs), "proxy.get");
        return new Dynamic(outHandle);
    }

    /// <summary>Applies a partial field update without resetting unspecified fields.</summary>
    public void Set(object fields, long timeoutMs = -1)
    {
        using var scope = ParamsMarshal.From(fields);
        RmiException.Check(Native.rmi_proxy_set(_handle, scope.Handle, timeoutMs), "proxy.set");
    }

    /// <summary>Resets explicitly-set fields back to prototype/inherited defaults.</summary>
    public void Clear(long timeoutMs = -1)
    {
        RmiException.Check(Native.rmi_proxy_clear(_handle, timeoutMs), "proxy.clear");
    }

    public object? this[string name]
    {
        get
        {
            using var snapshot = Get(new Dictionary<string, object?> { [name] = true });
            return snapshot[name];
        }
        set => Set(new Dictionary<string, object?> { [name] = value });
    }

    // ── Remote method calls ──────────────────────────────────────────────────

    /// <summary>Invokes a named remote method with <paramref name="parameters"/>
    /// (a <see cref="Dynamic"/>, an <c>IDictionary&lt;string, object?&gt;</c>, or <c>null</c>).</summary>
    public Dynamic Call(string name, object? parameters = null, long timeoutMs = -1)
    {
        using var scope = ParamsMarshal.From(parameters);
        RmiException.Check(
            Native.rmi_proxy_call(_handle, Key.Of(name), scope.Handle, out var outHandle, timeoutMs),
            $"proxy.call({name})");
        return new Dynamic(outHandle);
    }

    /// <summary>Backs calls like <c>dynamic calc = ...; calc.Add(a: 10, b: 32)</c>.
    /// Only named arguments are supported.</summary>
    public override bool TryInvokeMember(InvokeMemberBinder binder, object?[] args, out object? result)
    {
        var argNames = binder.CallInfo.ArgumentNames;
        var positionalCount = args.Length - argNames.Count;
        if (positionalCount != 0)
        {
            throw new ArgumentException("Dynamic proxy calls only support named arguments, e.g. calc.Add(a: 1, b: 2)");
        }

        var kwargs = new Dictionary<string, object?>();
        for (var i = 0; i < argNames.Count; i++)
        {
            kwargs[argNames[i]] = args[i];
        }
        result = Call(binder.Name, kwargs);
        return true;
    }

    // ── Events ────────────────────────────────────────────────────────────────

    /// <summary>Subscribes to a server-initiated event on this object.</summary>
    public void OnEvent(string name, Action<Dynamic> handler)
    {
        void Trampoline(nint paramsHandle, nint _)
        {
            try
            {
                handler(new Dynamic(paramsHandle, owned: false));
            }
            catch
            {
                // C ABI boundary: exceptions must not cross back into C++.
            }
        }

        NativeProxyEventFn native = Trampoline;
        _callbacks.Add(native);
        var fnPtr = System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(native);
        RmiException.Check(Native.rmi_proxy_on_event(_handle, Key.Of(name), fnPtr, nint.Zero), $"on_event({name})");
    }
}
