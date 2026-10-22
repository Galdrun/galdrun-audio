#pragma once

// The board's settings page on port 80: Wi-Fi, server address, output device
// and I2S clock mode. It runs in setup mode (on the board's own access point,
// 192.168.4.1) and in normal operation (on its LAN address) alike.
//
// Saving writes wifi.json and client.json and restarts the board: the clock
// mode is read when the audio backend is created, and a restart is also the
// one way to be sure the running client does not write its own copy of
// client.json back over the change.
//
// It is the board's one web page: the control port's own page (4720) sends a
// browser here, and the status this page shows comes from that control API.
//
// No authentication, like the control API on port 4720: anything on the LAN
// can change these settings. What it does refuse, like the control port, is a
// request by a foreign Host name (DNS rebinding) and a settings POST without a
// JSON content type (a cross-site form).
namespace web_ui {

extern const char* const kClientConfigPath;

bool start();

// Shown on the page, so it can say why it is there.
void setSetupMode(bool active);

} // namespace web_ui
