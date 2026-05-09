# Smoke notes - Task 10

Verified host -> notepad attach with empty napi symbols. Pipe path
`\\.\pipe\positron-<pid>`. No napi env captured (expected, no electron).

## Test environment

- Windows 11 Pro 10.0.26200
- Build: x64 Debug (`x64\Debug\host.exe`, `x64\Debug\payload.dll`)
- Notepad PID: 63568 (varies)
- End-to-end elapsed: ~137 ms
- Unit tests: 7/7 passing (`unit_host.exe`)

## Captured host output

```
inject OK
HELLO: {"electron_version":"","kind":"hello","napi_symbols_present":[],"pid":63568,"target_type":""}
```

Exit code 0; notepad still alive after the test (cleaned up via `Stop-Process`).

Also verified the same flow against `cmd.exe` as the alternate target — identical
result (`HELLO` received, target alive).

## Bug found and fixed during the smoke

The first run reported `inject OK` followed by `no HELLO received` even with a
15 s receive timeout. Adding diagnostic logging traced the failure to a
threading bug in the IPC server:

- The accept thread called `ConnectNamedPipe` and then entered a synchronous
  `ReadFile` loop on the same handle.
- The bootstrap thread, on a different OS thread, then called `WriteFile` on
  the same synchronous handle to push HELLO. WriteFile failed with
  `ERROR_NO_DATA` (232) and the read loop saw `ERROR_BROKEN_PIPE` (109).
- Concurrent ReadFile + WriteFile on a synchronous (non-overlapped) pipe
  handle from two different threads is not reliable on Win32; the kernel ends
  up tearing the pipe down rather than serving both I/Os.
- A direct WriteFile from the same thread that called ConnectNamedPipe
  succeeded (verified with a one-off diagnostic write).

Fix: the IPC server now exposes `Server::set_greeter(Greeter)` and writes the
greeting frame from the accept thread itself, immediately after
`ConnectNamedPipe` returns and before the read loop starts. Bootstrap supplies
the HELLO via the greeter callback (PID, NAPI symbols, electron version,
target type) instead of polling `is_connected()` and pushing on its own thread.

This keeps WriteFile and ReadFile serialised on the accept thread for the
greeting, while still allowing later async pushes from `Server::push` once a
proper write strategy (overlapped I/O or accept-thread-only writes) is in
place. For now `push` retains its WriteFile-from-any-thread implementation; it
is unused on the connection path but should be revisited when we add bidir
async pushes (Task 11+).

Files touched by the fix:

- `payload/src/ipc_server/ipc_server.h`
- `payload/src/ipc_server/ipc_server.cpp`
- `payload/src/bootstrap/init.cpp`
