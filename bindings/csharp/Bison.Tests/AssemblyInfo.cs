// MIT License © 2025 Binary Dice Games
// Bison's class registry and Key cache are process-global state (matching
// the C++ singleton registry behind bison_add_class()/bison_key()), so
// tests that touch them must not run concurrently -- mirrors how the
// Python suite (unittest, single-threaded by default) exercises them.
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]
