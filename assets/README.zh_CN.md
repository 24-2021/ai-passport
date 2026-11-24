<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐、电池参数等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### kfb24 中文字库

kfb24 应用内置 Noto Sans SC 的 14、16、22 px 子集。生成的字库源码放在 `main/fonts/`，因为 ESP-IDF 要把它作为组件源码编译；许可文本保留在本目录。

| 文件 | 尺寸与格式 | 用途 |
| --- | --- | --- |
| [`fonts/OFL.txt`](fonts/OFL.txt) | SIL Open Font License 1.1 | Noto Sans SC 的许可。 |
| `main/fonts/kfb24_14.c` | 14 px，4 bpp，LVGL 9 C 数组 | 提示、说明、页脚、关于页行。 |
| `main/fonts/kfb24_16.c` | 16 px，4 bpp，LVGL 9 C 数组 | 网格卡片标题与状态文字。 |
| `main/fonts/kfb24_22.c` | 22 px，4 bpp，LVGL 9 C 数组 | 主卡片标题与页面标题。 |

- 来源：[Noto CJK 仓库](https://github.com/notofonts/noto-cjk) 的 Noto Sans SC Regular，转换输入未作修改。
- 字符集：可打印 ASCII（`0x20`–`0x7E`）、`main/` 下出现的全部 CJK/假名/全角字符，以及常用中文标点。日志与注释也一起扫描，这样文字在日志和控件之间移动时 `tools/check_glyphs.py` 仍然通过。
- 修改界面文字后重新生成，需要 Node.js 与 `npx`：
  `python3 tools/gen_kfb24_fonts.py --font /path/NotoSansSC-Regular.otf`
  转换器（`lv_font_conv`）版本固定在 `tools/requirements-fonts.txt`。
- 目标路径：`main/fonts/`；`main/CMakeLists.txt` 已列出三个生成文件。C 源码随仓库提交，普通固件构建不需要 Node.js 或 OTF 文件。
- RAM：字形位图是 `const`，留在 Flash；只有很小的 `lv_font_t` 描述符和 LVGL 字形缓存占用内部 RAM。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/kfb24-home.png`](images/kfb24-home.png) | 480 × 640，PNG RGB | kfb24 主界面的 2 倍生成预览，用于根 README。布局与配色对应 `main/kfb24_ui.c` 的 240 × 320、四行动作卡片和页脚；这是界面预览，不是设备照片。2026-11-24 使用 Pillow 按代码中的布局常量生成。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 电池参数（battery）

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
保存万锂新能源 500 mAh 电芯的 CW2017 参数（profile2、XIL40X、2026-10-20）。
该文件是项目提供的 `喜梨_智能工卡_万锂新能源500mAh_profile2_XIL40X_20261020.txt`
的原样副本，按维护者要求保留用于本次固件更新；原文件没有附带单独的许可说明。

格式为 80 行两位十六进制数字，每行一个字节，按寄存器顺序排列。
BSP 在 `components/bsp/src/bsp_battery.c` 中嵌入这 80 字节，主机检查会验证
源文件及数组内容完全一致。文本文件 SHA-256：
`7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`。
解码后 80 字节 SHA-256：
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`。
这是电量计参数，不是充电配置，也不能通用于所有 500 mAh 电芯；详见
[硬件指南](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md#9-cw2017-电池计)。
