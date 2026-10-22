#pragma once

// Phase 29: the Service Control Manager's side of `--service`, and nothing
// else. Windows only; systemd needs no code, because it already sends SIGTERM
// and reads stderr.
//
// There is one stop path and this does not add a second. The SCM's stop and
// shutdown controls call `requestStop` -- the same flag SIGINT and SIGTERM
// set -- and report STOP_PENDING; `run` is main()'s own body, which notices
// the flag and runs the shutdown order as it does from a console. When it
// returns, its exit code is what the SCM is told, so a server that failed to
// start shows as a failed service rather than a stopped one.

#if defined(_WIN32)

#include <functional>

namespace multiroom::service {

// Blocks until `run` has returned on the SCM's service thread, and returns its
// exit code. Started from a console rather than by the SCM, it says so on
// stderr and returns 2 without calling `run`.
int runAsWindowsService(const std::function<int()>& run, void (*requestStop)());

} // namespace multiroom::service

#endif
