<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# kfb24 PC bridge

`kfb24_bridge.py` turns the board's nine actions into Windows input and app
launches. It listens on USB serial, Wi-Fi TCP, and optionally BLE, sends
`KFB24 HELLO` heartbeats every two seconds, and acknowledges every board command
on the receiving transport.

## Requirements

- Windows 10/11.
- Python 3.9 or newer with `pyserial`, `sounddevice`, and `pycaw`; `bleak` is
  additionally required for `--ble`:
  `python -m pip install -r pc/requirements.txt`
- `pycaw` lets the bridge temporarily unmute the virtual microphone's playback
  endpoint during a voice session and restore its state afterwards.
- WeType (the WeChat input method) installed as a TSF input method.
- VB-CABLE installed once (administrator, then reboot), and WeType's
  microphone set to `CABLE Output (VB-Audio Virtual Cable)`.
- Paths in the defaults below, or a JSON override.

## Find the board and configure Wi-Fi

The board is USB-serial-JTAG with VID:PID `303A:1001`.

```powershell
python pc/kfb24_bridge.py --list-ports
python pc/kfb24_bridge.py --configure --ssid "MyWifi" --password "secret"
```

`--configure` talks to the board over USB only, hex-encodes the credentials, and
sends the PC's LAN address and the TCP port. The board stores the settings in NVS
and reconnects by itself. If the PC has several network interfaces, pass
`--host 192.168.x.y` explicitly. The PC-side command does not write the Wi-Fi
password to disk; the board stores it in NVS. Stop a running bridge first, or the
COM port is already held: `pc\uninstall_autostart.cmd` stops both the startup
entry and the current bridge.

## Start automatically (recommended)

Double-click `pc/install_autostart.cmd` from Explorer. It finds Python 3.9+,
installs `pyserial` and `bleak` when missing, creates a `kfb24 Bridge` shortcut
in the Windows Startup folder, and starts the bridge hidden immediately. Output
goes to `%LOCALAPPDATA%\kfb24\bridge.log`. The bridge starts at every Windows
login, so the board no longer needs a manually opened terminal.

The launcher supervises the bridge: an abnormal exit is logged with its code and
the bridge is restarted after a 5-60 s backoff, so a crash does not leave the
board offline until the next login. A clean exit ends supervision.
`pc\uninstall_autostart.cmd` stops both the supervisor and the bridge.

- Start only this session: `pc/start_bridge.cmd`.
- Remove autostart and stop the bridge: `pc/uninstall_autostart.cmd`.
- The launcher always passes `--ble`, so a busy USB port or an unplugged cable
  can still be controlled over BLE. Pass `--ble-only` to skip USB entirely.

## Run the bridge

```powershell
python pc/kfb24_bridge.py --port COM6
```

Without `--port`, the bridge auto-detects the Espressif port. It serves TCP on
`0.0.0.0:8765` by default:

- USB is preferred when the board reports a USB bridge heartbeat.
- BLE is used when USB is unavailable and BLE is the active bridge.
- Wi-Fi/TCP is used when the bridge is only reachable on the LAN.
- The board sends each command once; the bridge ACKs on the receiving transport.

Allow Python through Windows Firewall on private networks for Wi-Fi control. The
listener accepts commands from the LAN, so run it only on a trusted network.

A serial monitor, an IDE, or another bridge holding the COM port no longer stops
the bridge. It logs `serial: COMx unavailable ... continuing with BLE/TCP` and
keeps the other transports alive. Only one bridge instance may run; a second one
exits with `another kfb24 bridge is already running`, and a TCP port conflict
disables only TCP.

### BLE transport

```powershell
python pc/kfb24_bridge.py --ble
```

The board advertises as `kfb24` with the Nordic UART Service. `--ble` makes the
bridge a BLE central in addition to USB/TCP; `--ble-name` changes the advertised
name. BLE is a cable-free alternative to USB, not a phone remote-control path.
The bridge reconnects automatically after the board restarts or moves out of
range.

For Bluetooth-only control, with no USB serial port opened:

```powershell
python pc/kfb24_bridge.py --ble-only
```

`--ble-only` implies `--ble` and skips serial autodetection. The board only needs
power, and Windows Bluetooth must be on. `pc/start_bridge.cmd --ble-only` works
too. A connected board reports `ble 1 bridge`; all nine actions and their ACKs
use the NUS link. All nine actions, including voice, work over BLE: the board
streams its own microphone as IMA ADPCM blocks, exactly as it does over USB.

The NUS link has no application-layer authentication. Any BLE central in range
that connects to `kfb24` can send the same commands as the bridge, so enable
`--ble` only in a trusted environment.

## Actions

| Board action | PC effect |
| --- | --- |
| `voice` | Streams the board microphone over the active link (USB, BLE, or Wi-Fi) into VB-CABLE's `CABLE Input`, so WeType records `CABLE Output`; holds the configured WeType hotkey (default `Ctrl+Win`). The next press stops both. Use WeType voice input, not Windows `Win+H`. |
| `enter` | Presses Enter once in the focused window, so the recognised text is submitted (chat send, terminal run, search submit). During a voice session the bridge releases the hotkey and stops the stream first, then presses Enter. |
| `ime` | Activates the WeType TSF profile (`{86598FB9-…}` / `{607FDF85-…}`, langid `0x0804`), falling back to the configured hotkey. |
| `wechat` | Opens, restores, minimizes, or focuses a WeChat window, in that state order. |
| `orca` | Opens, restores, minimizes, or focuses Orca. |
| `terminal` | Opens, restores, minimizes, or focuses Windows Terminal. |
| `trae` | Opens, restores, minimizes, or focuses Trae. |
| `chrome` | Opens, restores, minimizes, or focuses Chrome. |
| `edge` | Opens, restores, minimizes, or focuses Edge. |

For `voice`, put the cursor in the target edit control first. The board captures
16 kHz mono audio with its ES8311 microphone, encodes it as IMA ADPCM in 40 ms
blocks (640 samples, 323 bytes), and streams it with a CRC32 over the active
link: USB, BLE, or Wi-Fi. The bridge decodes each block, resamples it to the
virtual cable's endpoint rate, duplicates it to stereo when needed, and writes
it to VB-CABLE's "CABLE Input" playback endpoint. WeType records the matching
"CABLE Output" endpoint and runs its own speech recognition, so the board
microphone replaces the PC microphone without any other speech app.

The board sends `KFB24 AUDIO END` only when it stops capturing on its own: the
60 s cap, a microphone read error, or a lost transport. The bridge then releases
the latched WeType hotkey and closes the stream, so a board-side failure cannot
leave WeType listening to silence. A normal press from the board stops the
session directly and does not need `END`.

Pressing any other board action during a session ends it as well: the board
stops its capture locally, the bridge releases the voice hotkey when that
command arrives (`voice: ended by orca`), and only then runs the action.

WeType binds its insertion target when the hotkey is pressed, not when the text
arrives: moving focus mid-session leaves the recognised text in the window that
had focus at the start. Focus the target first, then press `voice` — the board
app actions (`orca`, `chrome`, `edge`, `trae`, `wechat`) do exactly that, but
they cannot redirect a session that is already running.

Recognised text is never copied to the clipboard, and the bridge cannot address
it to a specific application: WeType alone decides where the characters go.
Nothing presses Enter automatically: WeType commits a moment after the hotkey
is released, so an automatic keystroke would race the text, and the same Enter
would submit a half-finished line. Send the `enter` action when the text looks
right; the board's up/down keys only move the row selection and stop at the
first and last row instead of wrapping.

Install VB-CABLE once from https://vb-audio.com/Cable/ (administrator, then
reboot). In WeType's voice settings select
`CABLE Output (VB-Audio Virtual Cable)` as the microphone. List the visible
endpoints with:

```powershell
python pc/kfb24_bridge.py --list-audio
```

Windows "Stereo Mix" and the PC microphone are not used. If `CABLE Input` is
missing, `voice` logs the install instructions and does not press the WeType
hotkey. At the start of a session the bridge unmutes that playback endpoint if
needed (raising a zero volume to 35%) and restores its state when the session
ends. This automatic repair needs `pycaw`; without it the bridge logs a warning.

The bridge releases the keys after `voice_max_seconds` (default 60) and on exit,
so a lost board or bridge cannot leave `Ctrl`/`Win` stuck. The resulting text
goes to the focused window.

The launcher matches top-level windows by executable name: no window starts the
app, a minimized window is restored and focused, the foreground window is
minimized, and any other window is focused. If a matching window exists but
cannot be focused, the bridge reports failure instead of starting a duplicate
process.

## Overrides

The bridge reads `%APPDATA%\kfb24\bridge.json` when present. Every key is optional:

```json
{
  "wechat": "C:\\Program Files\\Tencent\\Weixin\\Weixin.exe",
  "orca": "C:\\Users\\me\\AppData\\Local\\Programs\\orca\\Orca.exe",
  "trae": "C:\\Users\\me\\AppData\\Local\\Programs\\Trae\\Trae.exe",
  "chrome": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "edge": "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "terminal": "wt.exe",
  "voice_hotkey": "ctrl+win",
  "voice_mode": "latch",
  "voice_max_seconds": 60,
  "virtual_mic": "CABLE Input",
  "ime_hotkey": "ctrl+space"
}
```

`virtual_mic` accepts a `sounddevice.query_devices()` index or a
case-insensitive name substring. The default targets the playback endpoint whose
capture side WeType records from.

`voice_mode` is `"latch"` by default: the first `voice` holds the shortcut, the
second releases it. Set it to `"tap"` when WeType is configured for its
"start voice input" (press once to start, press again to stop) mode.
`voice_max_seconds` is the auto-release timeout; `0` disables it (not
recommended). Set `voice_hotkey` to `right-alt` to latch Right Alt instead, or
to an empty string to disable the voice action. Configure the same shortcut and
WeType's hold-to-talk mode in its voice-input settings.

Use `--config PATH` for a different file, `--dry-run` to log actions without
sending input or launching apps, and
`--test-action voice|ime|wechat|orca|terminal|trae|chrome|edge` to test one action
locally. `--verbose` prints board console logs and every protocol frame.

## Troubleshooting

- No serial port: install the Espressif USB-JTAG driver if Windows does not
  enumerate `303A:1001`; use a data-capable cable.
- TCP never connects: check the PC address passed to `--configure`, that both
  devices are on the same LAN, and the firewall rule.
- BLE never connects: install `bleak`, confirm the board advertises `kfb24`, keep
  it within range, and make sure no other central holds the single BLE connection.
- WeType activation fails: confirm the IME is installed and enabled; the bridge
  then sends the `ime_hotkey` fallback.
- `voice` does nothing: focus a text field, set WeType to hold-to-talk, configure
  the same shortcut as `voice_hotkey`, and check that WeType is running. If the
  log says the virtual microphone output was not found, install VB-CABLE and
  select `CABLE Output` in WeType. If WeType uses start/stop mode instead, set
  `"voice_mode": "tap"`.
- `voice` transcribes silence: check the `audio:` lines. A working session logs
  `board microphone -> <endpoint> at <rate> Hz` and `board microphone
  streaming`. Run `--list-audio` to confirm `CABLE Input` exists, and select
  `CABLE Output` in WeType's voice settings. Install `sounddevice` and `pycaw`.
- `voice` is choppy over BLE or Wi-Fi: `audio: dropped N frame(s)` means the
  transport lost or corrupted ADPCM blocks. Keep the board closer to the PC,
  stop other BLE traffic, or use USB.
- `Ctrl`/`Win` stays down after `voice`: press the board `voice` action again, or
  wait for `voice_max_seconds`; the bridge also releases held keys on exit, on
  `END`, and whenever another board action ends the session.
- `audio: N block(s) arrived before the capture stream was open`: one block at
  the start of a session is normal, because the board begins streaming when its
  voice key is pressed. A count that keeps growing means the board is streaming
  with no session; press `voice` once to resynchronise.
- An app opens twice: close extra windows once; the bridge focuses the first
  visible top-level window it finds for that process.
