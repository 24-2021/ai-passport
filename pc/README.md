<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# kfb24 PC bridge

`kfb24_bridge.py` turns the board's five actions into Windows input and app launches. It listens on USB serial and/or Wi-Fi TCP, sends `KFB24 HELLO` heartbeats every two seconds, and acknowledges every board command on the same transport.

## Requirements

- Windows 10/11.
- Python 3.9 or newer with `pyserial`:
  `python -m pip install -r pc/requirements.txt`
- WeType (the WeChat input method) installed as a TSF input method.
- Paths in the defaults below, or a JSON override.

## Find the board and configure Wi-Fi

The board is USB-serial-JTAG with VID:PID `303A:1001`.

```powershell
python pc/kfb24_bridge.py --list-ports
python pc/kfb24_bridge.py --configure --ssid "MyWifi" --password "secret"
```

`--configure` talks to the board over USB only, hex-encodes the credentials, and sends the PC's LAN address and the TCP port. The board stores the settings in NVS and reconnects by itself. If the PC has several network interfaces, pass `--host 192.168.x.y` explicitly. The PC-side command does not write the Wi-Fi password to disk; the board stores it in NVS.

## Run the bridge

```powershell
python pc/kfb24_bridge.py --port COM6
```

Without `--port`, the bridge auto-detects the Espressif port. It serves TCP on `0.0.0.0:8765` by default:

- USB is preferred when the board reports a USB bridge heartbeat.
- Wi-Fi/TCP is used when USB is absent or the PC bridge is only reachable on the LAN.
- The board sends each command once; the bridge ACKs on the receiving transport.

Allow Python through Windows Firewall on private networks for Wi-Fi control. The listener accepts commands from the LAN, so run it only on a trusted network.

## Actions

| Board action | PC effect |
| --- | --- |
| `voice` | Sends `Win+H` to open Windows voice typing; the PC microphone is used. |
| `ime` | Activates the WeType TSF profile (`{86598FB9-…}` / `{607FDF85-…}`, langid `0x0804`), falling back to the configured hotkey. |
| `wechat` | Launches `C:\Program Files\Tencent\Weixin\Weixin.exe`. |
| `orca` | Launches `%LOCALAPPDATA%\Programs\orca\Orca.exe`. |
| `terminal` | Launches `wt.exe` (Windows Terminal). |

`voice` opens dictation, but text still goes to the focused edit control; focus WeChat, Orca, or the terminal first.

## Overrides

The bridge reads `%APPDATA%\kfb24\bridge.json` when present. Every key is optional:

```json
{
  "wechat": "C:\\Program Files\\Tencent\\Weixin\\Weixin.exe",
  "orca": "C:\\Users\\me\\AppData\\Local\\Programs\\orca\\Orca.exe",
  "terminal": "wt.exe",
  "ime_hotkey": "ctrl+space"
}
```

Use `--config PATH` for a different file, `--dry-run` to log actions without sending input or launching apps, and `--test-action voice|ime|wechat|orca|terminal` to test one action locally. `--verbose` prints board console logs and every protocol frame.

## Troubleshooting

- No serial port: install the Espressif USB-JTAG driver if Windows does not enumerate `303A:1001`; use a data-capable cable.
- TCP never connects: check the PC address passed to `--configure`, that both devices are on the same LAN, and the firewall rule.
- WeType activation fails: confirm the IME is installed and enabled; the bridge then sends the `ime_hotkey` fallback.
- `voice` does nothing: click into a text field first; `Win+H` is a Windows-level shortcut, not a board microphone stream.
