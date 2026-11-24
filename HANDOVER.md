# NP560+ 模拟器 → M5Stack Tab5 移植交接文档

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 编写日期 | 2026-11-24 |
| 适用对象 | 接手本项目的开发者 / 未来的自己 |
| 一句话现状 | 原厂 NP560.PRG + resident.bin 已在 Tab5（ESP32-P4）上原生解释执行，能进桌面、能响应物理键盘；正在优化开机速度（约 150s → 目标 <20s）。 |
| 主战场工程 | `/home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/`（本目录，独立 git 仓库） |
| 参考实现工程 | `/media/qiao/3b56c13d-bedc-441c-8da8-b6030fb755cb/github/Noah/np560-plus-emulator/`（QEMU 模拟器，语义权威 / oracle） |

> 阅读顺序建议：第 1 章看懂"为什么这么做"，第 2～4 章知道"东西在哪、靠什么跑"，第 5～6 章照着命令操作，第 7 章了解踩过的坑，第 8～9 章接着干活。

---

## 目录

1. [工程由来与总体架构](#1-工程由来与总体架构)
2. [所有工程路径一览](#2-所有工程路径一览)
3. [工程之间的关系](#3-工程之间的关系)
4. [依赖工具与环境](#4-依赖工具与环境)
5. [QEMU 模拟器运行 NP560 系统的方法（教学式）](#5-qemu-模拟器运行-np560-系统的方法教学式)
6. [M5Tab5 工程：编译 / 烧录 / 调试 / 测试](#6-m5tab5-工程编译--烧录--调试--测试)
7. [调试过程中发现的问题与解决方案](#7-调试过程中发现的问题与解决方案)
8. [当前状态与验证结果](#8-当前状态与验证结果)
9. [待解决问题与后续优化方向](#9-待解决问题与后续优化方向)
10. [附录](#10-附录)

---

## 1. 工程由来与总体架构

### 1.1 NP560+ 是什么

诺亚舟 NP560+ 是一款 2000 年代后期的国产学习机，主 CPU 是**自研的 King Billion DMCU 16 位指令集**（不是 ARM / 8051 / MIPS）。它有一个灰度 ST7529 LCD（可见 240×120）、64 键矩阵键盘、24kHz PCM 音频、三星 528MiB 大页 NAND（2048+64 OOB，64 页/块，4096 块）。

关键硬件事实（逆向确认，别再重新论证）：

- CPU 基线时钟 **24.576MHz**（PLL 4MHz×N/D），32.768kHz RTC 时基，两者 750 周期关系。
- 内部 X RAM 32KB（`0x0000..0x7FFF`），Y RAM 自 `00:8000` 起；程序空间 `20:0000..5F:FFFF`；NOR 4MB。
- LCD：ST7529 GRAM 85 列×160 行，2B3P / 3B3P 两种像素格式，双 80-COM bank 接线成 240×120 可见画面。
- NAND 镜像 `system.nand` = 553,648,192 字节 = **528MiB data 平面 + 16MiB OOB 平面**（两个平面不交错，配 64 字节 N1 容器头）。
- 固件 = `NP560.PRG`（4MB 原厂字节）+ `resident.bin`（2MB，QEMU 侧重建的 ROM/HLE）。
- 词典正文**不在 FAT 卷里**，而在 FTL 逻辑块 3739..4074 的 42MiB 隐藏区（固件绕过文件系统按逻辑块直读）。`info.nlb` 只是固件按文件系统找的那条通路的名字。

### 1.2 为什么有两条线

```
NP560+ 真机（逆向对象）
        │
        ├──① QEMU 模拟器 qemu-dmcu/（语义权威 / 快速 oracle）
        │      把 DMCU CPU、SoC、ST7529、键盘、NAND、时钟逐条逆向，
        │      跑原厂固件能进桌面、能查词。QEMU 是"正确行为"的参照物。
        │
        └──② M5Stack Tab5 re-host（本工程，最终目标）
               不移植 QEMU 本身（QEMU 的 TCG/软 MMU 在 ESP32-P4 上不现实），
               而是用纯 C 写一个原生解释器，直接执行同样的原厂固件字节，
               LCD 用 LVGL canvas 显示，键盘接 Tab5 的 A164 模块。
```

**为什么不能直接移植 QEMU**：QEMU 依赖 TCG 动态翻译、软 MMU、完整的 machine/device 模型与 POSIX 运行环境；ESP32-P4 是裸机 RTOS（FreeRTOS），资源（32MB PSRAM 但碎片严重）和运行模型都不匹配。所以走 re-host：复用 QEMU 逆向出的**指令语义与外设行为**，重写宿主层。

**纪律（沿袭 QEMU 主仓库 `doc/plan.md`）**：

1. 语义以 `qemu-dmcu/target/dmcu/helper.c` 和 `hw/dmcu/` 为**唯一权威**；不按"看起来合理"补行为。
2. 未知成本 / 未实现模式显式标注 TODO，不用猜测值掩盖。
3. QEMU 侧的诊断观察器（menu watch、NPE snapshot、trace ring）**不移植**——它们是调试工具，不是架构状态。
4. 每族指令移植后必须过 host 断言，再上真机。
5. **QEMU 当 oracle，真机只做最终验证**：QEMU 启动 ~10-14s，真机启动 ~150s；任何语义分歧先在 QEMU 上秒级复现定位，别反复烧录真机每次等 150 秒。

### 1.3 本项目已验证的关键里程碑

| 时间 | 事件 |
|---|---|
| 2026-11-23 | 固件装载链路全通：NAND ready → PRG/resident 装载 → PC=0x30000 → CPU 持续运行无 halt。 |
| 2026-11-23 深夜 | 确认"升级模式"根因：seed 镜像缺运行期数据，改用 journal 重放后的 `system.nand`。 |
| 2026-11-24 16:00 | 补 EXCH 指令后，真机首次进桌面（每日一句），PC=00:1018（native idle loop）。 |
| 2026-11-24 17:15 | 修 low SFR banked alias 后，64Hz 时基启动，物理键盘恢复响应。 |
| 2026-11-24 傍晚 | OOB 稀疏索引替代块预取，NAND 随机读压成一次顺序扫描（启动优化进行中）。 |

---

## 2. 所有工程路径一览

### 2.1 主战场：Tab5 re-host 工程（本目录，有 git）

```
/home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/          ← git 根（main 分支）
├── NP560_PORT.md                  移植进度清单 + 移植纪律（指令族逐一打勾）
├── HANDOVER.md                    本交接文档
├── README.md                      上游 M5Tab5-UserDemo 自带说明
├── platforms/tab5/                ★ 真正的 ESP-IDF 工程（idf.py 在这里跑）
│   ├── CMakeLists.txt / sdkconfig / sdkconfig.defaults / partitions.csv
│   ├── main/
│   │   ├── app_main.cpp           HAL::init() + np560_emu_start()
│   │   ├── np560_emu.cpp          ★ 模拟器宿主：任务循环、虚拟时钟、5s 诊断日志
│   │   ├── np560_sd.cpp           SD 加载：PRG/resident/system.nand → 内存/介质
│   │   └── np560_tab5_keyboard.cpp  A164 键盘 (I2C 0x6D) → NP560 矩阵触点
│   ├── components/
│   │   ├── dmcu_core/             ★ DMCU CPU 解释器（纯 C99，host 可编译）
│   │   │   ├── dmcu_exec.c/h      指令执行主循环（EXCH 等在此）
│   │   │   ├── dmcu_irq.c         32 源中断仲裁 / 向量 / 嵌套帧 / TRAP
│   │   │   ├── dmcu_bus.c         code/data 总线、段寻址、字节车道、MMIO 钩子
│   │   │   ├── dtu.c              DTU 数据传输单元
│   │   │   └── clock.c            PLL/CLKCON 时钟域
│   │   ├── np560_soc/             ★ NP560 外设模型
│   │   │   ├── np560_periph.c     SFR 设备分发（low SFR alias 修在这里）
│   │   │   ├── np560_lcd.c        ST7529 命令口 + GRAM
│   │   │   ├── np560_keypad.c     64 位矩阵 / F160/F164/F340
│   │   │   ├── np560_nand.c       NAND 控制器 F300
│   │   │   ├── np560_nand_file.c  ★ stdio 文件介质 + OOB 稀疏索引（启动优化）
│   │   │   ├── np560_ecc.c        ECC 引擎 F20A/C/E
│   │   │   ├── np560_timers.c     T0/T1/T2 + 32768Hz 时基
│   │   │   └── np560_audio.c      DAC 事件源（MP3 HLE 尚未移植）
│   │   ├── m5_tab5_keyboard_component/  从 M5Tab5-Keyboard-UserDemo 拷贝（MIT）
│   │   ├── keypad_scanner_tca8418/      官方 Ext Keyboard 底座路径（未接）
│   │   └── m5stack_tab5/                官方 BSP
│   └── build/                     idf.py build 产物
├── host/                          ★ Linux 主机快速回归（make && ./dmcu_runner）
└── tools/serial_capture.py        ★ 串口抓日志脚本（系统 Python + pyserial）
```

### 2.2 参考实现：QEMU 模拟器工程（有 git）

```
/media/qiao/3b56c13d-bedc-441c-8da8-b6030fb755cb/github/Noah/np560-plus-emulator/
├── qemu-dmcu/                     ★ QEMU 源码（new target: dmcu）
│   ├── target/dmcu/               CPU：helper.c（语义权威）、translate.c、clock.c
│   ├── hw/dmcu/                   SoC：np560.c（2 万行板级）、np560_timer.c 等
│   ├── build/qemu-system-dmcu    ★ 已编译可执行（ninja -C build 增量编译）
│   └── doc/                       逆向文档库（274 篇 md + asm 反汇编）
├── scripts/
│   ├── web/np560_web.py          ★ Web/QEMU 桥（启动 QEMU + HTTP API）
│   ├── capture_np560_web_frame.py  从 /api/display 抓 PNG 帧
│   ├── np560_qmp.py              单发 QMP get/set
│   ├── build_np560_system_nand.py  从 V3.80 原包构建 system.nand
│   ├── np560_hidden_area.py / np560_dict_extract.py  隐藏区 / 词典提取
│   └── CATALOG.md                所有脚本逐文件用途表
├── device/device.json            ★ 日常 QEMU 设备配置（ROM/NAND/journal 路径）
├── release/
│   ├── NP560.PRG                  原厂 4MB 固件
│   ├── resident.bin               QEMU 重建 ROM 2MB
│   ├── system.nand                ★ journal 重放后的完整镜像（528MiB+64B，Tab5 用这个）
│   └── system.nand.gz             出厂 seed（单独用会进升级模式，别放 TF 卡）
├── doc/guides/linux_boot_from_poweron.md  ★ 教学式开机复现文档
└── NP560+V3.80版系统升级程序/
    └── NP560+V3.80版系统升级程序.rar  原厂升级包（构建 system.nand 用）
```

### 2.3 TF 卡内容（在 Tab5 设备内 `/sd`，不是在电脑上）

按用户约定，文件**不放根目录**，统一放 `/sd/noahnp560/`，卡建议 FAT32：

| 文件 | 大小 | 来源 | 说明 |
|---|---|---|---|
| `NP560.PRG` | 4 MB | `release/NP560.PRG` | 原厂程序 → data 平面 0x30000 |
| `resident.bin` | 2,095,672 B | `release/resident.bin` | 重建 ROM → code ROM |
| `system.nand` | 553,648,192 B | `release/system.nand` | 完整 NAND 镜像（**已预解压**，设备端解压太慢 ~0.27MiB/s） |

> `system.nand.gz` 不必放卡；设备端 tinfl 流式解压代码保留作兜底，但实测 528MiB 要 ~30 分钟，所以推荐主机 `gzip -dc system.nand.gz > system.nand` 预解压后直接放卡。

---

## 3. 工程之间的关系

```
                        ┌──────────────────────────────────────┐
                        │  QEMU 工程（语义权威 + oracle）        │
                        │  qemu-dmcu/{target,hw}/dmcu           │
                        │  scripts/web/np560_web.py  (HTTP API) │
                        └──────────────┬───────────────────────┘
                                       │
          ① 逆向依据：helper.c / hw 逐条翻译成纯 C
          ② 运行时 oracle：同一份固件在 QEMU 上 ~10s 进桌面，
             用 /api/diagnostics、/api/key-status 对比 Tab5 行为
          ③ 数据供给：release/ 的固件与 system.nand 直接给 Tab5 用
                                       │
                                       ▼
                        ┌──────────────────────────────────────┐
                        │  Tab5 工程（re-host 目标）             │
                        │  dmcu_core/ + np560_soc/ + main/       │
                        │  host/ 回归测试 ←→ platforms/tab5/ 真机│
                        └──────────────┬───────────────────────┘
                                       │ idf.py build flash
                                       ▼
                        ┌──────────────────────────────────────┐
                        │  M5Stack Tab5 硬件（ESP32-P4）         │
                        │  TF: /sd/noahnp560/{PRG,ROM,nand}      │
                        │  LCD: LVGL canvas  ·  KB: A164 I2C     │
                        └──────────────────────────────────────┘
```

**一句话**：QEMU 负责"搞清楚正确行为是什么 + 提供可对比的运行环境"，Tab5 工程负责"在裸机上把同样行为重新实现一遍"，两者共用同一份原厂固件与 NAND 镜像。

### 3.1 git 提交历史（Tab5 仓库）

```
12e2593 Speed up NAND boot with OOB sparse index + self-check   ← 最新（启动优化）
58f3cf4 Fix keyboard: map low SFR bank so 64Hz timebase starts.
e246484 Add timebase/IRQ diagnostics to locate idle keyboard scan stall.
859bc91 Add keypad diagnostics: log matrix reads/active reads and key injection.
fc53ab2 Speed up boot: OOB block prefetch + timer sampling reduction
db72f42 Initial commit: NP560+ emulator port for M5Stack Tab5
```

约定：**每次改动都要 git 提交**（用户明确要求）。工作区应保持干净。

---

## 4. 依赖工具与环境

### 4.1 工具链

| 工具 | 版本 / 路径 | 用途 |
|---|---|---|
| ESP-IDF | **v5.4.2**，`/home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh` | Tab5 固件构建。**必须用 5.4.2**：5.5.3 与芯片 rev v1.0 不兼容；官方 UserDemo 要求 5.4.1 版本线。 |
| QEMU | `qemu-dmcu/build/qemu-system-dmcu`（8.2.10 np560-linux） | oracle；`ninja -C build qemu-system-dmcu` 增量编译。 |
| 系统 Python | `/usr/bin/python3`（pyserial 3.5） | 串口抓日志。**WorkBuddy 的 managed Python 没有 serial 模块**，必须用系统版。 |
| Python（脚本） | `/usr/bin/python3` 或 managed 3.13 均可 | QEMU Web 桥、离线分析脚本。 |
| host gcc/make | `/usr/bin/cc` | host 回归测试 `make && ./dmcu_runner`。 |
| ninja/meson | QEMU build 自带 | QEMU 增量编译。 |
| git | 系统版 | 两个仓库的版本管理。 |

### 4.2 ESP-IDF sdkconfig 关键点（重新生成 sdkconfig 时易丢，逐项核对）

- `ESP32P4_SELECTS_REV_LESS_V3=y` + `ESP32P4_REV_MIN_100=y`（芯片 rev v1.0）。
- `ESP_CONSOLE_USB_SERIAL_JTAG=y` + `ESP_CONSOLE_SECONDARY_NONE=y`。
- `FATFS_LFN_HEAP=y` + `FATFS_MAX_LFN=255`（长文件名必需；无 LFN 时 stat 全部失败，表现为 "missing"）。
- `CODEC_I2C_BACKWARD_COMPATIBLE` **必须为 not set**（设 y 会走 legacy I2C API，ES8388 初始化全部写失败 → assert）。
- `I2C_SKIP_LEGACY_CONFLICT_CHECK=y`（自研键盘组件与 esp_codec_dev 共存）。

### 4.3 沙箱与后台任务（本机特有）

助手的 shell 有 seccomp 过滤且无 new-privileges，长任务 / 需要干净 fork 的工具链用：

```bash
systemd-run --user --unit=<名> --collect /bin/bash -c '<命令>'
```

- 必须用 `--user` + service（默认 Type=simple）；**不要用 `--scope`**（仍继承过滤）。
- 日志重定向到 `/tmp/<名>.log`，`systemctl --user is-active <名>` 看状态，`--wait` 同步等待。
- 调试完必须 `systemctl --user stop <名>.service` 清理，确认无残留（用户明确要求）。
- 注意：`/tmp` 会被清理，重要日志要复制到工程目录或保留在 `~/.workbuddy/` 下。

---

## 5. QEMU 模拟器运行 NP560 系统的方法（教学式）

> 目标：从"开机上电那一刻"复现 NP560 启动画面。完整教学版见
> `/media/qiao/3b56c13d-bedc-441c-8da8-b6030fb755cb/github/Noah/np560-plus-emulator/doc/guides/linux_boot_from_poweron.md`。
> **注意：QEMU 的 `-display` / VNC / screendump 都看不到画面**，必须用浏览器看 `/api/display`（LCD gray5 帧经 QOM `lcd-frame` 出来）。

### 第 1 步：检查关键文件

```bash
ROOT=/media/qiao/3b56c13d-bedc-441c-8da8-b6030fb755cb/github/Noah/np560-plus-emulator
ls -lh $ROOT/release/NP560.PRG $ROOT/release/resident.bin \
      $ROOT/release/system.nand $ROOT/device/device.json \
      $ROOT/qemu-dmcu/build/qemu-system-dmcu \
      $ROOT/scripts/web/np560_web.py
```

要看到 6 个文件都存在（PRG 4MB、resident 2MB、system.nand 528MiB、QEMU 可执行）。

### 第 2 步：前台启动 Web 桥（会占用当前终端）

```bash
cd $ROOT
/usr/bin/python3 scripts/web/np560_web.py \
  --device device/device.json --host 127.0.0.1 --port 8765 \
  --run-log-dir /tmp/np560-boot-runs
```

看到 `NP560+ Web frontend` 和一列 GET 200 说明服务起来了。

### 第 3 步：浏览器看画面

打开 `http://127.0.0.1:8765/`，应该看到 NP560 的 LCD（240×120）。空闲时会自动关屏，按 `↑` 可唤醒。

### 第 4 步：触发复位，从头看启动

```bash
curl -s -X POST http://127.0.0.1:8765/api/control \
  -H 'Content-Type: application/json' -d '{"action":"reset"}'
```

复位后画面先空白 ~0.7s，然后出现"系统初始化，请等待..."，约 6-14s 进入英汉词典桌面。

### 第 5 步：抓帧 / 按词 / 清理

```bash
# 抓当前 LCD 帧到 PNG（--scale 放大便于看清）
/usr/bin/python3 $ROOT/scripts/capture_np560_web_frame.py \
  --url http://127.0.0.1:8765/api/display --output /tmp/frame.png --scale 4

# 按键（如查词 A P P L E ⏎）
curl -s -X POST http://127.0.0.1:8765/api/key \
  -H 'Content-Type: application/json' -d '{"key":"A"}'

# 观察 oracle 诊断（matrix_reads / diagnostics / opcode-profile）
curl -s http://127.0.0.1:8765/api/key-status
curl -s http://127.0.0.1:8765/api/diagnostics
```

结束时 `Ctrl+C` 停 Web 桥，然后确认清理：

```bash
ps aux | grep -E "[n]p560_web|[q]emu-system-dmcu"   # 应无输出
ss -lntp | grep 8765                                # 应无输出
```

### 5.1 QEMU 作为 oracle 的典型用法（调试分歧时）

- **键盘问题**：QEMU idle 态 `matrix_reads` 每秒 +2000~2600；按 Enter 后 `matrix_active_reads` 增长、`ma` 0→19、`event=134(0x86)`。若 Tab5 的 `mr` 恒定不变，说明 guest 没跳出 idle 扫描循环，去查时基 / IRQ。
- **启动卡死**：QEMU 健康启动的标志性 PC = `00:1018`（native idle loop）。`/api/diagnostics` 可看 registers/PC、halted、instruction_count。
- **NAND 行为**：临时在 `hw/dmcu/np560.c` 加 `OOBSTREAM` 打印 page/column/length，就能看到固件对 OOB 的扫描模式（实验完 `git checkout -- hw/dmcu/np560.c` 还原）。

---

## 6. M5Tab5 工程：编译 / 烧录 / 调试 / 测试

### 6.1 host 回归测试（亚秒级，先跑这个）

```bash
cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/host
make && ./dmcu_runner
```

当前 14 组 237 项断言全过（覆盖基础/移位/位域/控制流/重复/算术/中断嵌套/定时器/ECC/NAND 文件介质/code 平面等）。注意：只能测模型层，**测不到 guest 固件行为**。

### 6.2 ESP-IDF 编译

```bash
source /home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh   # 必须 bash
cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/platforms/tab5
idf.py set-target esp32p4   # 已配置过可跳过
idf.py build
```

编译产物在 `platforms/tab5/build/`。

### 6.3 烧录 + 抓串口日志

```bash
# 确认设备（USB 直连，通常是 /dev/ttyACM0；未连接时设备会消失）
ls -l /dev/ttyACM*

# 烧录
source /home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh
cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/platforms/tab5
idf.py -p /dev/ttyACM0 flash

# 抓日志 90 秒（系统 Python！）
/usr/bin/python3 /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/tools/serial_capture.py 90 \
  > /tmp/boot.log 2>&1
```

`serial_capture.py` 默认会 DTR/RTS 脉冲复位芯片；如果设备已在运行（如刚 flash 完），加 `--no-reset` 直接抓。

### 6.4 诊断日志字段说明（`np560: run:` 每 5s 一行）

| 字段 | 含义 | 健康值 |
|---|---|---|
| `PC=` | guest 当前 PC | 进桌面后 `00:1018`（native idle） |
| `cyc/s` | 虚拟周期推进速率 | ≈24.58M（若 0 = 虚拟时钟冻结） |
| `op/oh` | OOB 块预取次数 / 命中次数 | 优化后应为 `0` |
| `nand: reads/prog/erase/pf/rf/...` | NAND 介质统计 | `pf=0 rf=0`（无预取失败/读失败）；新增 sparse 命中计数 |
| `keypad: mr/ma/mask/code` | 键盘矩阵读次数 / 有效读 / 掩码 / 键码 | 修复后 `mr` 持续增长、`ma` 随按键变化 |
| `timebase: hz/exp/run/acc/tbicr/socr` | 64Hz 时基状态 | `hz=64 exp=63 run=1 tbicr=0031 socr=a082 irq_pending=1` |

### 6.5 真机验证节奏（避免浪费时间）

1. 先在 QEMU 上用 oracle 定位问题（秒级），再改代码。
2. host 回归 `make && ./dmcu_runner` 确认模型层无回归。
3. `idf.py build flash`。
4. 抓日志**必须覆盖到进桌面（约 150s）**：前 30s 还在 NAND 扫描、尚未执行时基初始化，窗口太早会误判"修复无效"。
5. 验证成功后清理调试进程与日志。

---

## 7. 调试过程中发现的问题与解决方案

> 这一章是本项目最值钱的部分。每条都按"现象 → 根因 → 修复"记录。

### 7.1 构建 / 环境类

**① ESP-IDF 5.5.3 编译失败**
- 现象：python env 不匹配、芯片 rev 不兼容。
- 解决：切到 **v5.4.2**，export.sh 用 bash source。

**② 静态大缓冲把 bss 撑爆**
- 现象：链接失败 / 栈保护 fault。
- 根因：`s_canvas_buf`(225KB) + `s_frame`(56KB) 放 bss，总 bss 458KB 超内部 SRAM。
- 解决：改用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`。

**③ rs485 任务栈溢出**
- 现象：Stack protection fault。
- 根因：官方 `hal_rs485.cpp` 任务栈 2000 太小，`mooncake_log(fmt v11)` 格式化溢出。
- 解决：栈提到 4096。

**④ LFN 未开 → 固件加载报 "missing"**
- 解决：`FATFS_LFN_HEAP=y` + `FATFS_MAX_LFN=255`。

**⑤ ES8388 初始化全部写失败 → assert**
- 根因：`CODEC_I2C_BACKWARD_COMPATIBLE=y` 走 legacy I2C，与 BSP 新驱动总线冲突。
- 解决：设为 not set。

**⑥ 沙箱无法 sudo / seccomp 继承**
- 解决：`systemd-run --user --unit=X --collect`（见 4.3）。

### 7.2 SD / NAND 数据类

**⑦ tinfl 流式解压两次大坑（现已不常用，保留兜底）**
- 输出必须用 32KB 环形字典（wrapping 模式）；NON_WRAPPING 下换缓冲后 LZ 回引历史丢失。
- `tinfl_decompress` 退出时 `*pIn_buf_size` = **已消费**字节数；曾误当"未消费"重喂导致流失步。
- 镜像尾部实测 553648192 = 528MiB + 64B，容差检查要用 `expect + NP560_NAND_PAGE_OOB`。
- 中断残留的不完整 `system.nand` 自动删除重解压。
- **最终方案**：主机预解压后直接放卡，设备端跳过解压。

**⑧ "系统初始化，请等待... / 完成FAT:mount程式" 死等**
- 现象：LCD 显示初始化框后不动。
- 根因：`release/system.nand.gz` 是**出厂 seed**，固件需要 NAND 运行期数据；缺失时走升级模式分支死等主机。
- 排查：QEMU 对照——日常 device（journal 重放）正常进桌面；干净 checkpoint（无 journal）复现同样卡死。
- 解决：用 QEMU QMP `qom-set nand-save` 导出 journal 重放后的 `release/system.nand`（553,648,192B），放 TF 卡。

**⑨ TF 镜像污染导致 `FAT: Init Format error!`**
- 现象：特定块 page 63 读出 `fe ca 43 08` ≠ 干净镜像的 `10 ac 00 dc`。
- 解决：重拷 `release/system.nand` 到 TF 卡。另有一次是优化代码的 off-by-one（见 7.4 节 ⑰）引起，别混淆。

### 7.3 CPU / 外设语义类

**⑩ `code_rom alloc failed`（固件装载失败）**
- 根因：read_file 已持有 4MB PRG + 2MB ROM 临时缓冲，再 malloc 2MB 拷贝；此时 PSRAM `free=6361712 largest=6291456`，连续 2MB 分配失败。
- 解决：新增 `np560_bus_take_code_rom()` 零拷贝所有权转移，`load_firmware` 内置 NULL 防双重释放。
- 教训：PSRAM 空闲 6MB 但**最大连续块仅 ~6MB**，任何 2MB 二次拷贝都可能失败。

**⑪ 复位 PC 未更新**
- 根因：`cpu_reset()` 在固件装载前调用（code_rom 尚为 NULL），PC 兜底 0x200000。
- 解决：装载成功后**只重设** `s_cpu.pc = 0x30000` 与 `resident_rom_limit`；不可重复整个 `cpu_reset`（memset 会清掉 periph_init 挂上的 SFR 回调）。

**⑫ 虚拟时钟冻结（定时器中断永不触发）**
- 现象：`cyc/s=0`、`irq_pending=0`、SFR 热点全空。
- 根因：`dmcu_exec_step` 不推进 cycles，而 `np560_periph_advance` 用 cycles 差值推进外设 → 虚拟时钟冻结。
- 解决：emulator task 按主机真实时间推进虚拟周期（`esp_timer_get_time` 差值 × system_clock_hz），`cyc/s≈24.58M`。
- 遗留：QEMU 权威配置是 `--instruction-clock`（icount 指令时钟），wall-clock 推进是过渡方案，解释器速率不足时定时器相对指令流加速（约 1.4% 实时偏差），行为差异待对齐。

**⑬ EXCH 指令漏移植 → 进不了桌面（卡在"已进入系统运行就绪状态"）**
- 现象：日志 `CPU halted: 0eb0 @ 2a:aa24`，长达 162s 停在"就绪状态"。
- 根因：`dmcu_exec.c` 漏了 EXCH（`op=0x0E`，交换两个 16 位 GPR，不改标志位）。
- 解决：补 `tmp=gpr[arg>>4]; gpr[arg>>4]=gpr[arg&15]; gpr[arg&15]=tmp;`
- 验证：修复后 162.759s 首次 `PC=00:1018`（与 QEMU 健康启动一致），进"每日一句"主界面。

**⑭ 键盘无反应 → low SFR banked alias 缺失**
- 现象：进桌面后物理按键无反应；注入正确（`KB key_id=55 → matrix=53 (Enter)`，mask 正确变化），但 `keypad: mr=8` 恒定、`ma=0`、`irq_pending=0`。
- 定位：QEMU oracle idle 态 `matrix_reads` 每秒 +2000~2600；按 Enter 后 ma 0→19。差异在 guest 没跳出 idle 扫描循环。
- 根因（QEMU `np560.c:20914` 注释实锤）：原厂在 DS0=1 下写逻辑 `715E`(TBC=8009) / `71A0`(TBICR=0031)，bank 映射成物理 `F15E`/`F1A0`；Tab5 的 MMIO 分发只特殊处理 F100(SOCR)，F15E/F1A0 落入丢弃 → TBICR.EN 永 0 → 64Hz 时基不启动 → idle loop 跳不出 → 键盘永不扫描。
- 诊断铁证：修复前 `hz=32768 exp=0 run=0 tbicr=0000`；修复后 `hz=64 exp=63 run=1 tbicr=0031 socr=a082 irq_pending=1`。
- 解决（commit `58f3cf4`）：`np560_periph.c` 的 MMIO read/write 末尾加 low SFR fallback `physical F100..F1FF → dmcu_sfr_read/write((physical-F100)>>1)`；专用设备（F160/164 键盘、F20A ECC、F300 NAND）分支在前不受影响。
- 验证：修复后 `mr 8→94`、`ma 0→16`、mask 随按键变化，用户确认屏幕有反应。
- 教训：Tab5 的 MMIO 分发器是 QEMU 内存映射的简化版；QEMU 里每个 memory region + alias 都要确认 Tab5 有对应分发，banked alias 尤其容易漏。

### 7.4 启动速度优化类（进行中）

**⑮ OOB 块预取效果有限**
- 尝试：`np560_nand_file.c` 加 4KB OOB 块预取（一块 64 页 OOB 物理连续）。
- 结果：162.8s → 152.6s，仅快 6%。`op=23091`（预取读盘）+ `oh=46087`（命中）= 69178，命中率仅 66.7%；4096 块却预取 23091 次 = 每块重复预取 ~5.6 次，单块缓存被击穿。
- 根因：I/O trace 全是 'O'（OOB 预取），page 序列呈**跨块跳跃**（16320→32704→49088，每 ~16384 页一跳），且多遍扫描步长略不同。固件 FTL tag 扫描是"跨块跳读每块一页 OOB"，块内 64 页连续假设不成立。
- 结论：方向应是全盘 OOB 稀疏索引，而非块内预取。

**⑯ 稀疏索引三次内存失败 → 高半区 + 分块方案**
- 第一次：整页 OOB 索引 3.9MB 连续 PSRAM 分配失败（碎片）。
- 第二次：分 64 chunk 仍失败。
- QEMU 实验证实：只保留 OOB 高 32 字节（FTL tag 在 56..63，全 32 字节掩码实验）固件就能正常进桌面 → 索引内存从 3.4MB 降到 1.7MB。
- 最终：`OobSparse` 存每页**高 32 字节**，64 chunk 独立分配，rank(段起点累计数)+popcount(present 位图) 查询，构建时 256KB 批量顺序读。

**⑰ `FAT: Init Format error!`（优化引入的回归）**
- 现象：稀疏索引版启动到 FAT 阶段报错（用户两次报告）。
- 根因：**rank 数组 off-by-one**——构建时 `rank[段号]` 在段末尾（`p&255==255`）且只在页面非 FF 时写；查询时却当作段起点之前的累计数叠加 popcount，导致段内页 OOB 索引整体偏移，FTL tag 读到别的页数据 → FAT 崩。
- 修复：在 256 页段的**起点无条件**记录累计数。
- 加固：构建时抽样 64 页，构建完立刻走查询路径回读比对，日志直接报 `OOB index self-check: 13/13 passed`（实测全过）。
- 教训：索引类优化必须有自检，不能靠"等 FAT 阶段崩不崩"间接判断。

---

## 8. 当前状态与验证结果

### 8.1 已完成 / 已验证

- [x] 原厂固件在 Tab5 上原生执行，进桌面（"每日一句"），PC=00:1018，无 halt/fault。
- [x] 物理键盘响应（low SFR alias 修复后，用户确认屏幕有反应）。
- [x] host 回归 14 组 237 断言全过。
- [x] OOB 稀疏索引优化已提交（`12e2593`）：块预取降到 0 次，纯执行速率从 ~14 万提升到 ~66 万指令/秒；`OOB index self-check 13/13 passed`；启动推进到 62 秒时已执行 2482 万指令、无 FAT 错误。

### 8.2 进行中 / 待最终确认

- [ ] 稀疏索引版**完整启动到桌面的总耗时**尚未取得最终数字（最后一份验证日志在 `/tmp` 被清理前只观察到 62s 进度正常，未抓到进桌面时刻）。需要重新烧录并抓一次完整 150s+ 日志，对比优化前 162.759s。
- [ ] 开机顺序扫描 16MB OOB 平面耗时约 10.5 秒（`OOB sparse index ready ... scan 10483ms`），可进一步优化（见 9.2）。

### 8.3 当前 git 状态

Tab5 仓库 `main` 分支，最新提交 `12e2593`，工作区应干净（除本交接文档与新增 tools 脚本提交前）。QEMU 仓库临时诊断代码已 `git checkout` 还原，保持干净。

---

## 9. 待解决问题与后续优化方向

### 9.1 功能待办（按 NP560_PORT.md）

| 项 | 位置 | 规模 | 说明 |
|---|---|---|---|
| 音频 HLE | `hw/dmcu/np560_audio.c` 剩余 + minimp3 | 大 | MP3 解码链路（核 1）、EQ；portable 配置 `--rom-mp3-hle-base 138790 --rom-mp3-arena-image 93952 --rom-npe-codec-owned` |
| DICTSPK 发音 | 专有声码器 | 大 | `DICTSPK.NLB` 24kHz 声码器未破解（熵 7.9bit，非 ADPCM）；ROM 里没有解码例程 |
| DTUC 别名 | `regs.c` | 小 | REG[21..24] 与内存 F142..F148 共享状态 |
| TCA8418 键盘 | `components/keypad_scanner_tca8418` | 小 | 官方 Ext Keyboard 底座路径（真机按需接入） |
| overlay alias 精确化 | `np560.c 22560-22700` | 中 | vma03 overlay alias 现用 phys 优先近似；Tab5 走 2a:aa24 / QEMU 走 2f:5082 的路径分歧源于未实现 late overlays writable RAM |

### 9.2 启动速度继续优化（用户明确要求"一定要优化"）

1. **完成稀疏索引最终验证**：抓完整启动日志，量化优化后耗时。
2. **离线预生成索引文件**：把 1.7MB 高半区 OOB 索引在主机生成好放 `/sd/noahnp560/`，设备启动时一次读入（~1s），省掉 10.5s 顺序扫描。需要往 TF 卡加一个文件（一次性操作）。
3. **时钟模型对齐**：把 wall-clock 虚拟周期改成与 QEMU icount 一致，消除 1.4% 偏差（也让启动扫描的周期数更接近 QEMU，可能间接影响耗时）。
4. **其他候选**：多块顺序预读、减少 fseek、OOB sidecar（注意 PSRAM 碎片）。

### 9.3 工程改进

- 把 `/tmp` 里的验证日志固化到工程 `logs/` 或 `~/.workbuddy/`，避免被清理。
- 每次真机验证后清理 systemd-run 单元、串口抓日志进程（用户明确要求）。
- 保持每次改动 git 提交。

---

## 10. 附录

### 10.1 常用命令速查

```bash
# ---- QEMU oracle ----
ROOT=/media/qiao/3b56c13d-bedc-441c-8da8-b6030fb755cb/github/Noah/np560-plus-emulator
cd $ROOT && /usr/bin/python3 scripts/web/np560_web.py --device device/device.json \
  --host 127.0.0.1 --port 8765 --run-log-dir /tmp/np560-boot-runs
curl -s -X POST http://127.0.0.1:8765/api/control -H 'Content-Type: application/json' \
  -d '{"action":"reset"}'
curl -s http://127.0.0.1:8765/api/diagnostics
cd $ROOT/qemu-dmcu/build && ninja qemu-system-dmcu       # QEMU 增量编译

# ---- Tab5 host 回归 ----
cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/host && make && ./dmcu_runner

# ---- Tab5 固件 ----
source /home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh
cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/platforms/tab5
idf.py build && idf.py -p /dev/ttyACM0 flash

# ---- 串口抓日志（系统 Python！）----
/usr/bin/python3 /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/tools/serial_capture.py \
  120 --no-reset > /tmp/boot.log 2>&1

# ---- 清理 ----
systemctl --user stop np560-xxx.service
ps aux | grep -E "[n]p560_web|[q]emu-system-dmcu|[n]p560_serial_capture"
ss -lntp | grep 8765
```

### 10.2 关键参考文档（QEMU 工程 doc/ 下）

| 主题 | 文档 |
|---|---|
| 开机复现教学 | `doc/guides/linux_boot_from_poweron.md` |
| 总体架构约束 | `doc/plan.md`、`doc/history/README-NP560.md` |
| 64Hz 时基修复 | `doc/hardware/np560_timebase.md` |
| 低 SFR / SOCR 契约 | `doc/hardware/np560_timer2_and_bit_sfr_contract.md` |
| EXCH 指令周期 | `doc/cpu/np560_instruction_costs.md` |
| NAND journal / OOB | `doc/storage/np560_nand_journal.md` |
| system.nand 构建 | `doc/storage/np560_system_nand_build.md` |
| 词典隐藏区 | `doc/storage/np560_hidden_library_area.md` |
| 固件启动契约 | `doc/firmware/NP560_resident_boot_contract.md` |

### 10.3 关键工具脚本（QEMU 工程 scripts/ 下）

- `web/np560_web.py`：Web/QEMU 桥，提供 `/api/health`、`/api/control`、`/api/key`、`/api/display`、`/api/diagnostics`、`/api/key-status`、`/api/opcode-profile`。
- `capture_np560_web_frame.py`：抓 LCD 帧到 PNG。
- `np560_qmp.py`：单发 QMP get/set（如 `qom-set nand-save` 导出镜像）。
- `build_np560_system_nand.py`：从 V3.80 原包（SHA `6f484f70...`）构建完整镜像。
- `np560_hidden_area.py` / `np560_dict_extract.py`：隐藏区与词典提取。
- `CATALOG.md`：全脚本用途索引。

### 10.4 术语表

| 术语 | 含义 |
|---|---|
| DMCU | NP560+ 自研 16 位 CPU 指令集 |
| re-host | 不移植 QEMU 本身，而是重写宿主层直接执行原厂固件 |
| oracle | QEMU 参考实现，用于对比定位 Tab5 行为分歧 |
| FTL | Flash Translation Layer，逻辑块→物理块映射，靠每块最后一页 OOB 尾部的 3×u16 tag 重建 |
| OOB | NAND 每页 64 字节带外区，存 ECC / FTL tag |
| resident.bin | QEMU 重建的 ROM/HLE（2MB），不是原厂文件 |
| seed | `system.nand.gz` 出厂镜像，缺运行期数据会进升级模式 |
| low SFR alias | 逻辑 715E/71A0 在 DS0=1 下映射到物理 F15E/F1A0 的 banked 视图 |
| icount | QEMU 指令时钟（`--instruction-clock --icount-shift 0`） |
