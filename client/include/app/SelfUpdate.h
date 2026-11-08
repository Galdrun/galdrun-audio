#pragma once

// The client's update from a package the server uploads (POST /api/update on
// the control port). The client never fetches anything itself.
//
// The work is split: the client receives the package and hands over to the
// updater script shipped beside it (galdrun-update.sh / galdrun-update.ps1),
// which checks the SHA-256, unpacks, tries the new binary with --version,
// swaps the files (the old ones go to .update/previous) and starts the new
// client with the same arguments. On Linux the client exec()s the script and
// the script exec()s the client, so the PID never changes and systemd or
// start-stop-daemon keep tracking it. On Windows the script waits for this
// process to exit and starts the new one.
//
// The new client then runs on trial (.update/trial): it confirms once its
// server session is established. A trial run that ends before confirming --
// a crash, or no session within kTrialTimeoutNs -- is rolled back by the next
// start, through the same script.

#include <cstdint>
#include <string>

namespace multiroom {
namespace self_update {

// How long a new version has to reach an established session.
constexpr uint64_t kTrialTimeoutNs = 180ULL * 1000000000ULL;

// The release archive this build matches ("linux-armv7", "windows-x64"), or
// empty where there is none (ESP32, an unknown architecture).
const char* releaseTarget();

// Why this install cannot update itself, empty when it can.
std::string unsupportedReason();

// Where an uploaded package is stored: <install dir>/.update/package.tar.gz
// (Linux) or package.zip (Windows).
std::string packagePath();

// Writes the uploaded package to packagePath(), replacing an older one.
bool writePackage(const char* data, size_t size, std::string& error);

enum class Trial
{
    None,    // no update is being tried
    Running, // this start is the new version's trial
    Failed   // the previous trial run ended without confirming: roll back
};

// Called once at start-up with this run's --config argument (empty without
// one). Marks a pending trial as running. A trial the updater recorded for
// another --config is None here: a second client in the same folder (the
// Connector 3 Bluetooth headphone client) leaves it alone.
Trial beginTrial(const std::string& configArgument);

// The trial run is healthy; the update stays.
void confirmTrial();

// The trial run stopped normally before it could confirm: the next start
// runs the trial again instead of rolling back.
void deferTrial();

// Replaces this process with the updater (Linux; returns only on failure) or
// starts it to run after this process exits (Windows; returns true). action is
// "install" (with the package's sha256) or "rollback". argv is this process's
// own command line, which the updater starts the client with again.
bool handOver(const std::string& action, const std::string& sha256, int argc, char** argv,
              std::string& error);

} // namespace self_update
} // namespace multiroom
