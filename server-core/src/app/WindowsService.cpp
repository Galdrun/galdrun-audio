#include "app/WindowsService.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <mutex>

namespace multiroom::service {
namespace {

// Long enough for the shutdown order -- every zone's rooms and pipeline, then
// the listeners -- which takes well under a second on a quiet house.
constexpr DWORD kStopWaitHintMs = 10000;

// StartServiceCtrlDispatcher's ServiceMain takes no context, so what it needs
// is reached through this, set before the dispatcher starts and cleared after
// it returns.
struct State
{
    const std::function<int()>* run = nullptr;
    void (*requestStop)() = nullptr;
    int exitCode = 0;

    std::mutex mutex; // the SCM's control thread and the service thread both report
    SERVICE_STATUS_HANDLE handle = nullptr;
    SERVICE_STATUS status{};
};

State* g_state = nullptr;

void report(State& state, DWORD current, DWORD waitHintMs = 0)
{
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    state.status.dwCurrentState = current;
    // A stop is accepted only while running: once one is under way, a second
    // would only be the same flag set again.
    state.status.dwControlsAccepted =
        current == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    state.status.dwWaitHint = waitHintMs;
    state.status.dwCheckPoint = current == SERVICE_STOP_PENDING ? state.status.dwCheckPoint + 1 : 0;
    if (current == SERVICE_STOPPED && state.exitCode != 0)
    {
        state.status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        state.status.dwServiceSpecificExitCode = static_cast<DWORD>(state.exitCode);
    }
    ::SetServiceStatus(state.handle, &state.status);
}

DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID context)
{
    State& state = *static_cast<State*>(context);
    switch (code)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(state, SERVICE_STOP_PENDING, kStopWaitHintMs);
        state.requestStop();
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    State& state = *g_state;

    // The name is not checked for an own-process service: there is only one.
    state.handle = ::RegisterServiceCtrlHandlerExW(L"", &control, &state);
    if (state.handle == nullptr)
    {
        state.exitCode = 1;
        return;
    }

    // Running from here: everything that can fail at startup is reported by
    // `run`'s exit code, which SERVICE_STOPPED carries.
    report(state, SERVICE_RUNNING);
    state.exitCode = (*state.run)();
    report(state, SERVICE_STOPPED);
}

} // namespace

int runAsWindowsService(const std::function<int()>& run, void (*requestStop)())
{
    State state;
    state.run = &run;
    state.requestStop = requestStop;
    g_state = &state;

    wchar_t name[] = L"";
    const SERVICE_TABLE_ENTRYW table[] = {{name, &serviceMain}, {nullptr, nullptr}};
    const BOOL dispatched = ::StartServiceCtrlDispatcherW(table);
    const DWORD error = dispatched ? 0 : ::GetLastError();
    g_state = nullptr;

    if (!dispatched)
    {
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            std::fputs("--service: not started by the Service Control Manager; run without "
                       "--service from a console\n",
                       stderr);
        else
            std::fprintf(stderr, "--service: StartServiceCtrlDispatcher failed (%lu)\n",
                         static_cast<unsigned long>(error));
        return 2;
    }
    return state.exitCode;
}

} // namespace multiroom::service
