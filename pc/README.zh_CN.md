<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# kfb24 电脑端桥接

`kfb24_bridge.py` 把开发板的九个动作转换成 Windows 输入与程序启动。它同时监听
USB 串口、Wi-Fi TCP，并可选监听 BLE；每两秒发送一次 `KFB24 HELLO` 心跳，并在收到
命令的同一通道上回复 ACK。

## 环境要求

- Windows 10/11。
- Python 3.9 或更新版本，并安装 `pyserial`、`sounddevice`、`pycaw`；使用
  `--ble` 时还需要 `bleak`：`python -m pip install -r pc/requirements.txt`
- `pycaw` 让桥接在语音会话开始时临时解除虚拟麦克风播放端点的静音，并在结束后
  恢复原来的静音与音量状态。
- 已安装并启用微信输入法（WeType TSF 输入法）。
- 已安装一次 VB-CABLE（管理员权限，安装后重启），并在微信输入法语音设置中把
  麦克风选为 `CABLE Output (VB-Audio Virtual Cable)`。
- 程序路径符合下方默认值，或通过 JSON 覆盖。

## 查找开发板并配置 Wi-Fi

开发板使用 USB-Serial-JTAG，VID:PID 为 `303A:1001`。

```powershell
python pc/kfb24_bridge.py --list-ports
python pc/kfb24_bridge.py --configure --ssid "我的Wi-Fi" --password "密码"
```

`--configure` 只通过 USB 与开发板通信，把 SSID/密码做十六进制编码，并发送电脑的
局域网地址和 TCP 端口。开发板把配置写入 NVS 后自动重连。电脑有多个网卡时，用
`--host 192.168.x.y` 明确指定地址。电脑端不会把 Wi-Fi 密码写入磁盘；开发板会把它
保存在 NVS。配置前先停止正在运行的桥接，否则 COM 口已被占用：
`pc\uninstall_autostart.cmd` 会同时删除自启项并停止当前桥接。

## 登录自启（推荐）

在资源管理器中双击 `pc\install_autostart.cmd`。脚本会查找 Python 3.9+，缺少
`pyserial` / `bleak` 时自动安装，在 Windows 启动目录创建 `kfb24 Bridge` 快捷方式，
并立即在后台启动桥接。输出写入 `%LOCALAPPDATA%\kfb24\bridge.log`。以后登录 Windows
会自动启动，不需要手动打开终端。

启动器会守护桥接进程：异常退出时把退出码写入日志，并按 5–60 秒退避后自动重启，
因此崩溃不会让板子一直离线到下次登录；正常退出则结束守护。
`pc\uninstall_autostart.cmd` 会同时停止守护进程和桥接进程。

- 只启动本次会话：`pc\start_bridge.cmd`。
- 取消自启并停止桥接：`pc\uninstall_autostart.cmd`。
- 启动器始终传入 `--ble`，因此 USB 被占用或没插线时仍可通过 BLE 控制；传
  `--ble-only` 可完全跳过 USB。

## 启动桥接

```powershell
python pc/kfb24_bridge.py --port COM6
```

不传 `--port` 时会自动查找 Espressif 串口。默认在 `0.0.0.0:8765` 提供 TCP 服务：

- 开发板报告 USB 桥接心跳时，优先走 USB。
- USB 不可用且 BLE 是当前桥接通道时，走 BLE。
- 电脑只能通过局域网访问时，走 Wi-Fi/TCP。
- 开发板每条命令只发送一次；桥接在接收通道上回复 ACK。

Wi-Fi 控制需要在 Windows 防火墙中允许 Python 访问专用网络。监听端会接受局域网
命令，请只在可信网络运行。

串口被串口监视器、IDE 或另一个桥接占用时，桥接不再退出，而是打印
`serial: COMx unavailable ... continuing with BLE/TCP` 并保留其他通道。同一时间只
允许一个桥接实例；重复启动会提示 `another kfb24 bridge is already running`；
TCP 端口冲突只会禁用 TCP。

### BLE 通道

```powershell
python pc/kfb24_bridge.py --ble
```

开发板以 `kfb24` 为名广播 Nordic UART Service。`--ble` 让桥接在 USB/TCP 之外再作为
BLE central 连接开发板；`--ble-name` 可修改广播名。BLE 是免 USB 线的替代通道，不是
手机遥控通道。开发板重启或离开范围后，桥接会自动重连。

只走蓝牙、不打开 USB 串口：

```powershell
python pc/kfb24_bridge.py --ble-only
```

`--ble-only` 隐含 `--ble` 并跳过串口自动查找。开发板只需供电，Windows 蓝牙必须打开。
`pc\start_bridge.cmd --ble-only` 同样可用。连接后开发板显示 `ble 1 bridge`，九个动作
及其 ACK 都走 NUS；语音动作同样通过 BLE 推流板载麦克风。

BLE NUS 通道没有应用层认证。范围内任何能连接 `kfb24` 的 BLE central 都能发送与
桥接相同的命令，因此只应在可信环境启用 `--ble`。

## 动作

| 开发板动作 | 电脑端效果 |
| --- | --- |
| `voice` | 开发板通过当前通道（USB、BLE 或 Wi-Fi）推流板载麦克风。有离线模型时桥接自己转写；否则推流到 VB-CABLE 的 `CABLE Input`，微信输入法从 `CABLE Output` 取音，并按住微信输入法语音快捷键（默认 `Ctrl+Win`）。再次按下同时停止。使用微信输入法，不再发送 Windows `Win+H`。 |
| `wake` | 开发板听到“你好小智”后免按键开始：有离线模型时桥接转写整句，执行应用指令或把文字输入焦点窗口；否则走微信输入法通路，检测到停顿后发送 `KFB24 CTRL voice_stop` 并回车提交。见[免按键语音](#免按键语音唤醒词)。 |
| `enter` | 在当前焦点窗口按一次回车，提交识别结果（聊天发送、终端执行、搜索提交）。录音进行中选中它时，桥接先松开语音快捷键并停止推流，再按回车。 |
| `ime` | 激活微信输入法 TSF 配置（`{86598FB9-…}` / `{607FDF85-…}`，langid `0x0804`）；失败时发送配置的热键。 |
| `wechat` | 按“启动 / 恢复 / 最小化 / 聚焦”切换微信窗口。 |
| `orca` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Orca。 |
| `terminal` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Windows Terminal。 |
| `trae` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Trae。 |
| `chrome` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Chrome。 |
| `edge` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Edge。 |

`voice` 请先把光标放进目标位置。开发板用 ES8311 麦克风采集 16 kHz 单声道音频，
按 40 ms 一块（640 个采样、323 字节）编码为 IMA ADPCM，并带 CRC32 通过当前通道
（USB、BLE 或 Wi-Fi）发送。桥接逐块解码，重采样到虚拟声卡端点的采样率，需要时
复制为双声道，再写入 VB-CABLE 的“CABLE Input”播放端点。微信输入法从对应的
“CABLE Output”录音端点取音并自行完成语音识别，因此板载麦克风取代电脑麦克风，
不需要任何其他语音程序。

开发板只在**自行**结束录音时发送 `KFB24 AUDIO END`：达到 60 秒上限、麦克风读取
失败或链路断开。桥接收到后松开按住的微信输入法快捷键并关闭音频流，避免板端异常
让微信输入法一直“聆听”静音。正常按键停止不会发送 `END`。

录音过程中按开发板上的其他动作键同样会结束本次录音：板端在本地停止采集，桥接收到
该命令后立即松开语音快捷键（日志 `voice: ended by orca`），然后再执行该动作。

微信输入法在**按下快捷键时**就确定了文字要输入到哪个窗口，而不是在文字生成时：录音
中途切换焦点，识别结果仍然留在开始时聚焦的那个窗口。所以请先聚焦目标窗口，再按
`voice` 开始录音——开发板上的程序键（`orca`、`chrome`、`edge`、`trae`、`wechat`）
就是用来把目标切到前台的，但它们无法改变已经开始的这次录音的投递目标。

识别出的文字不会写入剪贴板，桥接也无法指定把它投递给某个程序：文字落在哪里完全由
微信输入法决定。

VB-CABLE 只需安装一次：https://vb-audio.com/Cable/（管理员权限，安装后重启）。
在微信输入法的“语音设置”中把麦克风选为
`CABLE Output (VB-Audio Virtual Cable)`。用下面的命令查看端点：

```powershell
python pc/kfb24_bridge.py --list-audio
```

不再使用 Windows“立体声混音”和电脑麦克风。找不到 `CABLE Input` 时，`voice` 会
打印安装说明并且不按微信输入法快捷键。会话开始时桥接会在需要时解除该播放端点的
静音（音量为 0 时提高到 35%），结束后恢复原状态；自动修复依赖 `pycaw`，缺少时
只打印警告。

桥接会在 `voice_max_seconds`（默认 60 秒）后自动松开，并在退出时松开，避免开发板
或桥接失联后 `Ctrl`/`Win` 卡住。识别出的文字仍进入当前焦点输入框。

手动录音完成后不会自动回车：微信输入法在松开快捷键后才把文字写进输入框，自动按键既
可能抢在文字之前，也可能把没说完的一句提交掉。文字确认无误后选“发送”，由桥接
按一次回车；不打算发送就保持不动。板子上键/下键只移动列表选择，在第一项按上键
停在原地，不会跳到列表末尾。免按键录音是例外：听到说话后会自动回车，见下节。

启动器按可执行文件名匹配顶层窗口：没有窗口就启动；已最小化就恢复并聚焦；已在前台
就最小化；其他情况聚焦。若找到窗口但无法聚焦，桥接会报告失败，而不是再启动一个
重复进程。

## 免按键语音（唤醒词）

烧录带 `model` 分区的固件后，开发板在本地运行 WakeNet9s，听到“你好小智”前不推流。
识别到唤醒词且桥接连通时，板端发送 `KFB24 CMD wake <id>`。桥接默认用本地
SenseVoice 模型自己识别这句话；没有安装模型时才退回微信输入法通路。与手动录音
不同，桥接会计算每个 40 ms 解码块的 RMS：说话累计达到 `wake_min_speech_ms` 后，
出现 `wake_silence_ms` 的停顿就结束这句。桥接发送 `KFB24 CTRL voice_stop <id>`，
让板端停止采集，然后转写录音或松开微信输入法快捷键。`wake_no_speech_ms` 内没有
检测到说话则不执行动作直接结束；`wake_max_seconds` 限制整段会话。会话结束后的
1.2 秒内板端忽略新的唤醒词。

ESP32-C3 没有回声消除：聆听时不要通过开发板播放声音，否则板端自己的输出会被转写。
唤醒检测需要模型分区和约 130 KB 空闲堆；缺少任一项时板端记录 `wake: unavailable`，
按键流程仍然可用。`wake_vad_threshold` 是判定说话的 int16 RMS 门槛：环境嘈杂时
调高，说话较轻、一直不触发结束时调低。

| 键 | 默认值 | 含义 |
| --- | --- | --- |
| `wake_silence_ms` | 900 | 说话后判定结束的停顿 |
| `wake_min_speech_ms` | 250 | 停顿生效前所需的说话时长 |
| `wake_no_speech_ms` | 6000 | 一直没有说话时放弃 |
| `wake_vad_threshold` | 300 | 判定为说话的 RMS |
| `wake_max_seconds` | 25 | 整段会话上限，`0` 关闭 |
| `wake_auto_enter` | true | 说完后自动回车 |
| `wake_enter_delay_ms` | 700 | 微信输入法模式下松开快捷键到回车的间隔 |

## 离线识别与应用指令

安装 `sherpa-onnx` 和 SenseVoice 模型后，免按键语音不再依赖微信输入法和
VB-CABLE，桥接还能按内容执行指令，而不只是把文字打进去。Windows、Python 3.9+
只需安装一次：

```powershell
python -m pip install sherpa-onnx
mkdir "%LOCALAPPDATA%\kfb24\models" -Force
curl -L -o "%LOCALAPPDATA%\kfb24\models\sensevoice.tar.bz2" `
  https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2
tar -xjf "%LOCALAPPDATA%\kfb24\models\sensevoice.tar.bz2" -C "%LOCALAPPDATA%\kfb24\models"
```

桥接自动查找解压出的 `sherpa-onnx-sense-voice-*` 目录，也可用 `asr_model_dir`
指定。启动时后台加载 `model.int8.onnx`（约 240 MB）。停顿后识别耗时几百毫秒，
然后二选一：匹配到指令就执行；否则用 `SendInput` 把文字直接输入当前焦点窗口
（不占用剪贴板，也不需要微信输入法）。指令按整句精确匹配，含关键词的听写不会被
误执行：

- `<打开动词> <应用>` 或只说应用名：打开或切换已配置的应用。应用别名覆盖微信、
  Trae、Chrome（浏览器）、Edge、终端、Orca；打开动词包括“打开、启动、运行、
  切换到、切到、进入、回到”。
- “回车、按回车、确认、发送、提交”：按一次回车。
- `voice_commands` 可把整句映射到任意动作：`enter`、`ime`、`voice`、`wake`、
  `wechat`、`orca`、`terminal`、`trae`、`chrome`、`edge`。
- 其余整句作为听写输入；`voice_dictation` 设为 `false` 可只执行指令。

没有模型，或 `voice_backend` 设为 `wetype` 时，桥接保持旧行为：VB-CABLE 加
微信输入法快捷键，并可自动回车。`voice_backend` 支持 `auto`（默认，有模型时
离线）、`sherpa`、`wetype`。

| 键 | 默认值 | 含义 |
| --- | --- | --- |
| `voice_backend` | `auto` | `auto`、`sherpa` 或 `wetype` |
| `asr_model_dir` | 空 | 显式指定 SenseVoice 模型目录 |
| `asr_threads` | 2 | ONNX 推理线程数 |
| `voice_commands` | `{}` | 整句到动作的映射 |
| `voice_dictation` | true | 非指令整句是否输入到焦点窗口 |

## 与小智对话

离线识别可用时，唤醒词听到、又不匹配应用指令语法的句子会交给本地模型，而不是
直接输入到焦点窗口。开发板显示“小智正在思考…”面板，桥接把识别文本发给 Ollama
（或任何 OpenAI 兼容服务），答案通过开发板扬声器播报，同时显示在底部对话框。
手动 `voice` 动作仍然只做听写，只有唤醒词进入对话。

默认后端是 `http://127.0.0.1:11434` 的 Ollama。`llm_model` 为空时桥接选择第一个
已安装模型并记录名称。对话对延迟敏感，因此关闭思考模式；已加载的 2B 模型约一秒
回答。回复限制在两句话左右，去掉 markdown 和表情符号；`tts_voice` 选择 Windows
SAPI 语音（空为系统默认）。语音按 16 kHz 单声道合成，再用与上行相同的 IMA ADPCM
格式推给开发板：固件用 `KFB24 TEXT <base64>` 显示文字，用 `KFB24 AUDIO P` 播放。

模型可以调用 `open_app`、`type_text`、`press_enter` 和 `dispatch_agent`。
`dispatch_agent` 在 `agent_workdir` 里运行 CLI 编程 agent（默认 `pi`，可配置
`claude`），最多等待 `agent_timeout` 秒，再把结果总结成两句话。命令是 argv 列表，
`{prompt}` 直接替换，不经过 shell。精确的应用指令语法优先执行，所以“打开…”
不会等模型。服务不可达时桥接记录 `assistant: local model unavailable`，退回听写，
30 秒后重试。

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `assistant_enabled` | true | 未匹配的唤醒语句交给模型 |
| `llm_api` | `ollama` | `ollama`（原生接口，关闭思考）或 `openai` |
| `llm_url` | `http://127.0.0.1:11434` | 服务地址 |
| `llm_model` | 空 | 模型名；空则选择第一个已安装模型 |
| `llm_timeout` | 90 | 单次对话超时（秒） |
| `agent_commands` | `pi`、`claude` | 名称到 argv 模板的映射 |
| `agent_workdir` | 空 | CLI agent 工作目录；空则用桥接当前目录 |
| `agent_timeout` | 600 | CLI agent 超时（秒） |
| `tts_voice` | 空 | SAPI 语音名；空则用系统默认 |
| `tts_rate` | 0 | SAPI 语速，-10 到 10 |

默认调度 `pi`：本机的 Claude Code 指向一个拒绝所有模型名的代理，换用可用的 CLI
时只需改 `agent_commands`，不用改代码。

## 覆盖配置

存在 `%APPDATA%\kfb24\bridge.json` 时桥接会读取它，所有键都可省略：

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
  "voice_commands": {"开始写代码": "trae"},
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

`virtual_mic` 可填 `sounddevice.query_devices()` 里的设备序号，或设备名的不区分
大小写子串；默认指向微信输入法所录端点对应的播放端点。

`voice_mode` 默认为 `"latch"`：第一次 `voice` 按住快捷键，第二次松开。微信输入法
使用“启动语音输入”模式时改为 `"tap"`。`voice_max_seconds` 是自动松开秒数，设为
`0` 可关闭自动松开（不建议）。`voice_hotkey` 设为 `right-alt` 可改为锁存右 Alt，
设为空字符串可禁用语音动作。请在微信输入法的语音输入设置中选择“按住说话”，并
配置相同的快捷键。

用 `--config PATH` 指定其他文件；`--dry-run` 只打印动作，不发送输入或启动程序；
`--test-action voice|wake|ime|wechat|orca|terminal|trae|chrome|edge` 单独测试一个动作；
`--verbose` 打印开发板日志和所有协议帧。

## 排查

- 找不到串口：确认 Windows 能枚举 `303A:1001`；必要时安装 Espressif USB-JTAG
  驱动，并使用支持数据传输的 USB 线。
- TCP 连不上：检查 `--configure` 传入的电脑地址、两台设备是否在同一局域网、
  防火墙规则。
- BLE 连不上：安装 `bleak`，确认开发板正在广播 `kfb24`，保持在范围内，并确认没有
  其他 central 占用唯一的 BLE 连接。
- 微信输入法激活失败：确认输入法已安装并启用；桥接会改发 `ime_hotkey` 热键。
- `voice` 没反应：先聚焦输入框，把微信输入法设为“按住说话”，把语音快捷键设为与
  `voice_hotkey` 一致，并确认微信输入法正在运行。若日志提示找不到虚拟麦克风播放
  端点，安装 VB-CABLE 并在微信输入法中选择 `CABLE Output`。若使用“启动语音输入”
  模式，把 `"voice_mode"` 设为 `"tap"`。
- `voice` 识别为静音：正常会话应打印 `board microphone -> ... at ... Hz` 和
  `board microphone streaming`。用 `--list-audio` 确认 `CABLE Input` 存在，并在
  微信输入法语音设置中选择 `CABLE Output`。安装 `sounddevice` 与 `pycaw`。
- BLE/Wi-Fi 语音断续：`audio: dropped N frame(s)` 表示链路丢块或校验失败。拉近
  开发板与电脑的距离、减少其他 BLE 流量，或改用 USB。
- `voice` 后 `Ctrl`/`Win` 卡住：再按一次开发板“语音输入”，或等待
  `voice_max_seconds` 自动松开；桥接退出时、收到开发板 `END` 时，以及任何其他
  动作键结束会话时也会松开。
- `audio: N block(s) arrived before the capture stream was open`：会话开始时出现
  一两块属于正常现象，因为按下语音键后开发板立即开始推流。数字持续增长说明开发板
  在推流但电脑端没有会话；按一次 `voice` 重新对齐。
- 程序被打开两次：先关闭多余的窗口；桥接会聚焦该进程找到的第一个可见顶层窗口。
