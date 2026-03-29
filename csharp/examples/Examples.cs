using Bdg.Bison;

/// <summary>
/// Usage examples for the Bison C# binding.
/// </summary>
/// <remarks>
/// Build <c>bison_c</c> first, then run from the repository root:
/// <code>
/// cmake -B build -DPACKAGE_TESTS=ON
/// cmake --build build --config Debug --target bison_c
/// cd csharp
/// dotnet run --project Bison.csproj -- examples
/// </code>
/// </remarks>
internal static class Examples
{
    // ── Helpers ───────────────────────────────────────────────────────────────

    private static void Section(string title)
    {
        Console.WriteLine();
        Console.WriteLine("------------------------------------------------------------");
        Console.WriteLine($"  {title}");
        Console.WriteLine("------------------------------------------------------------");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 1 — Basic field get/set
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example1BasicFields()
    {
        Section("Example 1: basic field get/set");

        using var obj = new Dynamic();
        obj.SetInt("hp",     100);
        obj.SetFloat("speed", 9.5f);
        obj.SetBool("alive", true);
        obj.SetString("name", "hero");

        Console.WriteLine($"hp    = {obj.GetInt("hp")}");
        Console.WriteLine($"speed = {obj.GetFloat("speed"):F2}");
        Console.WriteLine($"alive = {obj.GetBool("alive")}");
        Console.WriteLine($"name  = {obj.GetString("name")}");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 2 — Numeric (array-like) indices
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example2ArrayAccess()
    {
        Section("Example 2: array-like indexed fields");

        using var arr = new Dynamic();
        arr.SetStringAt(0, "apple");
        arr.SetStringAt(1, "banana");
        arr.SetStringAt(2, "cherry");

        Console.WriteLine($"size = {arr.Size}");
        for (int i = 0; i < arr.Size; i++)
            Console.WriteLine($"  [{i}] = {arr.GetStringAt(i)}");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 3 — Nested objects
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example3NestedObjects()
    {
        Section("Example 3: nested objects");

        using var player = new Dynamic();
        player.SetString("name", "alice");

        using (var pos = new Dynamic())
        {
            pos.SetFloat("x", 1.0f);
            pos.SetFloat("y", 2.5f);
            player.SetObject("position", pos);
        }

        using var retrieved = player.GetObject("position")!;
        Console.WriteLine($"pos.x = {retrieved.GetFloat("x"):F1}"); // 1.0
        Console.WriteLine($"pos.y = {retrieved.GetFloat("y"):F1}"); // 2.5
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 4 — JSON import
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example4JsonImport()
    {
        Section("Example 4: JSON import");

        const string jsonText =
            "{\"player\": {\"name\": \"bob\", \"score\": 250}, \"level\": 3}";

        using var root = Dynamic.FromJson(jsonText);
        Console.WriteLine($"level = {root.GetInt("level")}");

        using var player = root.GetObject("player")!;
        Console.WriteLine($"player.name  = {player.GetString("name")}");
        Console.WriteLine($"player.score = {player.GetInt("score")}");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 5 — YAML import
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example5YamlImport()
    {
        Section("Example 5: YAML import");

        const string yamlText =
            "server:\n"             +
            "  host: localhost\n"   +
            "  port: 8080\n"        +
            "debug: true\n"         +
            "max_connections: 100\n";

        using var cfg = Dynamic.FromYaml(yamlText);
        using var server = cfg.GetObject("server")!;
        Console.WriteLine($"host = {server.GetString("host")}");
        Console.WriteLine($"port = {server.GetInt("port")}");
        Console.WriteLine($"debug           = {cfg.GetBool("debug")}");
        Console.WriteLine($"max_connections = {cfg.GetInt("max_connections")}");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 6 — Method registration
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example6Methods()
    {
        Section("Example 6: method registration and invocation");

        using var obj = new Dynamic();
        obj.SetInt("counter", 5);

        obj.AddMethod("double", (self, _params) =>
        {
            int current = self.GetInt("counter");
            self.SetInt("counter", current * 2);
            return new Dynamic();
        });

        using var p = new Dynamic();
        using var result = obj.Call("double", p);

        Console.WriteLine($"counter after double = {obj.GetInt("counter")}"); // 10
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 7 — BisonKey utility
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example7KeyUtility()
    {
        Section("Example 7: BisonKey utility");

        uint k = BisonKey.Of("velocity");
        Console.WriteLine($"key('velocity') = 0x{k:x8}");
        Console.WriteLine($"high bit set    = {(k & 0x80000000u) != 0}");
        Console.WriteLine($"deterministic   = {k == BisonKey.Of("velocity")}");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 8 — Class hierarchy
    // ═════════════════════════════════════════════════════════════════════════

    internal static void Example8ClassRegistry()
    {
        Section("Example 8: class registry");

        using (var proto = new Dynamic("CSharpVehicle"))
        {
            proto.SetInt("wheels", 4);
            try { proto.AddClass(); }
            catch (BisonError) { /* already registered */ }
        }

        using var car = new Dynamic("CSharpVehicle");
        Console.WriteLine($"wheels (inherited) = {car.GetInt("wheels")}"); // 4

        using var car2 = car.AddRef();
        car.SetInt("wheels", 3);
        Console.WriteLine($"car.wheels  = {car.GetInt("wheels")}");   // 3
        Console.WriteLine($"car2.wheels = {car2.GetInt("wheels")}");  // 3 – shared!
    }

    // ── Entry point ───────────────────────────────────────────────────────────

    internal static void Run()
    {
        Console.WriteLine("Bison C# binding -- usage examples");
        Example1BasicFields();
        Example2ArrayAccess();
        Example3NestedObjects();
        Example4JsonImport();
        Example5YamlImport();
        Example6Methods();
        Example7KeyUtility();
        Example8ClassRegistry();
        Console.WriteLine("\nAll examples completed successfully.");
    }
}
