# dayu-star-wifi 开发板

基于 ESP32-S3 的小智语音助手开发板固件（ESP-IDF v6.1，固件版本 2.5.2）。

## 硬件概览

| 项目 | 说明 |
| --- | --- |
| 主控 | ESP32-S3（16MB Flash，8MB PSRAM） |
| 屏幕 | ST7789 SPI LCD，240×240，SPI3_HOST |
| 麦克风 | I2S 输入，16 kHz |
| 喇叭 | I2S 输出，24 kHz |
| TF 卡 | SPI2_HOST，与 LCD 总线分离，无冲突 |
| 按键 | BOOT、音量 +、音量 - |
| 电池 | ADC 电量检测 + 充电检测 + 电源保持 |

### 引脚定义

| 功能 | GPIO |
| --- | --- |
| LCD MOSI / CLK / DC / RST / CS / 背光 | 10 / 9 / 8 / 18 / 14 / 13 |
| 麦克风 WS / SCK / DIN | 4 / 5 / 6 |
| 喇叭 DOUT / BCLK / LRCK | 7 / 15 / 16 |
| TF 卡 CS / MOSI / MISO / CLK | 46 / 2 / 1 / 3 |
| BOOT / 音量+ / 音量- | 0 / 40 / 39 |
| 电量检测（ADC2_CH6） | 17 |
| 充电检测 | 38 |
| 电源保持 | 21 |

> 充电检测不要使用 GPIO37：该引脚与 ESP32-S3 PSRAM 的 SPICS1 复用，会导致系统复位。

## 功能特性

### 1. 小智语音助手

- Wi-Fi 联网，WebSocket / MQTT+UDP 双协议接入小智服务端；
- 按键唤醒与语音唤醒，屏幕显示对话字幕；
- 多行字幕模式（`CONFIG_USE_MULTILINE_CHAT_MESSAGE`），底部信息可显示两行。

### 2. TF 卡 MP3 播放

插入 TF 卡（挂载到 `/sdcard`），音频全部由 **小智通过 MCP 工具接管**，无需按键操作：

| 工具 | 说明 |
| --- | --- |
| `self.sd_player.list` | 列出所有 MP3 的序号与歌名 |
| `self.sd_player.play` | 按序号或歌名片段播放，留空则播下一首 |
| `self.sd_player.next` | 下一首 |
| `self.sd_player.stop` | 停止播放 |
| `self.sd_player.status` | 当前歌曲、进度、总时长 |

示例对话：

```
"有哪些歌？"          → 返回 1/8 ... 8/8 的列表
"播放第 3 首"          → 开始播放
"播放等爱的玫瑰"        → 按歌名匹配播放
"下一首" / "停一下"     → 切歌 / 停止
```

屏幕底部显示两行信息：

```
1:23/4:45
3/8  等爱的玫瑰
```

实现要点：

- 逐帧解析 MPEG Layer-3 帧头，只把完整帧交给解码器，避免切块打断帧导致的丢帧卡顿；
- 44.1 kHz → 24 kHz 重采样（`esp_ae_rate_cvt`），立体声混合为单声道后写入 I2S；
- 支持 ID3v2（`TIT2`，UTF-16 / UTF-8 / GBK）与 ID3v1 歌名，GBK 通过内置映射表转 UTF-8；
- VBR 文件读取 Xing/Info 帧数得到精确时长，CBR 按码率估算；
- FAT 8.3 文件名无法被 VFS 打开的文件会在扫描阶段跳过，避免"列表里有但播不出"。

### 3. 对话与音乐互斥

本板没有硬件回声消除（AEC），音乐出声会被麦克风拾取并送进语音识别，形成"唱一句、答一句"的循环，因此采用互斥策略：

- 设备只要不处于 `idle`（唤醒 / 聆听 / 思考 / 说话），音乐立即暂停；
- 回到 `idle` 后从原位置自动续播；
- 对话若停在"聆听中"且连续 2 秒无人声，直接置为 `idle`（不向服务器发送停止指令，避免服务端补发 TTS），音乐随即继续。

### 4. 电池与电源管理

- ADC 滑动平均获取电量，状态栏显示电池图标与百分比（LVGL 内置 `montserrat_14` 字体）；
- 充电检测引脚实时显示充电状态；
- 空闲超时自动关机，按键可唤醒；
- GPIO21 作为电源保持引脚，关机时释放。

## 按键

| 按键 | 短按 | 长按（2 秒） |
| --- | --- | --- |
| BOOT | 对话 / 配网 | — |
| 音量 + | 音量 +10 | — |
| 音量 - | 音量 -10 | — |

播放控制已全部交给语音助手，按键只保留音量调节与唤醒。

## 编译

```sh
source /path/to/esp-idf/export.sh
python3 scripts/build.py dayu-star-wifi --name dayu-star-wifi
```

生成单文件固件（与官方 release 一致）：

```sh
python3 scripts/build.py dayu-star-wifi --name dayu-star-wifi --zip
# 产物：releases/v<version>_dayu-star-wifi.zip（内含 merged-binary.bin）
```

## 烧录

整片写入，地址 `0x0`：

```sh
esptool.py --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 write_flash 0x0 merged-binary.bin
```

或使用 `idf.py -p <PORT> flash`。

预编译固件下载：<https://github.com/yuhua2025/xiaozhi-esp32-dayustar/releases>

## 已知限制

- I2S 输出固定 24 kHz，44.1 kHz 音频必须降采样，12 kHz 以上频段有损失，听感偏"闷"；
- 无回声消除，播放音乐期间需要靠近麦克风、稍大声说唤醒词；
- 无 ID3 标签的文件只能显示序号和时长。
