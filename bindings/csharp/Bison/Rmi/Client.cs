// MIT License © 2025 Binary Dice Games
/**
 * @file Client.cs
 * @brief RAII wrapper around `rmi_client_handle`.
 */

namespace Bdg.Bison.Rmi;

/// <summary>
/// RAII wrapper around an <c>rmi_client_handle</c>. Construct via
/// <see cref="Tcp"/>, <see cref="Pipe"/>, <see cref="Term"/>, or
/// <see cref="Standalone"/>; dispose to disconnect/release.
/// </summary>
public sealed class Client : IDisposable
{
    private nint _handle;
    private bool _connected;

    private Client(nint handle) => _handle = handle;

    public static Client Tcp(string host, ushort port)
    {
        var h = Native.rmi_client_tcp_create(host, port);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_client_tcp_create failed");
        }
        return new Client(h);
    }

    public static Client Pipe(string path)
    {
        var h = Native.rmi_client_pipe_create(path);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_client_pipe_create failed");
        }
        return new Client(h);
    }

    public static Client Term()
    {
        var h = Native.rmi_client_term_create();
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_client_term_create failed");
        }
        return new Client(h);
    }

    /// <summary>In-process client dispatching directly to the local class registry.</summary>
    public static Client Standalone()
    {
        var h = Native.rmi_standalone_create();
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_standalone_create failed");
        }
        return new Client(h);
    }

    public Client Connect(object? parameters = null)
    {
        using var scope = ParamsMarshal.From(parameters);
        RmiException.Check(Native.rmi_client_connect(_handle, scope.Handle), "connect");
        _connected = true;
        return this;
    }

    public void Disconnect()
    {
        if (_connected)
        {
            RmiException.Check(Native.rmi_client_disconnect(_handle), "disconnect");
            _connected = false;
        }
    }

    public void Release()
    {
        if (_handle != nint.Zero)
        {
            Native.rmi_client_release(_handle);
            _handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Disconnect();
        Release();
        GC.SuppressFinalize(this);
    }

    ~Client() => Release();

    public Proxy Instantiate(string klassName, string nsName = "", object? parameters = null)
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        using var scope = ParamsMarshal.From(parameters);
        RmiException.Check(
            Native.rmi_client_instantiate(_handle, nsKey, Key.Of(klassName), scope.Handle, out var outHandle),
            $"instantiate({klassName})");
        return new Proxy(outHandle);
    }

    public Future InstantiateAsync(string klassName, string nsName = "", object? parameters = null)
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        using var scope = ParamsMarshal.From(parameters);
        RmiException.Check(
            Native.rmi_client_instantiate_async(_handle, nsKey, Key.Of(klassName), scope.Handle, out var outHandle),
            $"instantiate_async({klassName})");
        return new Future(outHandle);
    }

    /// <summary>Fetches class metadata. An empty <paramref name="klassName"/> queries all metadata.</summary>
    public Dynamic Describe(string klassName = "", string nsName = "")
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        var klassKey = klassName.Length > 0 ? Key.Of(klassName) : 0;
        RmiException.Check(Native.rmi_client_describe(_handle, nsKey, klassKey, out var outHandle), "describe");
        return new Dynamic(outHandle);
    }

    public Future DescribeAsync(string klassName = "", string nsName = "")
    {
        var nsKey = nsName.Length > 0 ? Key.Of(nsName) : 0;
        var klassKey = klassName.Length > 0 ? Key.Of(klassName) : 0;
        RmiException.Check(Native.rmi_client_describe_async(_handle, nsKey, klassKey, out var outHandle), "describe_async");
        return new Future(outHandle);
    }
}
