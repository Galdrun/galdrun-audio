# Galdrun client for Windows

Turns this PC into a room of a Galdrun multiroom system: it plays what the
Galdrun server sends, in sync with the other rooms. Keep this whole folder
together; the Visual C++ runtime it needs is next to `galdrun-client.exe`, so
nothing has to be installed.

```powershell
.\galdrun-client.exe                                 # the server finds it on the network
.\galdrun-client.exe --server 192.168.1.20:4711      # or connect to a server directly
```

The client announces itself on the network, the server adds it as a new room.
Settings are kept in `%LOCALAPPDATA%\Galdrun\client.json` and can be changed in
the browser at `http://localhost:4720/` (from other devices:
`http://<this-pc>:4720/`). `client.example.json` shows the file's layout.

- **Sound card**: by default the client uses the Windows default output in
  exclusive mode, so playback is bit-perfect and the system mixer is bypassed.
  Other programs cannot play on that device meanwhile. Pick the device and mode
  on the settings page, or with `--audio-device <id>` and
  `--audio-mode exclusive | exclusive-only | shared`.
- **Firewall**: the settings page listens on TCP 4720 and the client sends its
  announcement to UDP 4721. Allow the client when Windows asks.
- **Test**: `.\galdrun-client.exe --test-tone 10` plays a 440 Hz tone at -12 dBFS
  for 10 seconds and reports the sound card's clock drift. Turn the amplifier
  down first: in exclusive mode the Windows volume does not apply.

`.\galdrun-client.exe --help` lists every option. Windows 10/11, 64-bit.
