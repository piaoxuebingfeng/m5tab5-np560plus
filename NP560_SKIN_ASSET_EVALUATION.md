# NP560+ 机身皮肤图片资产调研与 Tab5 应用评估

日期：2026-11-25
调研对象：`scripts/web/assets/np560plus-open-cutout.png`（QEMU Web 前端资产）
目标：搞清该图片在 QEMU Web 页面中的使用机制，评估移植到 M5Stack Tab5 固件的可行路径，以及只取上半部分（去掉键盘）的可行性。
性质：**纯静态调研，未修改任何功能代码、未编译、未烧录。**

---

## 1. 结论摘要

1. Web 页面并没有把这张图片"渲染"进模拟器画面，而是把它当作 **CSS 背景皮肤**：图片铺满一个 1:1 容器，屏幕区域被抠成透明洞，洞的位置叠一个真实的 `<canvas>`（240×132），键盘区域叠一层透明按钮做热区。图片本身 `pointer-events: none`，不参与任何绘制计算。
2. 移植到 Tab5 **技术上可行**，但必须遵守 `DISPLAY_FLUENCY_OPTIMIZATION.md` 已确立的三条安全不变量。推荐路径是"离线转 RGB565 + LVGL 全屏 canvas 整幅合成 + 整幅 invalidate"，不碰 rotation/flush，也不重演 P0 的局部失效撕裂。
3. **只取上半部分（去掉键盘）更合适**：裁到 y≈780 后有效区接近 16:9，几乎正好铺满 Tab5 的 1280×720 逻辑横屏；RGB565 体积从 3.00 MB 降到约 1.8 MB；而且 Tab5 已有实体键盘，屏幕上再画一幅键盘图是冗余的。
4. 最大风险不是"能不能显示"，而是：抠图洞边缘的绿色残留、洞必须被画面精确填满、Tab5 触摸热区目前是 TODO（点图片上的按键不会生效）。

---

## 2. 图片资产实测属性

| 项目 | 实测值 | 获取方式 |
|---|---|---|
| 尺寸 | 1254 × 1254（正方形坐标系） | `file` / `identify` |
| 格式 | PNG, 8-bit RGBA, non-interlaced | 同上 |
| 文件大小 | 1.6 MB | `ls -lh` |
| 机身非透明外接框 | 1104 × 1086 +75 +86 | ImageMagick `alpha extract -format %@` |
| 屏幕透明洞（连通域） | 581 × 380 +349 +258，面积 177679 px | connected-components 分析 |
| 洞中心像素 | `srgba(0,0,0,0.0039)` → 几乎全透明 | `convert -format %[pixel:p{627,440}]` |
| 键盘/机身像素 | alpha ≈ 0.99（不透明） | 同上 |
| 键盘起始行 | y≈780（y=760 平均亮度 0.825 白壳，y=800 降到 0.36 黑键盘） | 逐行灰度扫描 |
| 洞边缘 | 有绿色色键残留（视觉可见的绿边） | 图片目视 |
| 解码为 RGB565（全图） | 3.00 MB | 1254×1254×2 |
| 解码为 RGB565（上半 1254×780） | 1.82 MB | 1254×780×2 |

---

## 3. Web 页面如何使用这张图片

### 3.1 DOM 三层结构（`scripts/web/index.html:33-39`）

```
.device-visual        position:relative; width:min(100%,860px); aspect-ratio:1
  ├─ <img class="device-art" src="assets/np560plus-open-cutout.png">
  │      width:100%; height:100%; object-fit:contain; pointer-events:none
  ├─ <div class="lcd-window">        position:absolute
  │      left:24.7%; top:18.9%; width:50.5%; height:34.4%
  │      └─ <canvas id="lcd" width="240" height="132">
  │             width:100%; height:auto; image-rendering:pixelated
  └─ <div id="skin-keys">            position:absolute; inset:0; pointer-events:none
         └─ 透明 <button class="skin-key"> × N（热区，pointer-events:auto）
```

关键点：

- `.lcd-window` 的百分比换算到 1254 坐标系约为 x=310..943 / y=237..669，套住透明洞（x=349..930 / y=258..638）并略微外扩，正好盖住抠图边缘。
- 图片 `pointer-events: none`，所有点击都交给 `#skin-keys` 层，因此"图片上的按键"实际是覆盖在图片上的透明按钮。
- `styles.css:103-128` 是全部样式；没有对图片做任何 canvas 合成、滤镜或缩放计算。

### 3.2 画面数据来源

- `app.js:555` 轮询 `/api/display`，服务端（`np560_web.py:1425-1429`）返回 `emulator.display()`。
- `app.js:283-315` 的 `drawFrame()`：把 gray5（240×120，每像素 5 bit）或内存诊断位图，按 off→on 颜色线性插值生成 `ImageData(240, 132)`——顶部 12 行是电池/段码 annunciator，主画面从第 12 行开始，最后 `putImageData()` 写入 canvas。
- canvas 的 240×132 由 CSS 放大到洞的尺寸，`image-rendering: pixelated` 保持像素风。

### 3.3 按键热区

- `app.js:621` 起 `buildSkinKeyboard()`：以 `size = 1254` 为坐标系，用 `left/top/width/height` 百分比给每个物理键建透明按钮。
- 上半部分热区：F1–F4（左列）、左侧翻页盘 M11/M19/M27、设置 M12、右侧快捷列 M59/M51/M43/M35、方向键/确认、返回 Escape（M04/0x8E）、发音/空格、背光（直接改滑杆）、侧边电源 M63。
- 下半部分键盘热区：QWERTY 三排、SHIFT、符号键、功能键行（英汉词典 M42、简明汉英 M50、屏幕取词 M07、帮助 M15、输入法 M58、删除 M47、查找 M23、添加 M31、修改 M39、目录）。
- `scripts/tests/test_np560_web_frontend.py` 有断言保证热区只映射到已验证的矩阵触点。

**结论：这张图片在 Web 端的角色是"皮肤 + 热区底图"，画面本身来自独立的 canvas。**

---

## 4. Tab5 侧现状约束

| 项目 | 现状 |
|---|---|
| 物理面板 | 720×1280 竖屏（MIPI-DSI） |
| LVGL 逻辑坐标 | 1280×720 横屏（`hal_esp32.cpp:104` `lv_display_set_rotation(...,90)`） |
| NP560 画面 | 3x → canvas 720×360（`np560_emu.cpp:36-38`），`lv_obj_center()` 居中，四周留底 |
| 渲染链路 | guest GRAM → 5 级灰度 → RGB565 源帧 → 3x 放大 → LVGL canvas → 90° 旋转 → PARTIAL flush（`avoid_tearing` 未开、无 vsync） |
| 三条安全不变量 | ① `s_canvas_buf` 模拟器与 LVGL 共享无锁；② 90° 旋转共用唯一旋转缓冲；③ 撕裂根因在送屏层。详见 `DISPLAY_FLUENCY_OPTIMIZATION.md` |
| PSRAM 余量 | resident.bin 装载后最大连续块约 6 MB；全屏 canvas 分配必须在固件装载之后 |
| 触控 | indev 存在但无 UI 控件消费（皮肤 TODO） |

---

## 5. 移植方案对比

| 方案 | 做法 | 与不变量的关系 | 评价 |
|---|---|---|---|
| **A. LVGL 全屏合成 canvas（推荐）** | 建 1280×720 RGB565 全屏 canvas；皮肤图离线转 RGB565 贴入，NP560 画面按洞坐标贴入；沿用现有 diff-gate，**有变化才整幅重绘 + 整幅 invalidate** | 保持全部三条：仍是单 buffer、整幅覆盖、不碰 flush/rotation | 架构改动最小，可复用现有 `render_frame` 的 diff 与放大逻辑；代价是每次更新多约 1.8 MB 背景 memcpy + 全屏 blend，静止时零开销 |
| B. LVGL 多对象（背景 image + 子 canvas） | 背景 image 静态创建一次，NP560 canvas 作为子对象定位到洞，只 invalidate 子对象 | 踩 P0 同类风险：局部失效 + 单旋转缓冲 + 无 vsync | LVGL 原生脏区合并比手写 `invalidate_area` 可靠，但仍可能撕裂；不推荐先做 |
| C. 原生显示后端 | `bsp_display_new()` 拿 panel handle，双 fb + vsync，在 fb 上合成背景与洞区域 | 根治撕裂，但需重写后端 | 即 `feature/native-display` 评估的方案，分支尚未实现，风险最高，不在本轮范围 |

### 图片落地方式

- **离线转换（推荐）**：用 Python/ImageMagick 把 PNG 转成 RGB565 raw（小端），运行时 `fread` 到 PSRAM，无需 LVGL PNG 解码（工程未找到 `LV_USE_PNG` 配置）。
- **存放位置**：TF 卡 `/sd/noahnp560/skin.bin`（遵守用户目录约定），或编译进 16 MB Flash（更稳，不依赖卡）。
- **内存时机**：皮肤 buffer 必须在 resident.bin 装载**之后**分配，否则会挤掉装载所需的连续 PSRAM（此前已有 `fread resident.bin failed` 的教训）。

---

## 6. 只用上半部分（去掉键盘）的可行性

**可行，且比全图更适合 Tab5。** 理由：

1. **比例匹配**：键盘从 y≈780 开始，上半部分有效区（机身 y=86..780）约 1254×694，宽高比 ≈1.81，几乎正好是 16:9，铺满 Tab5 逻辑 1280×720 横屏时几乎不变形；全图 1:1 方形铺横屏则必须上下裁掉键盘或留大黑边。
2. **避免冗余**：Tab5 已有实体键盘（`np560_tab5_keyboard.cpp` 的映射），屏幕上再画一幅键盘图是重复；去掉后皮肤只保留"上壳 + 屏幕洞 + 侧键"，更像一台打开上盖的真机。
3. **成本减半**：RGB565 从 3.00 MB 降到 1.82 MB，memcpy/blend 量相应下降。
4. **洞完整保留**：洞在 y=258..638，完全落在上半部分内，NP560 画面仍可嵌入。

**必须处理的细节**：

- 裁切边界：建议 y≈780；y=760 仍是白壳，y=800 已是黑键盘。底部 "PUSH" 拉手与黑色连接条要决定保留还是裁掉。
- 绿色边：洞边缘的绿色色键残留需离线去绿/羽化，否则在设备上会看到绿圈。
- 洞必须填满：洞区域 alpha≈0，NP560 画面必须精确覆盖，否则露黑底；建议离线生成一张"洞矩形"坐标表（图片坐标 → canvas 坐标）并静态核对。
- 缩放关系（按等比 1.02 倍铺 1280 宽估算）：洞约占总宽 46%，对应屏幕上约 593 px 宽；NP560 源 240 px 放大约 2.47 倍填入，按宽度适配后约 593×296，上下留边——与 Web 的 `object-fit` 行为一致。
- 触摸热区：Web 上端可按图片上的 F1/方向键/返回；Tab5 目前触控皮肤是 TODO，点图片按键暂不会生效。这是额外功能，不是显示可行性的阻碍。
- 方向：方案 A 直接按 LVGL 逻辑坐标写全屏 canvas，图片按最终横屏方向预置即可，不需要额外推导 90° 转置。

---

## 7. 建议下一步（静态优先，不烧录试错）

1. 离线生成上半部分皮肤 RGB565 raw：裁 y=86..780、去绿边、洞内填黑，输出尺寸与坐标表。
2. 写一张坐标映射表：图片像素 → 1280×720 canvas 像素 → 洞矩形 → NP560 源帧缩放参数，静态核对方向与边界。
3. 在 `feature/native-display` 之外的独立位置做**纯静态设计评审**，确认不触碰三条不变量后，再由用户决定是否进入实现。
4. 实现可分两步：先加"皮肤资源加载 + 静态显示"（不接 `render_frame`，只验证图片贴上去方向/位置正确），再接 diff 合成。每步独立提交。

---

## 8. 未决问题

- 用户是否希望保留键盘区域（全图）还是只取上半部分（推荐）。
- 皮肤资源最终放 TF 卡还是编译进 Flash。
- 是否需要把图片上的侧键做成触摸热区（需另做 indev 消费，属于独立功能）。
