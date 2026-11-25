# m5tab5-np560

在 **M5Stack Tab5**（ESP32-P4）上以 **re-host** 方式运行诺亚舟 NP560+ 学习机固件的实验性移植。

> 本仓库不是 QEMU 模拟器本体。DMCU 指令集与外设语义由独立的 QEMU 工程作为 oracle 参考，本仓库只包含面向 Tab5 的解释器、SoC 外设桥、皮肤资源与构建工具。

## 当前状态

- 已能从 TF 卡装载固件并启动到桌面。
- LCD 走 ST7529 语义模拟，Web 32 级绿灰配色，全屏皮肤合成 + 2.47x 放大。
- 物理键盘可用；Web 上半部分透明触摸热区当前 **18 个**（MP3 入口暂时禁用）。
- host 回归全部通过。
- **已知问题**：进入 MP3 后按“返回”会让 guest CPU 陷入 `01:0000` 的 TRAP 自陷，触摸和物理按键都会失去响应；详见 [TODO.md](TODO.md)。

## 硬件与 TF 卡

- 设备：M5Stack Tab5（ESP32-P4NRW32，MIPI-DSI 720×1280 触摸屏）。
- TF 卡需为 FAT32，文件**不要放根目录**，统一放在 `/sd/noahnp560/`：
  - `NP560.PRG`（原厂程序，4 MB）
  - `resident.bin`（重建 ROM，约 2 MB）
  - `system.nand`（QEMU journal 重放后的完整镜像，约 528 MiB）
- 皮肤资源已固化在 Flash 的 `np560skin` 分区，不放在 TF 卡上。

## 目录结构

```text
app/                        UserDemo 应用与皮肤素材
host/                       主机回归测试（dmcu_runner）
platforms/tab5/
  components/dmcu_core/     DMCU CPU 解释器、总线、MP3 HLE、minimp3
  components/np560_soc/     LCD / NAND / 键盘 / 音频等外设桥
  main/                     ESP-IDF 主程序、触摸热区、固件装载
  partitions.csv            分区表（含 np560skin 分区）
  skin_part.bin             皮肤分区镜像
tools/                      皮肤生成、串口抓日志等辅助脚本
*.md                        移植评估、性能对比与交接文档
```

## 构建与烧录

约定：

- `$REPO_ROOT` = 本仓库根目录
- `$IDF_PATH` = ESP-IDF 安装目录（本工程验证过 **v5.4.2**，不要用 v5.5.3）
- `$PORT` = 设备串口（例如 `/dev/ttyACM0`）

```bash
source "$IDF_PATH/export.sh"
cd "$REPO_ROOT/platforms/tab5"
idf.py build
```

烧录应用分区：

```bash
idf.py -p "$PORT" flash
```

皮肤分区镜像（当前分区表偏移为 `0xc74000`，请以 `idf.py partition-table` 输出为准）：

```bash
python -m esptool --chip esp32p4 -p "$PORT" write_flash 0xc74000 \
  "$REPO_ROOT/platforms/tab5/skin_part.bin"
```

## 主机回归

```bash
cd "$REPO_ROOT/host"
make dmcu_runner
./dmcu_runner
```

## 文档索引

- [TODO.md](TODO.md) — 当前待办与已知问题
- [NP560_PORT.md](NP560_PORT.md) — 移植路线与里程碑
- [HANDOVER.md](HANDOVER.md) — 详细交接记录、命令与背景
- [BRANCH_PERFORMANCE_COMPARISON.md](BRANCH_PERFORMANCE_COMPARISON.md) — fastboot 前后性能对比
- [DISPLAY_FLUENCY_OPTIMIZATION.md](DISPLAY_FLUENCY_OPTIMIZATION.md) — 显示流畅化与不变量
- [NATIVE_DISPLAY_EVALUATION.md](NATIVE_DISPLAY_EVALUATION.md) — 原生显示后端评估
- [NP560_SKIN_ASSET_EVALUATION.md](NP560_SKIN_ASSET_EVALUATION.md) — 皮肤资源与全屏合成评估

> 上述历史文档记录的是开发过程与结论，路径已统一改为占位符；实际操作时请按本机环境替换。

## 致谢

本项目基于 M5Stack UserDemo，并参考以下开源项目/资源：LVGL、Espressif esp_lvgl_port / esp_cam_sensor / esp_ipa / esp_sccb_intf / esp_video、BMI270 SensorAPI、Arduino-INA226、smooth_ui_toolkit、mooncake、mooncake_log 等。详见原始致谢列表与 `LICENSE`。

## 许可证

MIT License, Copyright (c) 2025 M5Stack Technology CO LTD. 详见 [LICENSE](LICENSE)。
