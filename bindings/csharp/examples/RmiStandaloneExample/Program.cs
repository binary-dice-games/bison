// MIT License © 2025 Binary Dice Games
/**
 * @file Program.cs
 * @brief Standalone RMI example using the Bison C# binding.
 *
 * Mirrors examples/rmi_abi_standalone_example.cpp and
 * bindings/python/examples/rmi_standalone_example.py: no separate server
 * process -- Client.Standalone() dispatches directly to the local
 * (in-process) class registry. Three worker threads each instantiate a
 * remote Calculator, perform several operations concurrently, then clean up.
 *
 * Run with:  dotnet run --project bindings/csharp/examples/RmiStandaloneExample
 */

using Bdg.Bison;
using Bdg.Bison.Rmi;

var printLock = new object();

static void RegisterCalculator()
{
    var proto = new Dynamic("Calculator");
    proto.AddMethod("add", (self, p, result) => result["result"] = (float)p["a"]! + (float)p["b"]!);
    proto.AddMethod("subtract", (self, p, result) => result["result"] = (float)p["a"]! - (float)p["b"]!);
    proto.AddMethod("multiply", (self, p, result) => result["result"] = (float)p["a"]! * (float)p["b"]!);
    proto.AddMethod("divide", (self, p, result) =>
    {
        var a = (float)p["a"]!;
        var b = (float)p["b"]!;
        if (b == 0.0f)
        {
            result["error"] = "division by zero";
            result["result"] = 0.0f;
        }
        else
        {
            result["result"] = a / b;
        }
    });
    Dynamic.AddClass(proto);
    proto.Release();
}

void RunClient(int clientId)
{
    using var client = Client.Standalone();
    client.Connect();
    using var calc = client.Instantiate("Calculator");

    lock (printLock)
    {
        Console.WriteLine($"[Client {clientId}] connected");
    }

    var a = 10.0f * clientId;
    var r = calc.Call("add", new Dictionary<string, object?> { ["a"] = a, ["b"] = 3.0f });
    lock (printLock)
    {
        Console.WriteLine($"[Client {clientId}] add({a:F0}, 3) = {(float)r["result"]!:F0}");
    }
    r.Release();

    var b = 7.0f * clientId;
    r = calc.Call("subtract", new Dictionary<string, object?> { ["a"] = 100.0f, ["b"] = b });
    lock (printLock)
    {
        Console.WriteLine($"[Client {clientId}] subtract(100, {b:F0}) = {(float)r["result"]!:F0}");
    }
    r.Release();

    var v = (float)clientId;
    r = calc.Call("multiply", new Dictionary<string, object?> { ["a"] = v, ["b"] = v });
    lock (printLock)
    {
        Console.WriteLine($"[Client {clientId}] multiply({v:F0}, {v:F0}) = {(float)r["result"]!:F0}");
    }
    r.Release();

    var divisor = (float)clientId; // non-zero since clientId >= 1
    r = calc.Call("divide", new Dictionary<string, object?> { ["a"] = 42.0f, ["b"] = divisor });
    lock (printLock)
    {
        Console.WriteLine($"[Client {clientId}] divide(42, {divisor:F0}) = {(float)r["result"]!:F0}");
    }
    r.Release();

    lock (printLock)
    {
        Console.WriteLine($"[Client {clientId}] done.");
    }
}

RegisterCalculator();
Console.WriteLine("[Server] RMI Calculator registered (standalone in-process mode).");

var threads = Enumerable.Range(1, 3).Select(i => new Thread(() => RunClient(i))).ToList();
foreach (var t in threads) t.Start();
foreach (var t in threads) t.Join();

Console.WriteLine("[Server] all clients done.");
