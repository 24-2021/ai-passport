<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# kfb24 电脑端桥接

`kfb24_bridge.py` 把开发板的五个动作转换成 Windows 输入与程序启动。它同时监听 USB 串口和 Wi-Fi TCP，每两秒发送一次 `KFB24 HELLO` 心跳，并在收到命令的同一通道上回复 ACK。

## 环境要求

- Windows 10/11。
- Python 3.9 或更新版本，并安装 `pyserial`：
  `python -m pip install -r pc/requirements.txt`
- 已安装并启用微信输入法（WeType TSF 输入法）。
- 程序路径符合下方默认值，或通过 JSON 覆盖。

## 查找开发板并配置 Wi-Fi

开发板使用 USB-Serial-JTAG，VID:PID 为 `303A:1001`。

```powershell
python pc/kfb24_bridge.py --list-ports
python pc/kfb24_bridge.py --configure --ssid "我的Wi-Fi" --password "密码"
```

`--configure` 只通过 USB 与开发板通信，把 SSID/密码做十六进制编码，并发送电脑的局域网地址和 TCP 端口。开发板把配置写入 NVS 后自动重连。电脑有多个网卡时，用 `--host 192.168.x.y` 明确指定地址。电脑端不会把 Wi-Fi 密码写入磁盘；开发板会把它保存在 NVS。

## 启动桥接

```powershell
python pc/kfb24_bridge.py --port COM6
```

不传 `--port` 时会自动查找 Espressif 串口。默认在 `0.0.0.0:8765` 提供 TCP 服务：

- 开发板报告 USB 桥接心跳时，优先走 USB。
- USB 不存在或电脑只能通过局域网访问时，走 Wi-Fi/TCP。
- 开发板每条命令只发送一次；桥接在接收通道上回复 ACK。

Wi-Fi 控制需要在 Windows 防火墙中允许 Python 访问专用网络。监听端会接受局域网命令，请只在可信网络运行。

## 动作

| 开发板动作 | 电脑端效果 |
| --- | --- |
| `voice` | 发送 `Win+H` 打开 Windows 语音输入，使用电脑麦克风。 |
| `ime` | 激活微信输入法 TSF 配置（`{86598FB9-…}` / `{607FDF85-…}`，langid `0x0804`）；失败时发送配置的热键。 |
| `wechat` | 启动 `C:\Program Files\Tencent\Weixin\Weixin.exe`。 |
| `orca` | 启动 `%LOCALAPPDATA%\Programs\orca\Orca.exe`。 |
| `terminal` | 启动 `wt.exe`（Windows Terminal）。 |

`voice` 只负责打开听写，文字仍进入当前焦点输入框；请先聚焦微信、Orca 或终端。

## 覆盖配置

存在 `%APPDATA%\kfb24\bridge.json` 时桥接会读取它，所有键都可省略：

```json
{
  "wechat": "C:\\Program Files\\Tencent\\Weixin\\Weixin.exe",
  "orca": "C:\\Users\\me\\AppData\\Local\\Programs\\orca\\Orca.exe",
  "terminal": "wt.exe",
  "ime_hotkey": "ctrl+space"
}
```

用 `--config PATH` 指定其他文件；`--dry-run` 只打印动作，不发送输入或启动程序；`--test-action voice|ime|wechat|orca|terminal` 单独测试一个动作；`--verbose` 打印开发板日志和所有协议帧。

## 排查

- 找不到串口：确认 Windows 能枚举 `303A:1001`；必要时安装 Espressif USB-JTAG 驱动，并使用支持数据传输的 USB 线。
- TCP 连不上：检查 `--configure` 传入的电脑地址、两台设备是否在同一局域网、防火墙规则。
- 微信输入法激活失败：确认输入法已安装并启用；桥接会改发 `ime_hotkey` 热键。
- `voice` 没反应：先把光标放进输入框；`Win+H` 是 Windows 级快捷键，不是开发板麦克风推流。
