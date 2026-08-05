// MIT License © 2025 Binary Dice Games
/**
 * @file RmiTests.cs
 * @brief Tests for Bdg.Bison.Rmi -- mirrors bindings/python/tests/test_rmi.py.
 *
 * RmiTests uses Client.Standalone() (in-process dispatch) exclusively so the
 * suite has no dependency on sockets/ports being available in the test
 * environment. RmiAuthTests is the exception -- Server.SetAuth() is
 * evaluated during the OP_CONNECT handshake, which standalone sessions skip
 * entirely, so it needs a real TCP client/server round trip.
 */

using Bdg.Bison;
using Bdg.Bison.Rmi;
using Xunit;

namespace Bdg.Bison.Tests;

public class RmiTests : IDisposable
{
    public RmiTests()
    {
        Dynamic.ClearRegistry();
        RegisterCalculator();
    }

    public void Dispose() => Dynamic.ClearRegistry();

    private static void RegisterCalculator()
    {
        var proto = new Dynamic("Calculator");
        proto.AddMethod("add", (self, p, result) => result["result"] = (float)p["a"]! + (float)p["b"]!);
        proto.AddMethod("echo", (self, p, result) => result["value"] = p["value"]);
        Dynamic.AddClass(proto);
        proto.Release();
    }

    [Fact]
    public void InstantiateAndCall()
    {
        using var client = Client.Standalone();
        client.Connect();
        using var calc = client.Instantiate("Calculator");
        var r = calc.Call("add", new Dictionary<string, object?> { ["a"] = 10.0f, ["b"] = 3.0f });
        Assert.Equal(13.0f, r["result"]);
        r.Release();
    }

    [Fact]
    public void GetItemSetItemFieldPatch()
    {
        using var client = Client.Standalone();
        client.Connect();
        using var calc = client.Instantiate("Calculator");
        calc["label"] = "primary";
        Assert.Equal("primary", calc["label"]);
    }

    [Fact]
    public void GetFullSnapshot()
    {
        using var client = Client.Standalone();
        client.Connect();
        using var calc = client.Instantiate("Calculator");
        calc["label"] = "primary";
        using var snapshot = calc.Get();
        Assert.Equal("primary", snapshot["label"]);
    }

    [Fact]
    public void UnknownMethodRaises()
    {
        using var client = Client.Standalone();
        client.Connect();
        using var calc = client.Instantiate("Calculator");
        Assert.Throws<RmiException>(() => calc.Call("does_not_exist"));
    }

    [Fact]
    public void MultipleProxiesIndependent()
    {
        using var client = Client.Standalone();
        client.Connect();
        using var a = client.Instantiate("Calculator");
        using var b = client.Instantiate("Calculator");
        a["label"] = "a";
        b["label"] = "b";
        Assert.Equal("a", a["label"]);
        Assert.Equal("b", b["label"]);
    }

    [Fact]
    public void DynamicAttributeStyleCall()
    {
        using var client = Client.Standalone();
        client.Connect();
        using var calc = client.Instantiate("Calculator");
        // The remote method was registered as "add" (RegisterCalculator()
        // above) -- member names resolve to their exact hashed string, with
        // no C#-vs-wire casing translation, so the call must match exactly.
        dynamic d = calc;
        Dynamic r = d.add(a: 1.0f, b: 2.0f);
        Assert.Equal(3.0f, r["result"]);
        r.Release();
    }
}

public class RmiAuthTests
{
    private static int _nextPort = 30500;

    private static ushort NextPort() => (ushort)(_nextPort++);

    [Fact]
    public void NoAuthSetConnectSucceeds()
    {
        var port = NextPort();
        using var server = Server.Tcp("127.0.0.1", port);
        server.Listen();
        using var client = Client.Tcp("127.0.0.1", port);
        client.Connect();
    }

    [Fact]
    public void AcceptingCallbackReceivesPayloadAndSetsIdentity()
    {
        var port = NextPort();
        using var server = Server.Tcp("127.0.0.1", port);
        string? seenUsername = null;
        server.SetAuth(payload =>
        {
            seenUsername = (string?)payload["username"];
            return (true, "alice-id");
        });
        server.Listen();

        using var client = Client.Tcp("127.0.0.1", port);
        client.Connect(new Dictionary<string, object?> { ["username"] = "alice" });

        Assert.Equal("alice", seenUsername);
    }

    [Fact]
    public void RejectingCallbackFailsConnect()
    {
        var port = NextPort();
        using var server = Server.Tcp("127.0.0.1", port);
        server.SetAuth(payload => (false, string.Empty));
        server.Listen();

        using var client = Client.Tcp("127.0.0.1", port);
        Assert.Throws<RmiException>(() => client.Connect());
    }

    [Fact]
    public void SetAuthAfterListenThrows()
    {
        var port = NextPort();
        using var server = Server.Tcp("127.0.0.1", port);
        server.Listen();

        Assert.Throws<RmiException>(() => server.SetAuth(payload => (true, string.Empty)));
    }
}
