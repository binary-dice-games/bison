// MIT License © 2025 Binary Dice Games
/**
 * @file Future.cs
 * @brief RAII wrapper around `rmi_future_handle`.
 */

namespace Bdg.Bison.Rmi;

/// <summary>
/// RAII wrapper around an <c>rmi_future_handle</c>. Consumed exactly once via
/// <see cref="GetDynamic"/> or <see cref="GetProxy"/>, or discarded with
/// <see cref="Release"/> / by disposing.
/// </summary>
public sealed class Future : IDisposable
{
    private nint _handle;

    internal Future(nint handle) => _handle = handle;

    public void Wait(long timeoutMs = -1)
    {
        RmiException.Check(Native.rmi_future_wait(_handle, timeoutMs), "future.wait");
    }

    public Dynamic GetDynamic()
    {
        RmiException.Check(Native.rmi_future_get_dynamic(ref _handle, out var outHandle), "future.get_dynamic");
        return new Dynamic(outHandle);
    }

    public Proxy GetProxy()
    {
        RmiException.Check(Native.rmi_future_get_proxy(ref _handle, out var outHandle), "future.get_proxy");
        return new Proxy(outHandle);
    }

    public void Release()
    {
        if (_handle != nint.Zero)
        {
            Native.rmi_future_release(_handle);
            _handle = nint.Zero;
        }
    }

    public void Dispose()
    {
        Release();
        GC.SuppressFinalize(this);
    }

    ~Future() => Release();
}
