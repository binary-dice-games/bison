// MIT License © 2025 Binary Dice Games
/**
 * @file Program.cs
 * @brief RMI server example using the Bison C# binding.
 *
 * Mirrors examples/rmi_abi_server_example.cpp and
 * bindings/python/examples/rmi_server_example.py.
 *
 * Run with:  dotnet run --project bindings/csharp/examples/RmiServerExample -- [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]
 */

using Bdg.Bison;
using Bdg.Bison.Rmi;

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

var options = CliOptions.Parse(args, defaultHost: "0.0.0.0", defaultPort: 7070);

RegisterCalculator();

using var server = options.Transport == "pipe" ? Server.Pipe(options.Name) : Server.Tcp(options.Host, (ushort)options.Port);
server.Listen();
Console.WriteLine(options.Transport == "pipe"
    ? $"[Server] Calculator listening on pipe {options.Name}"
    : $"[Server] Calculator listening on {options.Host}:{options.Port}");
Console.WriteLine("[Server] Press Enter to stop...");

Console.ReadLine();

server.Stop();
Console.WriteLine("[Server] stopped.");

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
