// MIT License © 2025 Binary Dice Games
/**
 * @file Program.cs
 * @brief RMI client example using the Bison C# binding.
 *
 * Mirrors examples/rmi_abi_client_example.cpp and
 * bindings/python/examples/rmi_client_example.py. Run RmiServerExample (or
 * any other Calculator server, e.g. rmi_abi_server_example) with matching
 * flags before starting this client.
 *
 * Run with:  dotnet run --project bindings/csharp/examples/RmiClientExample -- [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]
 */

using Bdg.Bison.Rmi;

var options = CliOptions.Parse(args, defaultHost: "127.0.0.1", defaultPort: 7070);

using var client = options.Transport == "pipe" ? Client.Pipe(options.Name) : Client.Tcp(options.Host, (ushort)options.Port);
client.Connect();

using var calc = client.Instantiate("Calculator");
Console.WriteLine("[Client] connected");

var r = calc.Call("add", new Dictionary<string, object?> { ["a"] = 10.0f, ["b"] = 3.0f });
Console.WriteLine($"[Client] add(10, 3) = {(float)r["result"]!:F0}");
r.Release();

r = calc.Call("subtract", new Dictionary<string, object?> { ["a"] = 100.0f, ["b"] = 21.0f });
Console.WriteLine($"[Client] subtract(100, 21) = {(float)r["result"]!:F0}");
r.Release();

r = calc.Call("multiply", new Dictionary<string, object?> { ["a"] = 7.0f, ["b"] = 6.0f });
Console.WriteLine($"[Client] multiply(7, 6) = {(float)r["result"]!:F0}");
r.Release();

r = calc.Call("divide", new Dictionary<string, object?> { ["a"] = 42.0f, ["b"] = 2.0f });
Console.WriteLine($"[Client] divide(42, 2) = {(float)r["result"]!:F0}");
r.Release();

Console.WriteLine("[Client] done.");

/// <summary>Shared --transport/--host/--port/--name flag parsing for the RMI examples.</summary>
internal sealed record CliOptions(string Transport, string Host, int Port, string Name)
{
    public static CliOptions Parse(string[] args, string defaultHost, int defaultPort)
    {
        var transport = "tcp";
        var host = defaultHost;
        var port = defaultPort;
        var name = "";

        foreach (var arg in args)
        {
            if (arg.StartsWith("--transport=")) transport = arg["--transport=".Length..];
            else if (arg.StartsWith("--host=")) host = arg["--host=".Length..];
            else if (arg.StartsWith("--port=")) port = int.Parse(arg["--port=".Length..]);
            else if (arg.StartsWith("--name=")) name = arg["--name=".Length..];
        }
        return new CliOptions(transport, host, port, name);
    }
}
