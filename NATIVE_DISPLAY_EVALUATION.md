# 去除 LVGL、改用原生显示 API 的可行性评估

日期：2026-11-25
范围：`platforms/tab5`（Tab5 模拟 NP560 固件）
性质：**架构评估**，本轮不改任何功能代码、不编译、不烧录。

## 1. 结论摘要

**技术上可行，且有真实的架构性收益，但这是一次"显示后端替换"而非小优化，风险高于 P1。建议开独立 feature 分支迭代，main 保持不动。**

- LVGL 在本工程里**只承担一件事**：把一块 1200×600 canvas 贴到屏幕。没有任何按钮、标签、布局、事件回调等业务 UI。
- 当前链路为"canvas → LVGL image blend → PPA 旋转 → 局部 draw_bitmap"，LVGL 是纯粹的中间层，引入了一次全屏搬移、一块整屏旋转缓冲、一个 LVGL 任务，且无 vsync 导致撕裂。
- 原生路径由 BSP 的 `bsp_display_new()` 直接提供 `esp_lcd_panel_handle_t`，可绕过整个 LVGL port，用 DPI 双 framebuffer + vsync 提交。
- **收益**：省掉 LVGL 的全屏 blend 与旋转缓冲（约 3 MB PSRAM）、去掉 LVGL 任务抢占解释器、从根上用硬件双缓冲消除撕裂。预期渲染链路开销下降，但**不是数量级提升**（解释器 IPS 已正常，主要收益在显示流畅度与撕裂根治）。
- **风险**：显示方向（90° 预转置）、触控 indev 无消费者、HAL 初始化路径需改、BSP `num_fbs` 从 1 改为 2。这些必须在分支里逐项静态论证后再动真机。

## 2. LVGL 实际职责（代码证据）

| 位置 | 用途 | 是否进入 NP560 运行链 |
|---|---|---|
| `np560_emu.cpp:404-408` | `lv_canvas_create` / `lv_canvas_set_buffer` / `lv_obj_center` | ✅ 唯一显示出口 |
| `np560_emu.cpp:106` | `lv_obj_invalidate(s_canvas)` | ✅ 触发重绘 |
| `np560_emu.cpp:178-180` | `bsp_display_lock/unlock`（实质是 `lvgl_port_lock` 互斥锁） | ✅ 保护 render |
| `hal_esp32.cpp:103-112` | `bsp_display_start_with_config` + 90° 旋转 + touch indev | ✅ `hal->init()` 必经 |
| `hal_esp32.cpp:21-45` | `lvgl_read_cb` 上报触控坐标 | ⚠️ indev 无任何 UI 控件消费（触控皮肤 TODO） |
| `hal_usb.cpp:261` | USB 鼠标 indev `lv_indev_set_display` | ❌ np560 主程序未调用 |
| `hal_power.cpp:99-132` | `lvglLock/unlock` 用于触摸唤醒 | ❌ np560 主程序未调用 |
| `app_main.cpp` | `hal->init()` 一次性初始化 | ✅ 入口 |

**判定：业务层对 LVGL 的依赖仅在"一块 canvas + 一把锁"。触控/鼠标/电源的 LVGL 依赖都在 HAL 通用代码里，np560 主程序并未走这些路径。**

## 3. 当前链路开销分析

每帧画面变化时的实际数据通路：

1. `np560_lcd_render()`：240×120 灰度 → RGB565（实测固定开销 ≈5.7 ms）
2. `memcmp`：28 KB 帧 diff
3. 5x 放大：1200×600 = 1.4 MB 写 PSRAM（SRAM 行展开 + 5 行 memcpy）
4. `lv_obj_invalidate()` → LVGL 把 canvas 当 image 对象调度重绘
5. LVGL task（priority 4，33 ms 周期）：image blend 到 draw buffer（整屏 1280×720 = 1.8 MB）
6. flush callback：手写 `rotate_copy_pixel()` 用 PPA 把脏区转置到 `draw_buffs[2]`
7. `esp_lcd_panel_draw_bitmap()` 局部送屏（无 vsync → 撕裂）

LVGL 贡献的"有用功"只有第 5、6 步的搬移与旋转，但代价是：

- **PSRAM**：draw buffer 921600×2×2 B = 3.6 MB（double）+ 旋转缓冲 1.8 MB ≈ **5.4 MB**
- **任务**：LVGL task 7168 字节栈、priority 4，与解释器 task 抢占
- **全屏 blend**：canvas → draw buffer 多一次 1.4~1.8 MB PSRAM 读写
- **撕裂**：PARTIAL + 局部 draw_bitmap 无 vsync（见 `DISPLAY_FLUENCY_OPTIMIZATION.md` 三条不变量）

## 4. 原生方案设计

### 4.1 显示初始化（绕过 LVGL）

- 用 `bsp_display_new()` / `bsp_display_new_with_handles()` 拿 `esp_lcd_panel_handle_t` + IO handle + DSI bus（BSP 注释明确这是"maximum flexibility"的无图形库路径）。
- 自行 `esp_lcd_panel_disp_on_off(panel, true)` + `bsp_display_backlight_on()`。
- `dpi_config.num_fbs` 由 1 改为 2（`m5stack_tab5.c:1134,1180`），用 `esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1)` 取得双缓冲。
- 注册 DPI vsync/refresh-done 回调，写入完成后在 vsync 边界切换显示缓冲。

### 4.2 渲染通路（去掉中间层）

- `render_frame()` 直接把 5x 放大结果写入当前后台 framebuffer：
  - 物理屏 720×1280 竖屏，横屏 1200×600 内容在写入时预转置为 600×1200 并居中（x 偏移 60，y 偏移 40）——与 P1 的"写入时预旋转"是同一操作，方向论证也相同。
  - 一次写入同时完成"放大 + 旋转 + 居中"，替代原来的"放大到 canvas → blend → PPA 旋转 → draw_bitmap"四步。
- 后台缓冲写完 → vsync 时切换到前台 → 原前台变下一帧后台。
- 锁机制：`bsp_display_lock` 换成自己管理的双缓冲互斥（或直接复用 `esp_lvgl_port_lock`，但那样仍留着 LVGL port，违背去中间层目标）。

### 4.3 触控处理

- 当前触控 indev 无 UI 消费者，可直接去掉 LVGL indev，保留 `esp_lcd_touch` 句柄供必要时读取坐标。
- np560 输入走物理键盘（`np560_tab5_keyboard.cpp`），不依赖 LVGL 触控。
- HAL 里 `sleepAndTouchWakeup()` 的 `lvglLock/unlock` 需换成无图形的互斥（该方法 np560 未调用，可保留但改实现）。

## 5. 收益评估

| 维度 | 当前（LVGL） | 原生方案 | 判断 |
|---|---|---|---|
| PSRAM | ~5.4 MB draw/旋转缓冲 + 1.4 MB canvas | 2×1.8 MB framebuffer，canvas 可省 | 净省约 3 MB |
| 全屏搬移 | canvas→draw buffer blend + PPA 旋转 | 写入时一次完成放大/旋转 | 少 1~2 次全屏读写 |
| 任务抢占 | LVGL task priority 4 | 无 | 解释器更独占 CPU |
| 撕裂 | 局部 draw_bitmap 无 vsync | 硬件双缓冲 + vsync | 根治 |
| 5x 清晰度 | 不变 | 不变（同算法） | 持平 |
| 画面方向 | 横屏（LVGL 90°） | 横屏（写入预转置） | 需静态论证 |

**量化预期**：渲染链路 CPU/带宽开销预计下降，撕裂从"必然"变"消除"。但解释器 IPS 当前已正常（131 万），故**整机 IPS 不会因此提升**；收益集中在显示流畅度与稳定性。这是架构简化 + 问题根治，不是性能数量级跃升。

## 6. 风险评估

| 风险 | 等级 | 缓解方式 |
|---|---|---|
| 写入预转置方向搞反 → 画面倒转/镜像 | 高 | 静态推导索引公式与 `lvgl_port_rotate_area` 270° 映射互逆；第一步只显示静态测试图案核对 |
| `num_fbs=1→2` 改动 BSP vendor 组件 | 中 | 在分支里改；或自建 panel 初始化，不动 BSP |
| 触控/LVGL 依赖残留导致编译失败 | 中 | 保留 LVGL 组件但不启动；仅替换显示出口；逐步解依赖 |
| HAL init 路径改动影响其他硬件 | 中 | `hal->init()` 保留，只把显示子路径切到原生；或新增 `hal->initForEmulator()` |
| 双缓冲切换时机错误 → 新一类撕裂 | 中 | 以 DPI vsync 回调为唯一提交点，后台缓冲写完才切换 |

## 7. 推进建议

**结论：值得做，但必须开独立分支、分阶段推进，不能并入 main。**

建议分支名：`feature/native-display`

分阶段（每阶段独立提交，静态论证优先，不频繁烧录）：

- **阶段 A（静态设计）**：在分支里写原生显示后端的设计与索引公式推导，明确 `num_fbs=2`、预转置映射、vsync 提交时序；代码层面确认与 HAL/BSP 的改动点。
- **阶段 B（最小验证）**：只让原生后端显示一张固定测试图案（如四角色块 + 箭头），确认方向、居中、双缓冲切换正确。**这一阶段不接 render_frame、不动键盘、不动固件**。静态推导 + 一次真机目视确认即可，不反复烧录。
- **阶段 C（接入 render_frame）**：把 5x 放大结果写进后台 framebuffer，保留原 LVGL 后端可切换（编译开关或运行时选择），A/B 对比确认无撕裂、IPS 不降、方向一致。
- **阶段 D（清理 LVGL）**：确认稳定后再移除 LVGL 组件依赖与 canvas，精简 HAL。

**红线**（沿用 `DISPLAY_FLUENCY_OPTIMIZATION.md`）：任何改动若无法在代码逻辑上证明不破坏显示方向与缓冲一致性，一律不实施；不以"烧录试试看"代替论证。P0 的教训是局部失效违反一致性必然撕裂，本方案必须整体替换、避免半切换状态。

## 8. 与本轮其他工作的边界

- main 保持 `f04fe38` + ESC 修复 + 文档，设备固件维持恢复版，不受本方案影响。
- `backup/fastboot-cd8b335` 的性能问题（25 倍 IPS 退化）与本方案无关，另行处理。
- 本评估不改 `dmcu_core/`，不触碰解释器。
