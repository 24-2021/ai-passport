<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# kfb24 PC bridge

`kfb24_bridge.py` turns the board's eight actions into Windows input and app
launches. It listens on USB serial, Wi-Fi TCP, and optionally BLE, sends
`KFB24 HELLO` heartbeats every two seconds, and acknowledges every board command
on the receiving transport.

## Requirements

- Windows 10/11.
- Python 3.9 or newer with `pyserial`; `bleak` is additionally required for
  `--ble`:
  `python -m pip install -r pc/requirements.txt`
- WeType (the WeChat input method) installed as a TSF input method.
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

- Start only this session: `pc/start_bridge.cmd`.
- Remove autostart and stop the bridge: `pc/uninstall_autostart.cmd`.
- The launcher always passes `--ble`, so a busy USB port or an unplugged cable
  can still be controlled over BLE.

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

The NUS link has no application-layer authentication. Any BLE central in range
that connects to `kfb24` can send the same commands as the bridge, so enable
`--ble` only in a trusted environment.

## Actions

| Board action | PC effect |
| --- | --- |
| `voice` | First press holds the configured WeType voice hotkey (default `Ctrl+Win`); the next press releases it. Use WeType voice input, not Windows `Win+H`. |
| `ime` | Activates the WeType TSF profile (`{86598FB9-…}` / `{607FDF85-…}`, langid `0x0804`), falling back to the configured hotkey. |
| `wechat` | Focuses an existing WeChat window; starts `Weixin.exe` only when no window exists. |
| `orca` | Focuses an existing Orca window; starts `Orca.exe` only when no window exists. |
| `terminal` | Focuses Windows Terminal; starts `wt.exe` only when no window exists. |
| `trae` | Focuses Trae; starts `%LOCALAPPDATA%\Programs\Trae\Trae.exe` only when no window exists. |
| `chrome` | Focuses Chrome; starts `chrome.exe` only when no window exists. |
| `edge` | Focuses Edge; starts `msedge.exe` only when no window exists. |

For `voice`, put the cursor in the target edit control first. The first board
press starts WeType recording by holding the shortcut; the next press stops it.
The bridge releases the keys after `voice_max_seconds` (default 60) and on exit,
so a lost board or bridge cannot leave `Ctrl`/`Win` stuck. The resulting text
goes to the focused window.

The launcher matches visible top-level windows by executable name, restores a
minimized window, and brings it to the foreground. If a matching window exists
but cannot be focused, the bridge reports failure instead of starting a duplicate
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
  "ime_hotkey": "ctrl+space"
}
```

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
  the same shortcut as `voice_hotkey`, and check that WeType is running. If
  WeType uses start/stop mode instead, set `"voice_mode": "tap"`.
- `Ctrl`/`Win` stays down after `voice`: press the board `voice` action again, or
  wait for `voice_max_seconds`; the bridge also releases held keys on exit.
- An app opens twice: close extra windows once; the bridge focuses the first
  visible top-level window it finds for that process.
