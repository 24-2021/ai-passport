<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# kfb24 电脑端桥接

`kfb24_bridge.py` 把开发板的八个动作转换成 Windows 输入与程序启动。它同时监听
USB 串口、Wi-Fi TCP，并可选监听 BLE；每两秒发送一次 `KFB24 HELLO` 心跳，并在收到
命令的同一通道上回复 ACK。

## 环境要求

- Windows 10/11。
- Python 3.9 或更新版本，并安装 `pyserial`、`sounddevice`；使用 `--ble` 时还需要
  `bleak`：`python -m pip install -r pc/requirements.txt`
- 已安装并启用微信输入法（WeType TSF 输入法）。
- 板载麦克风语音需要启用 Windows“立体声混音”并设为默认录音设备，同时在微信输入法
  的语音设置中选择它。
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

- 只启动本次会话：`pc\start_bridge.cmd`。
- 取消自启并停止桥接：`pc\uninstall_autostart.cmd`。
- 启动器始终传入 `--ble`，因此 USB 被占用或没插线时仍可通过 BLE 控制。

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

BLE NUS 通道没有应用层认证。范围内任何能连接 `kfb24` 的 BLE central 都能发送与
桥接相同的命令，因此只应在可信环境启用 `--ble`。

## 动作

| 开发板动作 | 电脑端效果 |
| --- | --- |
| `voice` | USB 时开发板推流板载麦克风，桥接播放到默认输出并按住微信输入法语音快捷键（默认 `Ctrl+Win`）；再次按下同时停止。BLE/Wi-Fi 时只控制快捷键。使用微信输入法，不再发送 Windows `Win+H`。 |
| `ime` | 激活微信输入法 TSF 配置（`{86598FB9-…}` / `{607FDF85-…}`，langid `0x0804`）；失败时发送配置的热键。 |
| `wechat` | 按“启动 / 恢复 / 最小化 / 聚焦”切换微信窗口。 |
| `orca` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Orca。 |
| `terminal` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Windows Terminal。 |
| `trae` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Trae。 |
| `chrome` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Chrome。 |
| `edge` | 按“启动 / 恢复 / 最小化 / 聚焦”切换 Edge。 |

`voice` 请先把光标放进目标位置。USB 连接时，开发板用 ES8311 麦克风采集 16 kHz
单声道 PCM，按 base64 + CRC32 行协议通过 USB 发送，桥接用 `sounddevice` 播放到
默认输出设备。Windows 必须启用“立体声混音”并设为默认录音设备，微信输入法必须把
麦克风选为“立体声混音”；否则微信输入法仍会录电脑麦克风。

启用方法：**声音设置 → 更多声音设置 → 录制**，在空白处右键勾选“显示禁用的设备”，
再右键“立体声混音”选择“启用”和“设为默认设备”。随后在微信输入法语音设置中选择
“立体声混音”，或确认“自动检测”解析到它。

缺少 `sounddevice` 或 PortAudio 无法打开默认输出时，桥接会打印原因，但仍会锁存
微信输入法快捷键。

桥接会在 `voice_max_seconds`（默认 60 秒）后自动松开，并在退出时松开，避免开发板
或桥接失联后 `Ctrl`/`Win` 卡住。识别出的文字仍进入当前焦点输入框。

启动器按可执行文件名匹配顶层窗口：没有窗口就启动；已最小化就恢复并聚焦；已在前台
就最小化；其他情况聚焦。若找到窗口但无法聚焦，桥接会报告失败，而不是再启动一个
重复进程。

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
  "ime_hotkey": "ctrl+space"
}
```

`voice_mode` 默认为 `"latch"`：第一次 `voice` 按住快捷键，第二次松开。微信输入法
使用“启动语音输入”模式时改为 `"tap"`。`voice_max_seconds` 是自动松开秒数，设为
`0` 可关闭自动松开（不建议）。`voice_hotkey` 设为 `right-alt` 可改为锁存右 Alt，
设为空字符串可禁用语音动作。请在微信输入法的语音输入设置中选择“按住说话”，并
配置相同的快捷键。

用 `--config PATH` 指定其他文件；`--dry-run` 只打印动作，不发送输入或启动程序；
`--test-action voice|ime|wechat|orca|terminal|trae|chrome|edge` 单独测试一个动作；
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
  `voice_hotkey` 一致，并确认微信输入法正在运行。若使用“启动语音输入”模式，把
  `"voice_mode"` 设为 `"tap"`。
- `voice` 识别为静音或电脑麦克风：板载音频只走 USB；确认当前是 USB 桥接，启用
  “立体声混音”并设为默认录音设备，再在微信输入法中选择它。安装 `sounddevice`，
  并查看桥接日志里的 `audio:` 行。
- `voice` 后 `Ctrl`/`Win` 卡住：再按一次开发板“语音输入”，或等待
  `voice_max_seconds` 自动松开；桥接退出时也会松开。
- 程序被打开两次：先关闭多余的窗口；桥接会聚焦该进程找到的第一个可见顶层窗口。
