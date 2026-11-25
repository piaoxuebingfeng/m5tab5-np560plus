# Tab5 分支性能对比与后续验证指南

| 项目 | 内容 |
|---|---|
| 文档日期 | 2026-11-25 |
| 对比对象 | `main`（无 fastboot） vs `backup/fastboot-cd8b335`（有 fastboot） |
| 基点 | `d5e935d`（fastboot 前最后一个提交） |
| 对比基准 HEAD（2026-11-25 11:10） | `main` = `302b943`；`backup/fastboot-cd8b335` = `86f5f6d` |
| 一句话结论 | fastboot 分支启动快约 30 秒，但指令执行性能退化约 **25 倍**（131 万 → 5.27 万 IPS），真机交互实际不可用；根因未定位，当前证据指向编译/链接生成代码层面，而不是键盘、时钟、渲染等运行时逻辑。 |

> 两个分支现在都含 ESC 修复（`302b943` / `86f5f6d`），因此本文的差异**不包含** ESC 按键问题。

---

## 1. 分支定义与差异总览

### 1.1 分支状态

| 分支 | HEAD | 独有内容 |
|---|---|---|
| `main` | `302b943` | ESC 修复（对齐 QEMU `6fcc208992`），其余与 `d5e935d` 相同 |
| `backup/fastboot-cd8b335` | `86f5f6d` | 6 个 fastboot/键盘提交 + ESC 修复：`b193f72` → `26e8d36` → `b43095a` → `6a1ff21` → `61ca234` → `cd8b335` → `86f5f6d` |
| `perf/icache-experiment` | `9d1aad5` | 软件 I-cache 实验（本次不参与对比） |

### 1.2 代码差异（`git diff --stat main backup/fastboot-cd8b335`）

共 15 个文件，+1594 / -24 行：

| 类别 | 文件 | 变化 | 与性能的关联 |
|---|---|---|---|
| 快照核心 | `np560_fastboot.c` / `.h` | +950 / +50（新增） | 静态大结构、CRC、身份校验；不参与指令解释 |
| 主循环时钟 | `platforms/tab5/main/np560_emu.cpp` | +190 | **核心嫌疑**：引导期 icount / 桌面期墙钟双策略、逐 tick 交付、checkpoint 调用 |
| 定时器实时路径 | `np560_timers.c` / `.h` | +39 / +16 | `pending_ticks`、`np560_timers_advance_realtime()`、`np560_timers_deliver_timebase()` |
| 外设分发 | `np560_periph.c` | +6 | `np560_periph_advance_realtime()` |
| NAND 落盘 | `np560_nand_file.c` | +38 | `np560_nand_file_medium_flush_dirty()`（保存快照前刷 dirty 槽） |
| 构建配置 | `sdkconfig` / `sdkconfig.defaults` | +4 / +2 | app_main 栈 3584 → 8192（快照结构太大导致栈溢出） |
| 组件注册 | `np560_soc/CMakeLists.txt` | +1 | 加入 `np560_fastboot.c` |
| 宿主回归 | `host/runner.c` | +192 | P 组：round-trip / CRC / 身份 / 实时 tick 断言 |
| 工具 | `tools/wait_fastboot.py` | +92（新增） | 快启里程碑抓日志 |
| 文档 | `HANDOVER.md` | +29 / -... | 记录快启用法 |

**关键事实**：`platforms/tab5/components/dmcu_core/` 整个目录在两分支间**零差异**——解释器源码逐字节相同。所以 25 倍性能差异不可能来自指令语义实现本身，只能来自：
1. 主循环运行时行为（时钟推进、中断交付频率）；
2. 编译/链接结果（工具链、优化选项、段布局、栈/堆使用）；
3. fastboot 引入的静态内存（16.9 MB 快照缓冲、身份结构）对内存布局/缓存的影响。

### 1.3 主循环机制差异（最重要）

`main`（icount 恒定）：
```c
delta = loop_insns;              /* 1 条指令 = 1 个 CPU cycle */
dmcu_clock_charge(&s_cpu, delta);
np560_periph_advance(&s_periph, delta);
```

`backup/fastboot`（双策略）：
```c
bool desktop = s_fastboot_loaded || s_saw_idle_pc;
if (desktop && s_last_clock_us && now_us > s_last_clock_us) {
    wall_us = MIN(now_us - s_last_clock_us, 200000u);   /* 单轮上限 200ms */
    delta = wall_us * dmcu_system_clock_hz(&s_cpu) / 1000000ULL;
}
if (desktop)
    np560_periph_advance_realtime(&s_periph, delta);   /* tick 先入 pending_ticks */
else
    np560_periph_advance(&s_periph, delta);
```
内层每指令边界：
```c
if (key_wake_ticks-- > 0) { ... } 
else if (!(ICR0.RQ) && np560_timers_deliver_timebase(&s_periph.timers)) { /* 逐个交付 tick */ }
dmcu_exec_step(...);
```
`pending_ticks` 是纯运行时字段，不进快启快照；RQ 是锁存位，批量 raise 会合并成一次 ISR，所以必须逐条交付才能复现 QEMU ptimer 语义。

---

## 2. 实测性能差异（全部来自真机日志）

### 2.1 汇总表

| 指标 | `main`（d5e935d + ESC） | `backup/fastboot`（cd8b335 + ESC） | 差异 |
|---|---|---|---|
| 启动到桌面 | **70.2 s**（`emulator task start` @17.025s 后 +70.2s，首次 idle PC=00:1018） | **40.5 s**（快照恢复，`fb=1/1/1`，frames=42，NAND 零访问） | 快约 30 s |
| 启动期 IPS | 56.2 万 ~ 76.6 万 | 未单独测（完整初始化时更慢） | — |
| 桌面期 IPS | **130.8 万 ~ 131.0 万**（均值 97.9 万） | **约 5.27 万** | **约 25 倍退化** |
| 每条指令 CPU 周期 | 未直接测（约 500 周期/条量级） | **6822.5 周期/条**（见 2.2） | 约 13.6 倍 |
| CPU 占用 | — | 360.00 MHz 精确跑满（无抢占） | 排除调度问题 |
| 时基 exp | 每 5 s +17~18（约 3.5 Hz，icount 语义） | 墙钟 41~49 Hz（修复后） | 中断频率大幅不同 |
| 键盘扫描 | `mr` 每 5 s +4~5，稳定增长 | `mr` 增长但交互延迟 10~20 s（用户实测） | 功能不可用 |
| 按键→guest 识别 | — | 修复后 76~502 ms（日志），用户主观仍慢 | 体验不一致 |
| 桌面刷新 | 正常 | 约 3 秒一帧（用户实测） | — |

### 2.2 决定性测量：cycle counter

在 `backup` 分支真机插入 `esp_cpu_get_cycle_count()` 插桩（代码已回退）：

```
682252218 cycles / 1895144 us = 360.00 MHz busy
6822.5 cycles / insn
```

推论：
- CPU 100% 满载、无调度器抢占 → 排除“被渲染/锁/其他任务抢占”的假设；
- 每条 NOP 真花 6822 个 CPU 周期（19 μs），同源代码在 host x86 `-O2` 是 1.7 ns/条，差约 4000 倍 → **纯代码生成差异**。

### 2.3 分段计时与三层基准（`backup` 分支，均已回退）

| 实验 | 结果 | 结论 |
|---|---|---|
| 主循环 5 s 分段 | `exec=4904ms`、`rend=140ms/24帧`、`lock=0ms` | 98% 时间在指令循环，渲染/锁不是瓶颈 |
| phys[] 顺序读 | 945k/s | 内存读本身够快 |
| `dmcu_code_read16` | 238k/s | 取指函数不是唯一瓶颈 |
| `dmcu_exec_step` 纯 NOP | 52766 ips（19 μs/条） | 解释器单条极慢 |
| SRAM vs PSRAM 取指 | 52591 vs 52765 ips | 取指内存位置无关 |
| `IRAM_ATTR` 搬 20 个热点函数 | bench 无变化 | 不是执行位置问题 |
| host x86 同源码 `-O2` | 5874 万 ips | 差异来自 RISC-V 工具链生成代码 |
| `dmcu_exec_step` 编译体积 | 0x2402 = 9222 字节巨型函数 | 怀疑长 if-else 分发链导致寄存器溢出/巨型栈帧 |

### 2.4 QEMU oracle 对照

| 场景 | 耗时 |
|---|---|
| QEMU 普通 reset → 桌面 | 1.99 s |
| QEMU 快照恢复 → 桌面 | 0.056~0.12 s（约 16~35 倍） |
| QEMU 快照恢复后键盘 | 曾失效，根因是 ptimer 运行时状态没恢复；`ed722f6ba1` 重放定时器寄存器后修复 |

QEMU 用墙钟 ptimer 且逐 tick 交付，所以 QEMU 不能复现 Tab5 的 IPS 退化——它本身就是实时的。

---

## 3. 已排除的假设（避免重复劳动）

以下方向都做过真机或 host 实验并被否定，**不要再查**：

1. **键盘链路**：M04/M28 映射已修（ESC 修复）；guest `ma/kwirq/fast` 都能增长，按键被真实扫描受理。
2. **墙钟时基不推进**：`exp` 已能从 5 s +1 提到 +204~244；但只看 `exp` 是假阳性，还要看 guest 实际受理 ISR。
3. **tick 批量合并**：已改逐 tick 交付（`61ca234`），`irq_pending` 从持续 1 变全 0。
4. **渲染/LVGL**：5 s 内仅 140 ms（3%）。
5. **锁/调度抢占**：`lock=0ms`，cycle counter 显示 CPU 360 MHz 满载。
6. **PSRAM/SRAM/IRAM 执行位置**：三组对照无差异。
7. **CPU 降频/电源管理**：实测 360 MHz，PM 未启用。
8. **`dmcu_core` 源码差异**：两分支逐字节相同。

---

## 4. 当前根因假设（按优先级）

| 优先级 | 假设 | 依据 | 如何证伪/证实 |
|---|---|---|---|
| P0 | 编译器对 `dmcu_exec_step` 的代码生成退化（长分发链 + `-O2` + 断言 level2 → 巨型栈帧/寄存器溢出） | 9222 字节巨型函数；6822 cycles/条；host 同源码 1.7 ns/条 | 对比两版 `dmcu_exec.c.obj` 反汇编与栈帧大小；试 `-Os`/`-O3`/`-fno-...` |
| P1 | 链接布局变化（fastboot 静态大数组挤占段，把热点函数/常量挪到慢速区域） | 快照 16.9 MB、身份结构、静态大数组；`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | 对比两版 ELF 的 `dmcu_exec_step` 地址与段属性；把热点数据搬回 SRAM/IRAM（已试无效，需结合地址看） |
| P2 | 墙钟模式下中断/交付开销被计入 `exec_step` 测量 | `backup` 桌面期每轮会产生多 tick 并逐条交付；旧 bench 在桌面期跑 | 关掉墙钟/关掉 tick 交付后重测 cycle |
| P3 | fastboot 恢复的运行时状态（SFR/RAM/IRQ）导致 guest 走了不同的慢路径 | 恢复后 `ICR0=0230` 与引导期不同 | 在 `main` 分支人工注入相同 SFR/内存状态后测 IPS |

---

## 5. 后续具体验证方法

### 5.1 总体纪律

1. **控制变量**：每轮只改一个因素，改完必跑 host 回归（`host/dmcu_runner` 全部通过）。
2. **先 host / QEMU，后真机**：语义问题在 QEMU oracle 秒级复现；真机只做最终验证。
3. **烧录前主动告知用户**（用户明确要求）；每次修改按功能提交 git。
4. **长任务用 `systemd-run --user --unit=<名> --collect --wait`**，日志在 unit 内重定向到 `/tmp/<名>.log`；调试结束必须 `systemctl --user stop` 并确认无残留。
5. **串口抓取必须用 `/usr/bin/python3`**（managed Python 无 pyserial）。
6. 旧版运行会写 `system.nand`，切回 fastboot 分支时快照身份可能失效，需要重建（可能十几分钟）。

### 5.2 标准 A/B 流程

```bash
REPO=/home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560
IDF=/home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh

# 1. 切分支（确认工作区干净）
cd $REPO && git checkout main            # 或 backup/fastboot-cd8b335
git status --short --branch

# 2. 编译（systemd-run 绕开沙箱；日志写在 unit 内）
systemd-run --user --unit=np560-ab-build --collect --wait /bin/bash -c \
  'source /home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh && \
   cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/platforms/tab5 && \
   idf.py build > /tmp/np560-ab-build.log 2>&1; echo BUILD_EXIT=$? >> /tmp/np560-ab-build.log'

# 3. 烧录（先告知用户！）
systemd-run --user --unit=np560-ab-flash --collect --wait /bin/bash -c \
  'source /home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh && \
   cd /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/platforms/tab5 && \
   idf.py -p /dev/ttyACM0 flash > /tmp/np560-ab-flash.log 2>&1; echo FLASH_EXIT=$? >> /tmp/np560-ab-flash.log'
```

### 5.3 启动耗时测量

抓日志（旧版需 ≥90 s，fastboot 版 ≥60 s；重启抓完整启动）：

```bash
systemd-run --user --unit=np560-ab-capture --collect --wait /bin/bash -c \
  '/usr/bin/python3 /home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/tools/serial_capture.py 150 > /tmp/np560-ab-boot.log 2>&1'
```

关键时间戳（ESP-IDF 日志自带毫秒）：
- `np560: emulator task start, clock=24576000 Hz` → T0
- `np560: run: PC=00:1018 ...` 首次出现 → 进桌面，耗时 = T1 − T0
- fastboot 分支另有里程碑：`fastboot: resumed desktop directly`、`fb=1/1/1`、`restored timers: TB=1/64Hz TBC=8009 ICR0=...`
- 也可直接用 `tools/wait_fastboot.py` 按里程碑抓（仅 fastboot 分支）。

### 5.4 IPS 与 cycle 测量

**IPS**：直接读 5 s 聚合日志行：
```
I (22034) np560: run: PC=02:6294 ips=766000 total=3830000 ...
```
取桌面期稳定后的 6~10 行求均值，记下 PC 段（确保都在 idle 附近，非引导扫描）。

**cycle/指令**（关键实验，代码需临时插桩后回退）：
- 位置：`np560_emu.cpp` burst 边界，取 `esp_cpu_get_cycle_count()` 差值 `Δcycles` 与 `esp_timer_get_time()` 差值 `Δus`；
- 计算：`busy_mhz = Δcycles / Δus`，`cycles_per_insn = Δcycles / burst_insns`；
- 判据：`busy_mhz ≈ 360` 表示 CPU 满载无抢占；`cycles_per_insn` 直接反映代码生成效率。

**对照实验（最优先做）**：
1. 在 `backup` 分支把桌面期墙钟改回 icount（只改 `np560_emu.cpp`，不动 `dmcu_core`），烧录测 IPS/cycle → 若恢复到 100 万量级，说明根因是主循环运行时行为（时钟/中断频率）；若仍 5 万，说明是编译/链接。
2. 在 `main` 分支把 icount 改成墙钟+逐 tick 交付，测 IPS/cycle → 若掉到 5 万，与上条互证。
3. 两版都关掉 tick 交付（`np560_timers_deliver_timebase`）再测 → 排除中断开销计入 bench 的干扰（P2）。

### 5.5 代码生成对比（不改功能，只读）

在两分支分别编译后（产物不要互相覆盖，编译前把 `build/m5stack_tab5.elf` 复制到 `/tmp/<分支>.elf`）：

```bash
source $IDF
# 反汇编目标函数
riscv32-esp-elf-objdump -d --disassemble=dmcu_exec_step /tmp/main.elf  > /tmp/dis-main.txt
riscv32-esp-elf-objdump -d --disassemble=dmcu_exec_step /tmp/fastboot.elf > /tmp/dis-fb.txt
diff -u /tmp/dis-main.txt /tmp/dis-fb.txt | less

# 对比目标文件大小与栈帧指令
ls -l build/esp-idf/dmcu_core/__idf_dmcu_core.dir/dmcu_exec.c.obj
grep -n 'addi.*sp,sp\|sw.*ra\|lw.*ra' /tmp/dis-*.txt | head -40
```

关注点：
- 两版 `dmcu_exec_step` 的字节数、栈帧大小、`s0-s11` 保存/恢复次数；
- 是否有大量 `lw/sw` 到栈（寄存器溢出）；
- 函数入口地址与所在段（DRAM/IRAM/PSRAM），对照 `esptool.py elf2image` 或 `readelf -S`。

### 5.6 键盘延迟测量

抓日志时请用户在桌面/词典页按 ESC 和方向键，核对：
```
I (...) np560_kb: KB key_id=0 row=0 col=0 pressed=1 -> matrix=4 mask=0000000000000010
I (...) np560: keypad: mr=... ma=... kwirq=... fast=...
```
- `key_id=0 -> matrix=4` 证明 ESC → M04 正确；
- `ma`、`kwirq`、`fast` 增长证明 guest 真受理；
- 按键时刻到 `ma` 增长的时刻差为端到端延迟（目标 <500 ms）。

### 5.7 快照身份与 TF 卡注意事项

- 快启身份 = `state_version` + 结构尺寸 + `NP560.PRG`/`resident.bin`/`system.nand` 的 size+mtime+头尾 CRC；`build_tag` 已被 `cd8b335` 排除，改宿主代码不必重建。
- `/sd/noahnp560/fastboot.force` 存在时强制重建快照。
- 文件必须放在 `/sd/noahnp560/`，不能放 TF 卡根目录。
- `system.nand` 是 553,648,192 B 的 journal 重放后镜像；seed `.gz` 只能进升级模式。
- `fastboot.snap` 约 16.9 MB；CRC 不匹配或身份不符时自动走完整初始化。

### 5.8 判定矩阵

| 观察结果 | 支持的假设 | 下一步 |
|---|---|---|
| backup 改回 icount 后 IPS 恢复 100 万 | P0/P2：墙钟/中断频率相关 | 在墙钟下找具体开销点（中断交付、delta 计算） |
| backup 改回 icount 后仍 5 万 | P1：编译/链接 | 做 5.5 反汇编对比，试编译选项 |
| main 改墙钟后 IPS 掉到 5 万 | 与上面互证主循环是触发条件 | 同上 |
| 关掉 tick 交付后 cycle 恢复 | P2：中断开销计入 bench | 优化交付路径（批量但保证 ISR 次数） |
| 两版 `dmcu_exec.c.obj` 汇编/栈帧不同 | P0：编译器代码生成 | 对比编译命令、断言等级、LTO，试 `-Os` |
| 汇编相同但地址/段不同 | P1：链接布局 | 调整段放置/内存布局 |

---

## 6. 环境与命令速查

| 项目 | 值 |
|---|---|
| Tab5 工程 | `/home/qiao/00-opencode-ai/m5tab5-bbk/m5tab5-np560/`（ESP-IDF 工程在 `platforms/tab5/`） |
| ESP-IDF | **v5.4.2**，`/home/qiao/ESPIDF/esp-idf-v5.4.2/export.sh`（必须 bash source；5.5.3 不兼容） |
| 设备 | `/dev/ttyACM0`（USB 直连） |
| 串口抓取 | `/usr/bin/python3 tools/serial_capture.py <秒数> [--no-reset]` |
| 快启抓日志 | `tools/wait_fastboot.py` |
| host 回归 | `cd host && make dmcu_runner && ./dmcu_runner` |
| QEMU oracle | `/media/qiao/.../np560-plus-emulator/`，Web 桥 `scripts/web/np560_web.py` |
| 沙箱绕过 | `systemd-run --user --unit=<名> --collect --wait`，长任务后台执行，结束清理 |

---

## 7. 风险与约束

1. **不要 ninja**：用户明确要求，统一用 `idf.py build`。
2. **不要反复盲改键盘/时钟/渲染**：第 3 节已列出排除项，按第 5.4/5.5 的实验顺序推进。
3. **每次改完必须按功能提交 git**；提交信息写清功能与根因。
4. **真机验证节点必须主动告知用户**，烧录前说明版本和风险。
5. **调试进程必须清理**：`systemctl --user stop <单元>` 并确认 `inactive`。
6. 当前设备固件是 `main` 的 `302b943`（无 fastboot，启动 70 s、IPS 131 万）；切到 fastboot 分支验证前要告知用户启动/快照状态会变化。
