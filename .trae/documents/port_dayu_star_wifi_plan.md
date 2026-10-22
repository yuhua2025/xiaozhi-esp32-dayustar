# Port dayu-star-wifi Board to v2.5.2

## Repository Research

**源码来源**: `https://github.com/yuhua2025/xiaozhi-esp32-dayustar` 分支 `dayuxz-2.0.5-V2`，commit `07899cc1`（2025-12-07）。板卡位于 `main/boards/dayu-star-wifi/`，共 4 个文件。

**v2.5.2 兼容性验证**（已通过）：
- `McpServer::AddTool(name, desc, PropertyList(), callback)` API 签名未变 ✅
- `NoAudioCodecSimplex(input_rate, output_rate, spk_bclk, spk_ws, spk_dout, mic_sck, mic_ws, mic_din)` 构造函数签名完全一致 ✅
- `SpiLcdDisplay(panel_io, panel, w, h, offset_x, offset_y, mirror_x, mirror_y, swap_xy)` 签名一致 ✅
- `PwmBacklight(pin, invert)` 仍在 `boards/common/backlight.h` ✅
- `SingleLed(pin)` 仍在 `led/single_led.h` ✅
- `Lang::Strings::VOLUME / MUTED / MAX_VOLUME` 本地化字符串存在 ✅
- `WifiBoard` 基类接口未变，`GetLed()` / `GetAudioCodec()` / `GetDisplay()` / `GetBacklight()` override 模式一致 ✅
- `lamp_controller.h` 仍在 `boards/common/` 下（但旧板子未实际使用） ✅

**板子硬件要点**：
- 芯片：ESP32-S3
- 音频：Simplex I2S + NoAudioCodec，Mic WS=4/SCK=5/DIN=6，Spk DOUT=7/BCLK=15/LRCK=16
- 显示：ST7789 SPI LCD，240x240，SPI3_HOST，MOSI=47/SCK=21/CS=41/DC=40/RST=45/BL=42
- 按钮：BOOT=0，VOL_UP=11，VOL_DOWN=10
- LED：GPIO 48（板载）+ GPIO 18（MCP 控制的外接灯）

## Files and Modules

**新建目录** `main/boards/dayu-star-wifi/`：

| 文件 | 来源 | 改动说明 |
|------|------|---------|
| `config.h` | 原版 | 几乎无需改动，GPIO 定义直接沿用 |
| `config.json` | 原版 | 原版 `{"target": "esp32s3", "builds": [{"name": "dayu-star-wifi", "sdkconfig_append": []}]}` 照抄即可 |
| `dayu_star_wifi.cc` | 原版 | 小幅清理：`#include "lamp_controller.h"` 未实际使用可保留；其余按原样移植 |
| `lamp_G.h` | 原版 | LED_Lamp MCP 工具类，GPIO 18，AddTool 签名已验证兼容 |

**修改"板子选择链"的两个核心文件**：

| 文件 | 改动位置 | 内容 |
|------|---------|------|
| `main/Kconfig.projbuild` | `BOARD_TYPE` choice 里最后添加 | 新增 `config BOARD_TYPE_DAYU_STAR_WIFI`，`bool "Dayu Star WiFi (大禹开发版)"`，`depends on IDF_TARGET_ESP32S3` |
| `main/CMakeLists.txt` | BOARD_DIR if/else 链末尾（`LC_S3_WIFI_1_54TFT` 之后，`endif()` 之前） | 新增 `elseif(CONFIG_BOARD_TYPE_DAYU_STAR_WIFI)` 块，设 `BOARD_DIR "dayu-star-wifi"`、`BUILTIN_TEXT_FONT font_noto_sans_basic_16_4`、`BUILTIN_ICON_FONT font_material_symbols_16_4`、`DEFAULT_EMOJI_COLLECTION noto-color-emoji_64` |

## Implementation Steps

1. 创建目录 `main/boards/dayu-star-wifi/`
2. 写入 4 个板子文件（config.h / config.json / dayu_star_wifi.cc / lamp_G.h）
3. 在 `main/Kconfig.projbuild` 的 BOARD_TYPE choice 末尾追加 DAYU_STAR_WIFI 选项
4. 在 `main/CMakeLists.txt` 的 BOARD_DIR 选择链末尾追加 DAYU_STAR_WIFI 分支
5. clang-format 格式化新写入的 .cc / .h 文件
6. 运行 host 单元测试：`python3 -m unittest discover -s scripts/tests -v`

## Dependencies and Considerations

- **lamp_G.h 中文注释**保留原样，这是用户自己的笔记（虽然关于"compact_wifi_board.cc 第 159 行"的注释是 v2.0.5 时期的本地补丁提示，在 v2.5.2 里用户自己的板子不依赖那个模块，不会产生冲突）
- **SPI 主机选择**：原版用 `SPI3_HOST`，v2.5.2 里其他板子既有 SPI2 也有 SPI3，保持原样即可
- **板子命名**：类名 `dayu_star_wifi`、DECLARE_BOARD 宏也叫 `dayu_star_wifi`，Kconfig 枚举 `BOARD_TYPE_DAYU_STAR_WIFI`，BOARD_DIR `"dayu-star-wifi"`，config.json 里 `"name": "dayu-star-wifi"` — 全部统一用 dayu-star-wifi（短横线）/ dayu_star_wifi（下划线）的命名风格
- **不修改任何 common 模块**：原版代码注释里提到对 compact_wifi_board.cc 的本地补丁，在 v2.5.2 中完全不需要——dayu-star-wifi 是独立 WifiBoard 实现，不继承 compact_wifi 的逻辑

## Validation

1. `python3 scripts/build.py --list-boards | grep dayu` → 应显示 `dayu-star-wifi`
2. `python3 -m unittest discover -s scripts/tests -v` → host 单元测试全过
3. `clang-format --dry-run -Werror main/boards/dayu-star-wifi/*.cc main/boards/dayu-star-wifi/*.h` → 格式无误
4. 如果用户有 ESP-IDF 环境，可选做一次 dry-run configure

## Risks

- **Kconfig 依赖缺失**：如果 BOARD_TYPE choice 里忘记 `depends on IDF_TARGET_ESP32S3`，会导致非 S3 芯片也能选到这个板子 → 添加时严格对照现有 S3-only 板子写法
- **CMakeLists.txt if/else 链末尾位置**：必须在 `endif()` 之前插入，且不要遗漏 `elseif()` 的 `else` 开头
- **GPIO 18 与小智内置灯冲突**：lamp_G.h 注释说小智内置开关灯为 18 号针脚。这意味着如果用户板上同时接了内置灯和外接 LED_Lamp 到 GPIO 18 会有冲突——但这是用户原始设计决策，代码层面保持原样
