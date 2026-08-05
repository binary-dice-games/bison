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
    private readonly List<object> _callbacks = new(); // keep native-callback delegates alive

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

    /// <summary>
    /// Registers a connection-authentication callback; call before <see cref="Listen"/>.
    /// <paramref name="handler"/> receives the client's <c>OP_CONNECT</c> payload and
    /// returns whether to accept the connection and (if accepted) an identity string.
    /// </summary>
    public unsafe void SetAuth(Func<Dynamic, (bool Accepted, string Identity)> handler)
    {
        bool Trampoline(nint payloadHandle, byte* identityBuf, nuint identityBufLen, nint _)
        {
            try
            {
                var (accepted, identity) = handler(new Dynamic(payloadHandle, owned: false));
                if (accepted && !string.IsNullOrEmpty(identity) && identityBufLen > 0)
                {
                    var bytes = System.Text.Encoding.UTF8.GetBytes(identity);
                    var n = Math.Min(bytes.Length, (int)identityBufLen - 1);
                    for (var i = 0; i < n; i++) identityBuf[i] = bytes[i];
                    identityBuf[n] = 0;
                }
                return accepted;
            }
            catch
            {
                // C ABI boundary: exceptions must not cross back into C++.
                return false;
            }
        }

        NativeAuthFn native = Trampoline;
        _callbacks.Add(native);
        var fnPtr = System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(native);
        RmiException.Check(Native.rmi_server_set_auth(_handle, fnPtr, nint.Zero), "set_auth");
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
