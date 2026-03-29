// Top-level entry point for the csharp/ project.
// Run with:   dotnet run -- examples
//
// Build bison_c first:
//   cmake -B ../build -DPACKAGE_TESTS=ON
//   cmake --build ../build --config Debug --target bison_c

if (args.Length > 0 && args[0] == "examples")
    Examples.Run();
else
    Console.WriteLine("Usage: dotnet run -- examples");
