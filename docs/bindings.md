# Language Bindings

All bindings wrap the `bison_c` shared library. Build it first:

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_c
```

Set `BISON_LIB` to the full path of the shared library if it is not found automatically:

```bash
# Windows:
$env:BISON_LIB = (Resolve-Path .\build\Debug\bison_c.dll)
# Linux:
export BISON_LIB=$(pwd)/build/libbison_c.so
# macOS:
export BISON_LIB=$(pwd)/build/libbison_c.dylib
```

---

## Python (`bindings/python/`)

Thin `ctypes` wrapper. No installation needed — import directly.

**Requirements:** Python 3.x, `pytest` (optional, for tests)

```bash
# Run examples:
python bindings/python/examples.py

# Run tests:
python -m pytest bindings/python/test_bison.py -v
python -m unittest bindings.python.test_bison
```

---

## Java (`bindings/java/`)

JNA binding exposing a `Dynamic` class.

**Requirements:** Java 17+, Maven 3.6+ (JNA 5.14 and JUnit 5.10 fetched automatically)

```bash
cd bindings/java
mvn compile exec:java "-Dexec.mainClass=com.bdg.bison.examples.BisonExamples"
mvn test
```

Quick-start snippet:

```java
try (Dynamic obj = new Dynamic()) {
    obj.setInt("hp",      100);
    obj.setFloat("speed", 9.5f);
    obj.setBool("alive",  true);
    obj.setString("name", "hero");
    System.out.println(obj.getInt("hp"));  // 100
}

try (Dynamic root = Dynamic.fromJson("{\"x\": 1, \"y\": 2}")) {
    System.out.println(root.getInt("x")); // 1
}
```

---

## C# (`bindings/csharp/`)

P/Invoke binding for .NET 6+. `Dynamic` implements `IDisposable`.

**Requirements:** .NET SDK 6.0+ (xUnit 2.7 fetched automatically by NuGet)

```bash
cd bindings/csharp
dotnet run -- examples

cd bindings/csharp/tests
dotnet test
```

Quick-start snippet:

```csharp
using Bdg.Bison;

using var obj = new Dynamic();
obj.SetInt("hp",      100);
obj.SetFloat("speed", 9.5f);
obj.SetBool("alive",  true);
obj.SetString("name", "hero");
Console.WriteLine(obj.GetInt("hp"));      // 100

using var root = Dynamic.FromJson("{\"x\": 1, \"y\": 2}");
Console.WriteLine(root.GetInt("x")); // 1
```
