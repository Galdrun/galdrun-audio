#pragma once

// The server's error model, settled once and used by every module.
//
// Each module declares its own small error enum next to the code that can
// produce it, and returns Result<T, ThatEnum>. This header only fixes the
// shape a failure travels in, so a caller can log one, report one through the
// status API and match on one the same way everywhere.
//
// Exceptions never cross a module boundary. Where the standard library or a
// third-party header throws, the module that called it catches at its own edge
// and converts (see util/FileUtil for the first instance).
//
// The detail string is not decoration. The rule the roadmap states for every
// phase is that a component which cannot do its job says so explicitly, and an
// enum on its own cannot name the key, the path or the format that was wrong.

#include <expected>
#include <string>
#include <utility>

namespace multiroom {

template <typename Code>
struct Failure
{
    Code code{};
    std::string detail;
};

template <typename T, typename Code>
using Result = std::expected<T, Failure<Code>>;

// A call that either succeeds or fails and has no value of its own.
template <typename Code>
using Status = std::expected<void, Failure<Code>>;

template <typename Code>
[[nodiscard]] std::unexpected<Failure<Code>> fail(Code code, std::string detail = {})
{
    return std::unexpected(Failure<Code>{code, std::move(detail)});
}

// "UnknownKey: audio.maxSampleRte" -- one log line, one status field.
//
// errorName() is found by argument-dependent lookup, so a module supplies its
// own overload beside its enum and nothing has to be registered anywhere.
template <typename Code>
std::string describe(const Failure<Code>& failure)
{
    const std::string name = errorName(failure.code);
    return failure.detail.empty() ? name : name + ": " + failure.detail;
}

} // namespace multiroom
