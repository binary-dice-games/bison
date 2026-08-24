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

## Viewing a trace

Open https://ui.perfetto.dev/, choose "Open trace file", and select the
`.perfetto-trace` file written by `start_capture`/`stop_capture`. Each OS
thread that recorded at least one event appears as its own track, named
`thread-<tid>` unless overridden with `recorder::set_thread_track_name()`.
