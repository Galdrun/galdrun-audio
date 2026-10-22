#pragma once

// A browser on the control port: the settings page at "/" and the control
// protocol at "POST /api", one request per connection. Every other path,
// /api/settings among them, is 404: the page tells this server from the
// ESP32's port-80 one by that answer.
//
// The control server tells HTTP from its own JSON lines by the first bytes
// (a JSON request starts with '{'), so every client gets a page without a
// second port, thread or setting. The request body of /api is a control
// request exactly as a line would carry it, and the answer is the response
// line, so the page can do what the protocol can and nothing more.
//
// No authentication, like the line protocol. What the browser adds is that
// any web site the user opens could send requests here; /api therefore wants
// a JSON content type (a cross-site request with one needs a preflight, which
// is never answered) and a Host that is an address or this machine's own name
// (which a DNS rebinding attack cannot fake).

#include "control/ControlProtocol.h"

#include <string>

#include <cstddef>

namespace multiroom {

// The client's settings page (shared/web/client-settings.html), the one copy
// every build serves: here at "/", and on the ESP32 by its port-80 server.
extern const char kClientSettingsPage[];
extern const size_t kClientSettingsPageBytes;

// An address, localhost, or this machine's own name, bare or under .local. A
// rebinding attack brings the attacker's domain name, which is none of them --
// including one that merely starts with this machine's name.
bool isTrustedHost(const std::string& hostHeader);

// True when the buffered bytes start an HTTP request rather than a line.
bool looksLikeHttpRequest(const std::string& inbox);

// Consumes one complete request from `inbox` and puts the whole response,
// headers and body, in `response`. False while the request is incomplete;
// then nothing is consumed. The caller closes the connection after sending,
// except GET /api/input-events: inputEvents is set and SSE continues on it.
bool serveHttpRequest(std::string& inbox, IControlHandler& handler, std::string& response,
                      bool* inputEvents = nullptr);

} // namespace multiroom
