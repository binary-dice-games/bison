// MIT License © 2025 Binary Dice Games
// Grants the wish C# binding (Bdg.Wish, in the sibling wish repo) access to
// Dynamic/Proxy/Future's internal raw-handle constructors. wish's own C ABI
// (wish_client_c.h) returns plain bison_handle/rmi_proxy_handle/
// rmi_future_handle values -- the exact same types this library already
// wraps -- so Bdg.Wish needs to construct these wrappers around handles it
// received directly from wish_* calls, mirroring how bison.rmi.Proxy(handle)
// is called directly from wish/client.py in the Python bindings.
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Bdg.Wish")]
