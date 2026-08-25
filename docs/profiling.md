# Profiling

Bison can record execution traces in Perfetto's track-event protobuf wire
format and write them to a file that opens directly at
https://ui.perfetto.dev/. This has no dependency on the Perfetto SDK or a
protobuf library — see `src/rmi/DESIGN.md` §16.1 for the architecture and
`FORMAT.md` §7 for the byte-level wire format.

## Enabling it on a server

```cpp
#include "src/rmi/rmi.hpp"

bdg::bison::rmi::server srv{transport};
srv.enable_profiling("/var/log/myapp/traces"); // output directory
srv.listen(bison::dynamic{});
```

`enable_profiling()` registers the `__BisonProfiler` singleton class and
installs a recorder for the server's own native code. It must be called
before `listen()`.

## Instrumenting code

Anywhere in server or client process code:

```cpp
#include "src/rmi/shared/profiling.hpp"

void render_frame() {
  BISON_TRACE_SCOPE("render_frame");
  // ...
  BISON_TRACE_INSTANT("frame_submitted");
}
```

`BISON_TRACE_SCOPE` records a slice spanning the enclosing scope;
`BISON_TRACE_INSTANT` records a zero-duration point event. Both are no-ops
when no capture is active, so they are safe to leave in place permanently.

Use `BISON_TRACE_COUNTER` to track a numeric value over time instead of a
per-thread slice or instant:

```cpp
BISON_TRACE_COUNTER("queue_depth", queue.size());
BISON_TRACE_COUNTER("cpu_load", 0.72); // int64 or double values
```

All samples for a given counter name share a single track in the trace,
regardless of which thread records them. Optionally set the unit shown in
the Perfetto UI before capture starts:

```cpp
if (auto* r = bdg::bison::rmi::profiling::recorder::local())
  r->set_counter_unit("queue_depth", bdg::bison::perfetto::counter_unit::count);
```

## Attaching a client recorder

A connected RMI client that wants its own code traced (in addition to the
server's) attaches once after connecting:

```cpp
#include "src/rmi/rmi.hpp"

auto recorder = bdg::bison::rmi::attach_profiling(client); // client is bdg::bison::rmi::client&
```

Keep the returned `shared_ptr<client_recorder>` alive for as long as the
client should be traced; destroying it stops recording on that client.

## Starting and stopping capture

Capture is controlled at runtime via RMI calls against the
`__BisonProfiler` singleton — it is not always-on:

```cpp
auto proxy = client.instantiate(NS_BISON, CLASS_PROFILER).get();

bison::dynamic start_params;
start_params[FIELD_PROFILER_LABEL] = std::string{"startup"};
bison::dynamic resp = proxy.call(METHOD_START_CAPTURE, std::move(start_params)).get();
std::string trace_path = resp.as<std::string>(FIELD_PROFILER_PATH);

// ... exercise the code paths you want traced ...

proxy.call(METHOD_STOP_CAPTURE, bison::dynamic{}).get();
```

`trace_path` is chosen by the server under the directory passed to
`enable_profiling()` — it is never derived from client input. After
`stop_capture()` returns, the file at `trace_path` is complete and ready to
open.

## C ABI

Third-party applications embedding Bison through the pure-C ABI
(`include/rmi_c.h`) control profiling the same way, without linking against
the C++ headers above. This surface is not exposed through the language
bindings (Python/C#/Java/etc.) — only through `rmi_c.h` directly.

```c
#include "rmi_c.h"

rmi_server_enable_profiling(server, "/var/log/myapp/traces");

bool started = false;
rmi_server_start_capture(server, "startup", &started);

// ... exercise the code paths you want traced ...
rmi_trace_scope_begin("render_frame");
rmi_trace_instant("frame_submitted");
rmi_trace_counter_int("queue_depth", 42);
rmi_trace_counter_double("cpu_load", 0.72);
rmi_trace_scope_end();

rmi_server_stop_capture(server);
```

`rmi_trace_is_active()` reports whether a capture is currently running in
this process. All `rmi_trace_*` functions are safe no-ops when no capture is
active, matching `BISON_TRACE_SCOPE`/`BISON_TRACE_INSTANT`/
`BISON_TRACE_COUNTER` above.

## Viewing a trace

Open https://ui.perfetto.dev/, choose "Open trace file", and select the
`.perfetto-trace` file written by `start_capture`/`stop_capture`. Each OS
thread that recorded at least one event appears as its own track, named
`thread-<tid>` unless overridden with `recorder::set_thread_track_name()`.
