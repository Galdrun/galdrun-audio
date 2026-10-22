#include "config/BoardConfig.h"

#include "util/FileUtil.h"

namespace multiroom::board_config {

json::Value read(const std::string& path)
{
    std::string text, error;
    json::Value board;
    if (!fileutil::readTextFile(path, text, error) || !json::parse(text, board, error) ||
        !board.isObject())
        return json::Value::makeObject();
    return board;
}

const json::Value& current()
{
    // A function-local static: initialized once, safely from any thread.
    static const json::Value board = read(kPath);
    return board;
}

bool enabled(const json::Value& board, const char* key)
{
    bool value = false;
    return json::getBool(board, key, value) && value;
}

} // namespace multiroom::board_config
