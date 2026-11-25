# NP560+ Tab5 移植（re-host）
> 路径约定：`$REPO_ROOT` = 本仓库根目录，`$IDF_PATH` = ESP-IDF 安装目录，`$ORACLE_ROOT` = QEMU 参考工程根目录。

本工程（`m5tab5-np560/`）是 **M5Stack Tab5（ESP32-P4）上的 NP560+ 模拟器
re-host 工程主战场**：基于 M5Tab5-UserDemo 完整官方工程（保留其 app 框架、
HAL、全部板级驱动），叠加 DMCU 解释器核心。不是移植 QEMU 本身——而是复用
np560-plus-emulator 仓库 `qemu-dmcu/` 已逆向完成的指令语义与外设行为，
写一个原生解释器直接运行原厂固件（NP560.PRG + resident.bin）。

## 本工程内模拟器代码位置

```
m5tab5-np560/
├── platforms/tab5/components/dmcu_core/   DMCU CPU 解释器核心（纯 C99，host 可编译）
├── platforms/tab5/components/np560_soc/   NP560+ 外设模型（LCD GRAM/按键）
├── platforms/tab5/main/np560_emu.cpp      模拟器宿主（LVGL9 canvas 显示 + 模拟器任务）
├── platforms/tab5/main/app_main.cpp       入口：HalEsp32::init() + np560_emu_start()
├── host/                                  Linux 主机回归（make && ./dmcu_runner）
└── NP560_PORT.md                          本文件：进度清单与移植纪律
```

app_main 已停用 mooncake 应用框架流程，改为 HAL 硬件初始化 + 模拟器直启；
app/ 目录源码仍编译（保留待后续接回框架做应用壳）。

## 目录结构

```
esp32-tab5/
├── CMakeLists.txt / sdkconfig.defaults / partitions.csv   ESP-IDF 工程配置
├── components/
│   ├── m5stack_tab5/     Tab5 BSP（从官方 M5Tab5-UserDemo 拷贝）
│   ├── dmcu_core/        DMCU CPU 解释器核心（纯 C99，host 可编译）
│   └── np560_soc/        NP560+ 外设模型（LCD GRAM/按键矩阵/NAND 桩）
├── main/                 ESP-IDF 入口：板级初始化、SD 固件加载、模拟器任务
├── host/                 Linux 主机侧快速回归（make && ./dmcu_runner）
└── images/               放 NP560.PRG / resident.bin / system.nand.gz
```

## 已完成（2026-11-23）

- [x] 工程骨架 + BSP 集成
- [x] CPU 状态模型（pc/gpr[16]/sfr[256]/psw/SB/SE，对齐 cpu.h）
- [x] 取指与指令长度（translate.c `dmcu_is_32bit`/helper.c `dmcu_insn_size`）
- [x] 段式代码寻址（CS:PC，PC 进位不改 CS）
- [x] 三个地址空间：扁平代码 NOR（0x200000, 3MB）、DS0/DS1 分段数据总线、
      内部端口 64KB（X RAM/Y RAM/DPRAM，FP/SP/Y 指针绕过总线）
- [x] 字节车道规则（16 位总线偶地址=高字节；对内部端口同样适用）
- [x] 寄存器别名（RBn 高低字节、RB10=SE、sfr>=0xF0→GPR、PSW 位序 ZV CN、SB）
- [x] 数据寻址（DS0/DS1 分段、IGNDS 未分段窗口、F2E0..F2FE GPR 直接别名）
- [x] 指针 AGU（X0/X2/Y2 循环缓冲：长度字节、base 对齐 2 的幂）
- [x] ALU 全套（ADD/ADDC/SUB/SUBC/AND/OR/CMP/XOR + ZV CN 标志、链式 Z）
- [x] **exec_move 全族**（helper.c:7102，寄存器/立即/指针/SFR/内存各形态）
- [x] **exec_alu 全族**（helper.c:7023，全部 12 种 hi 操作数形态）
- [x] **exec_shift 全族**（helper.c:7238：SL/SR/ASR/ASL/ROL/ROR/SLOSB/SROSB，
      含 SB 缓冲、负计数转派、ASL 饱和、V 丢弃位 OR 语义）
- [x] **exec_bit 全族**（helper.c:7415：0xE0+ 位置位/清零、BAND/BOR/BXOR/
      BMOV/BMOVN/BCMP，双输入位标志 + PSR 位写覆盖规则）
- [x] **exec_control 语义核心**（helper.c:18346 剥离观察器：0xAC/0xBC 条件
      近跳转、0xAD/0xBD 远跳/远调用（含 HLE 钩子 + mask ROM 窗口）、
      0xCC/0xCE 经寄存器跳转、0xCD 近调用、0xD0..0xDE 条件跳转、
      0x9A..0x9D 位测试跳转、RETS/RETF/RETI）
- [x] 主循环补充：MUL/UMUL（R6:R7 + SOCR.FR 小数模式）、PUSH/POP SFR（0x1F）、
      LINK/UNLINK（0x2E/0x2F）、NEG（0x3E）、CRIT/IGNDS（0x3F）、
      REPS/REPB（0xBF/0xCF/0xAE/0xAF + 完成收尾回绕）、RESET（0x3FFF）
- [x] 完成收尾语义（helper.c 24255-24318）：IGNDS/CRIT 窗口递减、
      重复引擎回绕、DS 切换/窗口到期/IRQ 变化返回码
- [x] NOP 0xc7
- [x] 时钟域（clock.c：PLL/CLKCON 频率、外部等待、文档化指令成本表）
- [x] **exec_compare_update**（helper.c:20006：CMPD/CMPI 0x4E..0x9F 同族，
      寄存器/SFR×立即/内存三形态，比较+按 delta 自增减）
- [x] **exec_mac**（helper.c:20046：memory MAC 0xBE，X/Y 指针环形 AGU、
      R5低+R6:R7 40 位累加器、SE 独立、RNDST/MULS、.m 回写、舍入/饱和、
      PSR.MV/MS）
- [x] **exec_divide**（helper.c:20174：DIV/DIVL/UDIV/UDIVL 0xC2..0xC5，
      商 R7 余 R6，除零/溢出只置 V）
- [x] **exec_exponent**（helper.c:20215：NORM/NORMF 0x1E，kind0/1 有符号
      减一、kind2/3 无符号、kind4+ 块形式仅在更小时写 RB10）
- [x] **中断系统**（dmcu_irq.c，剥离 cpu.c/dtu.c 全部观察器）：
      ICR 32 源仲裁（EN/RQ/PRI/ED、CPRI 阈值、CIRQ 同级阻塞、升序并列）、
      向量表（0x40+s*8 / 0xC0+(s-16)*4，SOCR.RAM 选段）、嵌套帧
      （PC/CS/PSW，NSEG 决定 4/6 字节）、重复影子挂起/恢复、TRAP（0x0F，
      CN1916884A p124：trap*4、NSEG=当前段否则 CS=1）、RETI 簿记
      （弹前帧位置匹配才结算、合成 RETI 不提前结束外层）、CRIT/IGNDS
      窗口屏蔽、SP/FP 重叠延后、ICR/SOCR/PSW 写触发重仲裁
- [x] **DTU 数据传输单元**（dtu.c:118 原样移植）：4 通道 DTUC
      （REG[21..24]）、指针表 F0F0+（unbanked 别名折叠）、逐请求传输、
      INC 方向、WBT 字节车道、COUNT 归零完成请求以优先级 3 交 CPU、
      故障源标记；外设入口 dmcu_irq_raise
- [x] host 回归测试 8 组 75 项断言全部通过（基础/移位/位域/控制流/重复/
      杂项/算术/中断嵌套+TRAP）
- [x] bit_read/bit_write（低外设位地址/70..7F GPR 别名/DPRAM）
- [x] **定时器外设**（np560_soc/np560_timers.c，移植 hw/dmcu/np560_timer.c，
      ptimer → CPU 周期累加器）：T0/T1/T2 周期模式（TxC=0x8080、limit=TxP+1、
      上计数回读、完整首周期重启）、32768kHz 时基（32768>>select，limit=1）、
      到期 → ICR 源 1..3 / 源 0 RQ 锁存；CLKCON 写 SOCR.UP 门控
- [x] **音频 DAC 事件源**（np560_soc/np560_audio.c，移植 hw/dmcu/np560_audio.c
      架构子集）：DAC0/1 样本锁存、DACC/AUDIO_PLL 速率公式
      （mode 8/10、decimation 1024/1536）、请求节拍 → ICR 源 17/16、
      48kHz PCM 呈现环；enabled=(control&0x8001)==1
- [x] **板级组合层**（np560_soc/np560_periph.c）：SFR 设备钩子分发
      （cpu->sfr_device_read/write，对应 np560_sfr_device_*）、统一时间推进
      np560_periph_advance(delta_cycles)
- [x] host 回归测试 10 组 104 项断言全部通过（新增：定时器 T1 到期/重装/
      计数回读/ICR2 RQ→ISR 进入、时基 1 秒 32768 触发、CLKCON 门控、
      音频请求时钟/ICR16-17/样本锁存/禁用保持）
- [x] **ST7529 LCD 命令口**（np560_soc/np560_lcd.c，移植 hw/dmcu/np560.c
      :9290-9836，剥离 frame/status 诊断 trace）：命令参数表（EXT0/EXT1
      双 bank）、CASET/LASET 窗口钳位、DATSDR 2B3P/3B3P 拆包与 C/L 扫描
      方向、窗口回绕 completed_frames、RAMRD 总线保持器哑读、RMWIN/RMWOUT
      、SLPIN/SLPOUT/DISINV/DISNOR/DISON/DISOFF、部分显示（PTLIN 四行块
      越界无操作）、COMSCN/ASCSET/SCSTART/DISCTRL、EXT=1 Gray1/2 表与
      ANASET/EPCTIN 参数计数；可见面 240x120 输出（NP560 玻璃双 80-COM
      bank 固定接线 + COMSCN + LI/CI + 光学浓度 0..31，np560.c:9576-9615
      注释的 V3.80 COMSCN=001 → 可见行读 GRAM 139..20 实证）
- [x] **总线字节 MMIO 路由**（dmcu_bus.c）：ST7529 物理区间 0xF00000
      ..0xF00003 仅字节事务有效（QEMU valid.min=max=1，16 位读 0/写丢弃）
      ，字节事务经 mmio_read8/mmio_write8 钩子直达设备（offset=平坦&3），
      不做读改写；np560_periph_init(periph, cpu, bus) 安装钩子
- [x] **修复字节车道双重交换 bug**（dmcu_bus.c translated 8 位路径）：
      旧代码 ^1 车道交换后又按车道奇偶选字节，等于交换两次；权威证据
      helper.c:1834（偶车道=高字节）、helper.c:4044（逻辑 386C→物理
      B86D）、doc/cpu/np560_dtu.md:38。修复后 internal/translated/DTU
      phys8 三路径一致（平坦字节 = 车道地址 ^1，LSB-first 存储）；
      runner A 组字节断言按 QEMU 语义重写（对齐偶/奇车道写高/低字节）
- [x] host 回归测试 11 组 151 项断言全部通过（新增 K 组 45 项：复位态/
      STREAD/3B3P+2B3P 拆包/窗口回绕/未完成组保持/RAMRD 哑读/RMW 读写/
      EXT 灰度表/EXT 阻断 RAM 命令/COMSCN 可见接线/总线命令-数据-状态
      路由/16 位拒绝）
- [x] **键盘矩阵 F160/F340**（np560_soc/np560_keypad.c，移植 hw/dmcu/
      np560.c:8983-9288）：64 位矩阵状态 bit=col*8+row（V3.80 扫描器
      2E:5F28）、行选通低有效（drive OR C0FC 抬行 AND ~row 清位）、
      同列多键合并、F160 bit12 外部电源输入、F164 驱动寄存器回读、
      F340 事件锁存（导航别名 0x82..0x85、读清/写 0 清、offset6 原始码）、
      legacy 16 位键状态展开表（web_key_state 兼容路径）；word MMIO
      总线钩子（0xF100..0xF3FF）由 np560_periph 安装。注：QEMU 的
      matrix_pulse_index 合成脉冲是 web 指针点击专用，Tab5 实键为
      保持态驱动，未移植
- [x] **Tab5 物理键盘桥**（main/np560_tab5_keyboard.cpp + 拷贝
      M5Tab5-Keyboard-UserDemo 的 m5_tab5_keyboard_component，MIT）：
      A164 键盘模块 STM32F030 @I2C 0x6D（SDA=0/SCL=1，port1 自建、
      轮询模式避开 INT50 与 TCA8418 共享），事件 key_id=row*14+col
      → 70 键映射表 → NP560 矩阵触点（布局证据 scripts/web/app.js
      matrixIndexes：字母/数字共触点、方向键 61/37/45/29、Enter53、
      ESC/BS 28、空格/句点 54、SYM 26）；未插键盘每 5s 重试。
      另发现官方 Ext Keyboard 底座为 TCA8418 @0x34 挂 BSP EXT I2C
      （GPIO53/54，8×9 矩阵，components/keypad_scanner_tca8418 已在
      工程内）——两条硬件路径，后续真机验证时按实际键盘接入方式切换
- [x] host 回归测试 12 组 171 项断言全部通过（新增 L 组 20 项：空闲
      驱动/无接触/单键导通/同列合并/跨行/驱动回读/F340 别名读清写清/
      legacy 展开与矩阵导通/外电 bit12/总线 word 路由 DS1 段）
- [x] **NAND 控制器 F300**（np560_soc/np560_nand.c，移植 hw/dmcu/
      np560.c:8114-8931）：寄存器 F300..F321 word 访问、READ_PAGE/
      PROGRAM_PAGE/ERASE_BLOCK/READ_ID/COPY_BACK、AND 编程语义（1→0）、
      OOB 跨页流（4 页上限）、DMA 字节槽流（length==1 时列/缓冲 ^1 车道
      修正）、器件 ID 0xDCEC/几何 0x95、错误码 PAGE/RANGE/COMMAND/BACKING。
      介质抽象 Np560NandMedium（read/program/erase 页级回调）：host 用
      内存介质；ESP32 上接 microSD 分页介质（SD 加载批次实现）。QEMU 的
      journal/trace 诊断设施不移植（宿主侧，非 guest ABI）
- [x] **硬件 ECC 引擎 F20A/F20C/F20E**（np560_soc/np560_ecc.c，移植
      np560.c:7630-7712）：配对奇偶 SEC-DED（置位数据位贡献 position=
      (word<<4)|bit 给 Parity、互补给 NParity）、12 位字计数、写 F20A
      清零；banked F20A 与 IGNDS unbanked 页 0x01000A0A 双视图（总线
      phys_read16/write16 已支持 unbanked 页换算路由）
- [x] **word MMIO 路由拆分**（np560_periph）：F100→SOCR SFR 视图、
      F160/F340→键盘、F20A/C/E→ECC、F300..F321→NAND、其余读 0/写丢弃；
      dmcu_bus 新增 dmcu_phys_slot_read8/write8（NAND DMA 字节槽流，
      不做 ^1 车道交换），修复 unbanked 页地址被 &MASK24 裁剪的越界隐患
- [x] host 回归测试 13 组 214 项断言全部通过（新增 M 组 43 项：复位态/
      READ_ID+几何/页读 DMA 出/字节车道 A0/页编程/AND 语义/OOB 编程读回/
      块擦除/三类错误码/COPY_BACK/ECC 喂字配对奇偶/清零/双视图/SOCR
      视图读写/DS1 翻译路由）
- [x] **NAND stdio 文件介质**（np560_soc/np560_nand_file.c）：整片 raw
      镜像（data+OOB 2112B/页交错，528MiB，QEMU system.nand checkpoint
      同格式）的 read/program(AND)/erase 页级实现，host 与 ESP32 共用；
      erase_block 参数为块号（0..4095）
- [x] **code 平面分离**（dmcu_bus）：QEMU code/data 是两个独立地址空间，
      新增 bus->code_rom（resident.bin 2MB）与 code 平面取指语义
      （vma03：X RAM alias 0..0x7FFF / PRG 0x30000..0x42FFFF / 其余 ROM
      phys 优先回落 / ≥0x430000 missing 0x0f00）；code_rom==NULL 时回落
      旧单平面行为（回归兼容）
- [x] **SD 加载层**（main/np560_sd.cpp，ESP32）：bsp_sdcard_init("/sd")、
      NP560.PRG→data 平面 0x30000（vma03）、resident.bin→code ROM、
      data 平面 ROM 镜像 0x10000..0x2FFFF（data_soc_rom_alias）、
      system.nand.gz 首次流式解压→system.nand（esp_rom miniz tinfl，
      gzip 头解析+断点续传检测）、NAND 文件介质挂接；TF 卡路径
      /sd/noahnp560/（用户约定）
- [x] **真实固件启动**（np560_emu.cpp）：QEMU 复位语义（SP=0xfffe、
      DS1=1、resident ROM 窗口 [0,2MB)、reset PC=0x30000 firmware-vma03，
      对齐 portable 启动配置 boot-mode=firmware-vma03 --soc-rom
      resident.bin --soc-rom-skeleton）；SD 无固件时降级 smoke test
- [x] host 回归测试 14 组 237 项断言全部通过（新增 N 组 23 项：文件介质
      前置擦除/全页编程/读回/AND 语义/擦除恢复/code 平面 X RAM 别名/
      ROM/overlay 优先/PRG 区间含 0x200000/PRG 后 missing）

## 待移植清单（按 helper.c 指令族，行号为 qemu-dmcu/target/dmcu/helper.c）

| 单元 | 位置 | 规模 | 说明 |
|---|---|---|---|
| 音频 HLE | hw/dmcu/np560_audio.c 剩余 + minimp3 | 大 | MP3 解码链路（核 1）、EQ；portable 配置 --rom-mp3-hle-base 138790 --rom-mp3-arena-image 93952 --rom-npe-codec-owned |
| DTUC 别名 | regs.c | 小 | REG[21..24] 与内存 F142..F148 共享状态（dtu.md） |
| TCA8418 键盘 | components/keypad_scanner_tca8418 | 小 | 官方 Ext Keyboard 底座路径（真机按需接入） |
| overlay alias 精确化 | np560.c 22560-22700 | 中 | vma03 overlay alias 现用 phys 优先近似（phys 0 回落 ROM），真机验证后按需精确 |

每完成一族：在 host/runner.c 加对应断言（可构造针对性指令序列，或用
`doc/disassembly/` 的真实固件片段做黄金对照——QEMU 侧用 QMP 导出寄存器状态）。

## host 快速回归

```bash
cd $REPO_ROOT/host && make && ./dmcu_runner
```

## ESP-IDF 构建

```bash
. ~/ESPIDF/esp-idf-v5.4.2/export.sh
cd $REPO_ROOT/platforms/tab5
idf.py set-target esp32p4   # 已配置过可跳过
idf.py build flash monitor
```

固件放 TF 卡 `/noahnp560/`（用户约定，不放根目录）：`NP560.PRG`（4MB →
data 平面 phys 0x30000）、`resident.bin`（2MB → SoC ROM 0x0）、
`system.nand.gz`（66MB → 首次启动流式解压为 `system.nand` 528MiB，
卡建议 FAT32）。

## 真机验证记录（节点②，2026-12-23）

- **IDF 版本**：从 5.5.3 迁移到 **5.4.2**（官方 M5Tab5-UserDemo 要求 5.4.1
  版本线）。5.5.3 默认芯片版本 REV_MIN_301 与实际芯片 rev v1.0 不兼容。
- **sdkconfig.defaults 关键项**（重新生成 sdkconfig 时易丢失，逐项核对）：
  - `ESP32P4_SELECTS_REV_LESS_V3=y` + `ESP32P4_REV_MIN_100=y`（芯片 rev v1.0）
  - `ESP_CONSOLE_USB_SERIAL_JTAG=y` + `ESP_CONSOLE_SECONDARY_NONE=y`
  - `FATFS_LFN_HEAP=y` + `FATFS_MAX_LFN=255`（`noahnp560`/`system.nand.gz`
    等长文件名必需；无 LFN 时 stat 全部失败表现为 "missing"）
  - `CODEC_I2C_BACKWARD_COMPATIBLE` 必须为 not set（esp_codec_dev Kconfig
    default y；设 y 会走 legacy I2C API 而 BSP 总线由新驱动安装，ES8388
    初始化全部写失败 → assert）
  - `I2C_SKIP_LEGACY_CONFLICT_CHECK=y`（自研键盘组件与 esp_codec_dev 共存）
- **静态大缓冲必须 PSRAM 堆分配**（heap_caps_malloc MALLOC_CAP_SPIRAM）：
  内部 SRAM bss 容量有限，s_canvas_buf(225KB)+s_frame(56KB) 放 bss 曾把
  总 bss 推到 458KB 超限。
- **HAL 任务栈**：官方 hal_rs485.cpp 的 rs485 任务栈 2000 太小，任务内触发的
  mooncake_log(fmt v11) 格式化会栈溢出（Stack protection fault），已提到 4096。
- **tinfl 流式解压契约**（np560_sd.cpp ensure_nand_image，两次大坑）：
  1. 输出必须用 32KB 环形字典（wrapping 模式，对齐
     tinfl_decompress_mem_to_callback）。NON_WRAPPING 模式下 tinfl 直接从
     输出缓冲读 LZ 回引历史，换缓冲后 dist 校验立即 FAILED。
  2. `tinfl_decompress` 退出时 `*pIn_buf_size` = **已消费**字节数
     （mz_inflate 源码 `next_in += in_bytes` 为证）。必须 `in_rem -= consumed`
     再 memmove 未消费尾部。曾误当"未消费"，把已消费数据重喂导致流失步。
  - NAND raw 镜像尾部实测 553648192 = 528MiB + 64 字节，容差检查用
    `expect + NP560_NAND_PAGE_OOB`。
  - 上次解压中断残留的不完整 system.nand 现自动删除重解压（gz 为权威源）。
  - 真机解压速率 ~0.27MiB/s（SD 写入为主），528MiB 约 30 分钟，日志每
    32MiB 打一次进度。
- **验证状态**：ES8388 codec init OK、键盘 OK、SD OK、smoke test OK、
  NAND 解压进行中（后续：固件装载 → CPU 运行观察）。

## 固件装载与 CPU 运行（节点②续，2026-11-23 晚）

NAND 镜像改为**主机预解压**（设备端 tinfl 流式解压 ~0.27MiB/s 太慢，
`gzip -dc system.nand.gz > system.nand` 后放 TF 卡 /noahnp560/，553648192
字节，设备端 `ensure_nand_image()` 大小检查通过直接跳过解压）。

首启暴露两个 bug，均已修复：

1. **`code_rom alloc failed`**：`np560_bus_load_code_rom` 在 read_file 已
   持有 4MB PRG + 2MB ROM 临时缓冲之外再 malloc 2MB 拷贝。实测此时 PSRAM
   仅剩 `free=6361712 largest=6291456`（bus phys 平面 16MB + GUI 占用后），
   2MB 连续分配失败。修复：新增 `np560_bus_take_code_rom()`（dmcu_bus.c/h）
   零拷贝所有权转移，rom 缓冲直接移交 bus，`load_firmware` 内置 NULL 防
   双重释放。copy 版保留供 host runner 使用。
2. **复位 PC 未更新**：`cpu_reset()` 在固件装载前调用（bus->code_rom 尚
   为 NULL），PC 兜底 0x200000；装载成功后需 PC=0x30000。不能重复整个
   cpu_reset（memset 会清掉 periph_init 挂上的 SFR 设备回调），改为装载
   成功后仅重设 `s_cpu.pc = 0x30000` 与 `resident_rom_limit`。

附带发现：固件装载路径下 smoke test 的 `R0=0` 不是回归——PRG 装载写入
`0x30000..0x430000` 覆盖了 smoke program 所在的 `0x200000`（smoke 仅在
无固件降级路径运行，固件成功时不执行）。

**最终验证（NP560DIAG 诊断代码已移除后烧录）**：

```
I (2359) np560_sd: psram free=6361968 largest=6291456 before firmware load
I (2361) np560_sd: NAND image ready (553648192 bytes)
I (6372) np560_sd: firmware loaded: PRG 4096KB @0x30000, ROM 2046KB, reset PC=0x30000
I (6374) np560_sd: NAND medium attached (/sd/noahnp560/system.nand)
I (6374) np560: reset PC=0x30000, resident ROM window 2046KB
I (6387) np560: emulator task start, clock=24576000 Hz
```

CPU 从 0x30000 持续运行（24.576MHz），无 halt、无 fault、无 unsupported
指令风暴（60s+ 观察）。ROM 实测 2046KB（resident.bin 文件 2,095,672 字节，
QEMU 侧重建产物，不足整 2MB；代码平面窗口按实际大小建立）。
节点②固件装载链路打通；下一步：LCD 画面确认 → 键盘联调 → 音频 HLE。

## LCD 画面确认与"升级模式"根因（节点②续，2026-11-23 深夜）

**LCD 渲染链路真实打通**：Tab5 屏幕中央黑色区域显示原厂固件输出
（"系统初始化，请等待... / 完成FAT:mount程式"）——ST7529 GRAM → RGB565 →
LVGL canvas 2x 渲染、DMCU 解释器执行原厂固件、LCD 命令口 MMIO 全部工作。

### bug3：虚拟时钟冻结（定时器中断永不触发）

诊断手段：emulator task 每 5s 打印 PC/cyc/s/irq_pending + SFR/MMIO 读写
热点采样（np560_periph_diag_take）。症状：cyc/s=0（cycles 零推进）、
irq_pending=0、SFR 热点全空——`dmcu_exec_step` 不推进 cycles，而
`np560_periph_advance` 用 cycles 差值推进外设，`dmcu_clock_charge` 无人调用
→ 虚拟时钟冻结 → 定时器/TBC 中断永不触发。

修复：emulator task 按主机真实时间推进虚拟周期（`esp_timer_get_time` 差值
× system_clock_hz），对齐 QEMU ptimer 非 icount 语义。修复后 cyc/s≈24.58M。
（注：QEMU 权威配置为 `--instruction-clock --icount-shift 0 --realtime`，
即 `-icount shift=0,align=on,sleep=on` 指令时钟；wall-clock 推进是过渡
方案，解释器速率不足时定时器相对指令流加速，行为差异待后续对齐。）

### 根因：干净 NAND 缺运行时数据 → 固件进升级模式

QEMU 原版对照（Linux build + Web 前端 `/api/display` 抓帧）：
- 用户日常 device（checkpoint + device.npj journal 7.5MB 重放）→ **正常
  桌面**（<<英汉词典>> 输入界面）
- 干净 checkpoint（无 journal）→ **复现同样卡死**（空白初始化框）

结论：release/system.nand.gz 是出厂 seed，固件启动需要 NAND 里的运行期
数据（journal 历史写入）；缺失时固件走升级模式分支死等主机连接。

**解决方案**：手动 QEMU（独占 QMP，`-S` 暂停 CPU）对 journal 重放后的
NAND 执行 `qom-set nand-save` → 导出 `release/system.nand`
（553,648,192 字节，data+OOB）。设备端 `np560_nand_file.c` 以 `r+b` 打开，
支持固件运行期写持久化。将该文件放 TF 卡 `/noahnp560/system.nand`
（无需 gz，大小检查直接通过）。

排障工具沉淀：QEMU QMP 只允许一个客户端（Web 前端占用时第二个连接
recv 报 EINVAL）；`/api/display` 返回 gray5 base64 帧可离线解码比对；
`--nand-save-on-exit` 与 `--device` 互斥。

## 移植纪律（沿袭主仓库 plan.md）

1. 语义以 helper.c/hw 为唯一权威；不按"看起来合理"补行为。
2. 未知成本/未实现模式显式标注 TODO，不用猜测值掩盖。
3. QEMU 侧诊断观察器（menu watch、NPE snapshot、trace ring）**不移植**——
   它们是调试工具不是架构状态。
4. 每族指令移植后必须过 host 断言再上真机。

## 性能预估

NP560+ CPU 基线 24.576MHz；ESP32-P4 双核 400MHz RISC-V。解释器目标
≥30M instr/s（实测待阶段一真机基准）。音频 minimp3 HLE 可放第二核。
