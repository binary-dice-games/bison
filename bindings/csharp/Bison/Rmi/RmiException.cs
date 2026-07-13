// MIT License © 2025 Binary Dice Games
/**
 * @file RmiException.cs
 * @brief Exception type raised when an `rmi_*` C API call returns a
 *        non-zero error code.
 */

namespace Bdg.Bison.Rmi;

/// <summary>Raised when an <c>rmi_*</c> C API call returns a non-zero error code.</summary>
public sealed class RmiException : Exception
{
    private static readonly Dictionary<RmiErrorCode, string> Messages = new()
    {
        [RmiErrorCode.Null] = "Null handle or pointer",
        [RmiErrorCode.InvalidState] = "Operation invalid for current state",
        [RmiErrorCode.Timeout] = "Request timed out",
        [RmiErrorCode.RemoteException] = "Server raised an exception",
        [RmiErrorCode.Transport] = "Transport error",
        [RmiErrorCode.Exception] = "Internal C++ exception",
    };

    public RmiErrorCode Code { get; }

    public RmiException(RmiErrorCode code, string context = "")
        : base(Format(code, context))
    {
        Code = code;
    }

    private static string Format(RmiErrorCode code, string context)
    {
        var msg = Messages.TryGetValue(code, out var m) ? m : $"Unknown error {(int)code}";
        return context.Length > 0 ? $"{context}: {msg}" : msg;
    }

    internal static void Check(int rc, string context = "")
    {
        if (rc != (int)RmiErrorCode.Ok)
        {
            throw new RmiException((RmiErrorCode)rc, context);
        }
    }
}
