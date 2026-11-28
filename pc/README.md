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
| `voice` | Streams the board microphone over the active link (USB, BLE, or Wi-Fi). With an offline model the bridge transcribes it itself; otherwise it feeds VB-CABLE's `CABLE Input` so WeType records `CABLE Output` and holds the configured hotkey (default `Ctrl+Win`). The next press stops both. Use WeType voice input, not Windows `Win+H`. |
| `wake` | Hands-free session started by the board's wake word ("hello Xiaozhi"): with an offline model the bridge transcribes the utterance, executes an app command, or types it; otherwise it uses the WeType path, detects the pause, sends `KFB24 CTRL voice_stop`, and submits with Enter. See [Hands-free voice](#hands-free-voice-wake-word). |
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
Manual sessions never press Enter automatically: WeType commits a moment after
the hotkey is released, so an automatic keystroke would race the text, and the
same Enter would submit a half-finished line. Send the `enter` action when the
text looks right; the board's up/down keys only move the row selection and stop
at the first and last row instead of wrapping. Hands-free sessions are the
exception and submit themselves once speech has been heard; see below.

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

## Hands-free voice (wake word)

With the `model` partition flashed, the board runs WakeNet9s locally and streams
nothing until it hears the wake phrase. On detection it sends `KFB24 CMD wake <id>`
only while the bridge link is ready; the bridge records the utterance itself
when an offline model is installed, otherwise it falls back to the WeType
pipeline. Unlike a manual session, the bridge measures the RMS of every decoded
40 ms block: once speech has lasted `wake_min_speech_ms`, a `wake_silence_ms`
pause ends the utterance. The bridge sends `KFB24 CTRL voice_stop <id>` so the
board stops capturing and frees the microphone, then either transcribes the
recording or releases the WeType hotkey. A session with no speech within
`wake_no_speech_ms` closes without acting; `wake_max_seconds` caps the whole
session. The board ignores a new wake word for 1.2 s after a session ends.

The ESP32-C3 has no acoustic echo cancellation: never play audio through the
board while it listens, or its own output becomes the transcript. Wake
detection needs the model partition and roughly 130 KB of free heap; when either
is missing the board logs `wake: unavailable` and the button flow still works.
`wake_vad_threshold` is the int16 RMS that counts as speech — raise it in a
noisy room, lower it if a quiet speaker never triggers the end of speech.

| Key | Default | Meaning |
| --- | --- | --- |
| `wake_silence_ms` | 900 | pause after speech that ends the utterance |
| `wake_min_speech_ms` | 250 | speech required before a pause counts |
| `wake_no_speech_ms` | 6000 | give up when nothing was said |
| `wake_vad_threshold` | 300 | RMS that counts as speech |
| `wake_max_seconds` | 25 | hard session cap; `0` disables it |
| `wake_auto_enter` | true | press Enter after a completed utterance |
| `wake_enter_delay_ms` | 700 | WeType-only delay between hotkey release and Enter |

## Offline speech recognition and app commands

`sherpa-onnx` with a SenseVoice model makes the bridge independent of WeType and
VB-CABLE for hands-free use, and lets it act on what was said instead of only
typing it. Install once (Windows, Python 3.9+):

```powershell
python -m pip install sherpa-onnx
mkdir "%LOCALAPPDATA%\kfb24\models" -Force
curl -L -o "%LOCALAPPDATA%\kfb24\models\sensevoice.tar.bz2" `
  https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2
tar -xjf "%LOCALAPPDATA%\kfb24\models\sensevoice.tar.bz2" -C "%LOCALAPPDATA%\kfb24\models"
```

The bridge finds the extracted `sherpa-onnx-sense-voice-*` directory
automatically; `asr_model_dir` overrides it. It loads `model.int8.onnx` in the
background at startup (about 240 MB). After the pause, recognition takes a few
hundred milliseconds and the bridge either routes a command or types the text
into the focused window with `SendInput` (no clipboard, no WeType). The command
grammar is deliberately exact, so dictation that merely contains a keyword is
never executed:

- `<open verb> <app>` or just `<app>`: open or focus the configured app. App
  aliases cover WeChat, Trae, Chrome (browser), Edge, terminal, and Orca;
  open verbs include the Chinese equivalents of open, start, switch to, and go
  back to.
- Submit phrases (Enter, send, confirm) press Enter once.
- `voice_commands` maps extra whole utterances to any action name:
  `enter`, `ime`, `voice`, `wake`, `wechat`, `orca`, `terminal`, `trae`,
  `chrome`, `edge`.
- Anything else is dictated. Set `voice_dictation` to `false` to keep the
  bridge command-only.

When the model is absent — or `voice_backend` is `wetype` — the bridge keeps the
old behavior: VB-CABLE plus the WeType hotkey, with the optional automatic
Enter. `voice_backend` accepts `auto` (default: offline when the model exists),
`sherpa`, or `wetype`.

| Key | Default | Meaning |
| --- | --- | --- |
| `voice_backend` | `auto` | `auto`, `sherpa`, or `wetype` |
| `asr_model_dir` | empty | explicit SenseVoice model directory |
| `asr_threads` | 2 | ONNX inference threads |
| `voice_commands` | `{}` | whole-utterance phrase to action map |
| `voice_dictation` | true | type utterances that are not commands |

## Talking to the assistant

When an offline recogniser is active, a wake-word utterance that does not match
the app command grammar goes to a local model instead of the focused window.
The board shows a thinking panel, the bridge posts the transcript to Ollama (or
any OpenAI-compatible server), and the answer is spoken through the board
speaker while the same text appears in the dialog panel. The manual `voice`
action still dictates; only the wake word holds a conversation.

The default backend is Ollama at `http://127.0.0.1:11434`. With an empty
`llm_model` the bridge picks the first installed model and logs its name.
Thinking is disabled because a board conversation is latency-sensitive; a
warm 2B model answers in about a second. Replies are capped at roughly two
sentences, markdown and emoji are stripped, and `tts_voice` selects a Windows
SAPI voice (empty: system default). Speech is rendered at 16 kHz mono and
streamed to the board as IMA ADPCM blocks, the same format as the uplink; the
firmware shows `KFB24 TEXT <base64>` and plays `KFB24 AUDIO P` blocks.

The model may call `open_app`, `type_text`, `press_enter`, and
`dispatch_agent`. `dispatch_agent` runs a CLI coding agent (`pi` by default,
`claude` when configured) in `agent_workdir`, waits up to `agent_timeout`, and
summarises the result. Commands are argv lists, so `{prompt}` is substituted
without a shell. The exact app grammar runs first, so a plain "open ..." never
waits for the model. If the server is unreachable the bridge logs
`assistant: local model unavailable`, falls back to dictation, and retries
after 30 s.

| Key | Default | Meaning |
| --- | --- | --- |
| `assistant_enabled` | true | route unmatched wake utterances to the model |
| `llm_api` | `ollama` | `ollama` (native API, thinking off) or `openai` |
| `llm_url` | `http://127.0.0.1:11434` | server base URL |
| `llm_model` | empty | model name; empty picks the first installed |
| `llm_timeout` | 90 | chat request timeout in seconds |
| `agent_commands` | `pi`, `claude` | name to argv template map |
| `agent_workdir` | empty | CLI agent working directory; empty uses the bridge CWD |
| `agent_timeout` | 600 | CLI agent timeout in seconds |
| `tts_voice` | empty | SAPI voice name; empty uses the system default |
| `tts_rate` | 0 | SAPI rate, -10 to 10 |

`pi` is the default because Claude Code on this machine points at a proxy that
rejects every model name; install a working CLI and change `agent_commands` to
switch, no code change needed.

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
  "wake_silence_ms": 900,
  "wake_min_speech_ms": 250,
  "wake_no_speech_ms": 6000,
  "wake_vad_threshold": 300,
  "wake_auto_enter": true,
  "wake_enter_delay_ms": 700,
  "wake_max_seconds": 25,
  "voice_backend": "auto",
  "asr_model_dir": "",
  "asr_threads": 2,
  "voice_commands": {"start coding": "trae"},
  "voice_dictation": true,
  "assistant_enabled": true,
  "llm_api": "ollama",
  "llm_url": "http://127.0.0.1:11434",
  "llm_model": "",
  "llm_timeout": 90,
  "agent_commands": {"pi": ["pi", "-p", "--no-session", "{prompt}"]},
  "agent_workdir": "",
  "agent_timeout": 600,
  "tts_voice": "",
  "tts_rate": 0,
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
`--test-action voice|wake|ime|wechat|orca|terminal|trae|chrome|edge` to test one
action locally. `--verbose` prints board console logs and every protocol frame.

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
