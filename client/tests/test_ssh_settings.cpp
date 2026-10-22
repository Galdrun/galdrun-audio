#include "TestSupport.h"
#include "net/SshSettings.h"
#include "util/FileUtil.h"
#include <string>
#include <sys/stat.h>

using namespace multiroom;

MULTIROOM_TEST(ssh_settings_opt_in_state_validation_and_helper)
{
    multiroomtest::TempDirectory directory("ssh-settings");
    const auto board = directory.file("board.json");
    const auto shadow = directory.file("shadow");
    const auto config = directory.file("ssh.conf");
    const auto pid = directory.file("sshd.pid");
    const auto input = directory.file("input");
    const auto helper = directory.file("helper");
    std::string error;
    CHECK(!ssh_settings::supported(board));
    CHECK(fileutil::writeTextFileAtomic(board, "{\"sshSettings\":true}", error));
    CHECK(ssh_settings::supported(board));

    // Fresh firmware: no password, off, not running.
    CHECK(fileutil::writeTextFileAtomic(shadow, "root::::::::\nsshd:*:::::::\n", error));
    json::Value settings;
    CHECK(ssh_settings::load(settings, error, shadow, config, pid));
    CHECK(!settings.find("enabled")->asBool());
    CHECK(!settings.find("passwordSet")->asBool());
    CHECK(!settings.find("running")->asBool());
    CHECK_EQ(settings.find("user")->asString(), std::string("root"));
    CHECK(fileutil::writeTextFileAtomic(shadow, "root:$6$salt$hash:::::::\n", error));
    CHECK(fileutil::writeTextFileAtomic(config, "SSH_ENABLED=1\n", error));
    CHECK(ssh_settings::load(settings, error, shadow, config, pid));
    CHECK(settings.find("enabled")->asBool() && settings.find("passwordSet")->asBool());
    CHECK(fileutil::writeTextFileAtomic(shadow, "root:!:::::::\n", error));
    CHECK(ssh_settings::load(settings, error, shadow, config, pid));
    CHECK(!settings.find("passwordSet")->asBool());

    const auto parse = [](const char* text) { json::Value value; std::string e; json::parse(text, value, e); return value; };
    for (const char* invalid : {"{}", "{\"enabled\":1}", "{\"enabled\":true,\"user\":\"x\"}",
                                "{\"enabled\":true,\"password\":\"short\"}",
                                "{\"enabled\":true,\"password\":\"line\\nbreak!!\"}",
                                "{\"enabled\":true,\"currentPassword\":5}"})
        CHECK(!ssh_settings::validate(parse(invalid), error));
    CHECK(ssh_settings::validate(parse("{\"enabled\":false}"), error));
    CHECK(ssh_settings::validate(parse("{\"enabled\":true,\"password\":\"long enough\",\"currentPassword\":\"\"}"), error));

    // The helper gets three lines on stdin; its stderr is the error.
    CHECK(fileutil::writeTextFileAtomic(helper, "#!/bin/sh\ncat > '" + input + "'\n", error));
    CHECK(::chmod(helper.c_str(), 0755) == 0);
    CHECK(ssh_settings::save(parse("{\"enabled\":true,\"password\":\"long enough\"}"), settings, error,
                             helper, shadow, config, pid));
    std::string sent;
    CHECK(fileutil::readTextFile(input, sent, error));
    CHECK_EQ(sent, std::string("1\n\nlong enough\n"));
    CHECK(settings.find("enabled") != nullptr);

    CHECK(fileutil::writeTextFileAtomic(helper, "#!/bin/sh\necho 'the current password is wrong' >&2\nexit 1\n", error));
    CHECK(::chmod(helper.c_str(), 0755) == 0);
    CHECK(!ssh_settings::save(parse("{\"enabled\":false,\"currentPassword\":\"guess\"}"), settings, error,
                              helper, shadow, config, pid));
    CHECK_EQ(error, std::string("the current password is wrong"));
    // A wrong password locks the next attempt out for a few seconds.
    CHECK(!ssh_settings::save(parse("{\"enabled\":false,\"currentPassword\":\"guess2\"}"), settings, error,
                              helper, shadow, config, pid));
    CHECK(error.find("wait") != std::string::npos);
}
