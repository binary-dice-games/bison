// MIT License © 2025 Binary Dice Games
/**
 * @file Server.cs
 * @brief RAII wrapper around `rmi_server_handle`.
 */

namespace Bdg.Bison.Rmi;

/// <summary>
/// RAII wrapper around an <c>rmi_server_handle</c>. Construct via
/// <see cref="Tcp"/>, <see cref="Pipe"/>, or <see cref="Term"/>; dispose to
/// stop/release.
/// </summary>
public sealed class Server : IDisposable
{
    private nint _handle;
    private bool _listening;

    private Server(nint handle) => _handle = handle;

    public static Server Tcp(string host, ushort port)
    {
        var h = Native.rmi_server_tcp_create(host, port);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_server_tcp_create failed");
        }
        return new Server(h);
    }

    public static Server Pipe(string path)
    {
        var h = Native.rmi_server_pipe_create(path);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_server_pipe_create failed");
        }
        return new Server(h);
    }

    public static Server Term(string? cmd = null)
    {
        var h = Native.rmi_server_term_create(cmd);
        if (h == nint.Zero)
        {
            throw new OutOfMemoryException("rmi_server_term_create failed");
        }
        return new Server(h);
    }

    public Server Listen(object? parameters = null)
    {
        using var scope = ParamsMarshal.From(parameters);
        RmiException.Check(Native.rmi_server_listen(_handle, scope.Handle), "listen");
        _listening = true;
        return this;
    }

    public void Stop()
    {
        if (_listening)
        {
            Native.rmi_server_stop(_handle);
            _listening = false;
        }
    }

    public void Release()
    {
        if (_handle != nint.Zero)
        {
            Native.rmi_server_release(_handle);
            _handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Stop();
        Release();
        GC.SuppressFinalize(this);
    }

    ~Server() => Release();
}
