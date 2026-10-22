#pragma once

// The board file a Linux firmware image ships, /etc/<brand>-board.json: which
// optional features this board has (network, CPU, DAC and LED settings) and
// its hardware limits. It is part of the image, so it is read once per process
// and every reader shares the parsed copy; a change takes a client restart.

#include "util/Json.h"

#include <string>

namespace multiroom::board_config {

constexpr const char* kPath = "/etc/" MULTIROOM_BRAND_SLUG "-board.json";

// The file at `path`, parsed; an empty object when it is missing or invalid,
// which is every board that is not a firmware image.
json::Value read(const std::string& path);

// read(kPath), the first time it is asked for.
const json::Value& current();

// True only when `key` is the JSON value true: how the file opts a feature in.
bool enabled(const json::Value& board, const char* key);

} // namespace multiroom::board_config
