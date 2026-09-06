# 开发日志

完整记录 AI Passport Stardew Valley Dex 从立项到成型的全部开发过程，包括
踩过的坑、方案演进和最终实现。

---

## 阶段 0：可行性分析

**目标**：基于 FoloToy AI Passport 硬件（ESP32-C3、8 MB Flash、无 PSRAM、
240x320 ST7789 屏、三按键 ADC 键盘）做一台星露谷物语**离线图鉴**。

最大的矛盾是数据量 vs 硬件限制：

| 项 | 原始规模 | 限制 |
|---|---------|------|
| 条目 | ~1900 条（data/*.json） | factory 分区只有 3 MB |
| 图片 | ~17 MB PNG | 无 PSRAM，不能整图解压 |

调研结论：

1. **压缩决定成败**：像素画（16 色级调色板）经 raw-DEFLATE 压缩后体积
   极小——宝可梦参考项目 1000+ 张图仅 408 KB，星露谷素材预估 <1 MB。
2. **分区合法余量**：不新增分区也能放下——3 MB factory 内嵌全部资源即可，
   `cardid`（0x356000）与 `recovery`（0x700000）两个保护区不触碰，
   小程序 BLE 安装兼容性完整保留。
3. **架构**：纯 C 核心模块（可脱离 ESP-IDF 宿主测试）+ LVGL UI +
   构建期 Python 脚本把 JSON/图片编译成静态 C 表 + bin 资源。
4. **语言**：v1 用英文界面（Montserrat 字体零成本）；中文需自制字库。
5. **独立仓库** `ai-passport-stardew-valley`，仅复制 BSP 与 Recovery 钩子，
   不回 PR 上游。

---

## 阶段 1：项目初始化

- 从官方 `ai-passport` 复制 `components/bsp`（按键/显示/I2C/音频/电池驱动）
  与 `bootloader_components/recovery_boot_hook`（5 秒 UP 键进 Recovery）。
- 配置 `CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv`——
  分区表与模板逐字节一致。

---

## 阶段 2：数据管线（构建期）

`tools/gen_dex_data.py` + `tools/validate_dex_data.py`，源数据取自社区项目
[stardew-valley-data](https://github.com/chiefpansancolt/stardew-valley-data)：

```
data/*.json + images/
        │  gen_dex_data.py
        ▼
dex_static.c/h      条目表（cat/idx/sprite_idx/attrs_off/name/desc）
dex_sprites.bin     TOC(12B/条: off,len,w,h) + raw-DEFLATE RGB565 像素
dex_attrs.bin       "Label|Value\0" 连续字符串池（2934 条属性）
        │  validate_dex_data.py
        ▼
一致性校验（TOC 越界 / 解压回读 / 尺寸上限 96x96）
```

最终规模：**30 类、1161 条、1092 图**，全部嵌入 3 MB factory 应用。

关键处理：

- **透明色预混**：PNG 透明像素在生成期直接混到星露谷纸色 `#F4F0E0`，
  运行时零 alpha 混合，LVGL 直接按不透明 RGB565 渲染。
- **raw-DEFLATE**：每张图独立压缩（平均压缩比约 50%），运行时解压到一块
  静态 18 KB 缓冲（96x96x2 字节）。
- **属性串池**：`"Label|Value\0"` 连续存放，条目只存偏移，运行时零拷贝
  解析。实测最长属性值 90 字符、最长属性名 "Ingredients"。

---

## 阶段 3：三页 UI 状态机

- **分类网格 → 条目列表（LVGL Roller）→ 条目详情** 三页状态机。
- 布局几何收敛到 `dex_layout.c` 纯 C 结构体（240x320 各区域矩形），
  独立于 LVGL，可宿主校验。
- 按键映射：单击移动/确认，长按 ±10 快翻（详情页）或返回，双击跳首/末条。
- 位置记忆：NVS 落盘带 2 秒空闲去抖，连续翻页只落最后位置。

---

## 阶段 4：线程模型与图片显示

LVGL 非线程安全，且解码耗时不能堵 LVGL 任务，最终模型：

```
按键回调 (button 任务, 持 bsp_lvgl_lock)
  只改 RAM 状态 / 更新 LVGL 控件 / 投递消息
        │ 队列
        ▼
dex_work 任务 (优先级 5)
  MSG_SPRITE: 解码精灵图到双缓冲后备块 + 提交序列号 s_seq++
  MSG_SAVE:   NVS 位置落盘
        │
        ▼
LVGL 定时器 poll_tick (30ms)
  对比 s_seq 感知新帧 → 读就绪描述符 → 无缝替换旧图
```

踩过的两个坑：

| 坑 | 现象 | 修复 |
|---|------|------|
| 跨线程共用同一图像描述符 | 图片大面积错乱/撕裂 | 双缓冲 + 独立描述符，写者只写后备块、读者只读就绪块 |
| 布尔标志在快速连点下丢帧 | 快速翻页后图片不更新 | 递增序列号 `s_seq`，poll 侧对比消费，不丢不重 |

---

## 阶段 5：显示细节迭代（真机反馈驱动）

- 分类页标题改为 `Stardew Valley`。
- 顶栏电量加 `BAT:` 前缀（不在位显示 `BAT: --`）。
- **位置 chip 与电量重叠**：第一次改锚定电池左侧，真机仍重叠；第二次按
  "比图册名称右移一点点" 的方向改为 `lv_obj_align_to(chip, title,
  OUT_RIGHT_MID, 8)` 锚定类别名右侧 8px——用最坏组合（最长类别名
  "Artisan Goods" + "999/999"）验算右缘约 161px < 电池左缘约 166px。
- 图片加载优化：缩短 poll 周期、加深消息队列、解码请求优先入队。

---

## 阶段 6：偶发 NO IMAGE 修复（miniz tinfl 三个坑）

真机现象：进入图鉴按太快时，一段时间显示 "NO IMAGE"，稍后又能显示；
串口日志显示**同一 sprite 索引（130/131/132）`ok` 与 `none` 交替**。

排查出 miniz tinfl 的三个坑：

1. **栈溢出**：`tinfl_decompressor` 上下文约 33 KB（内嵌 32 KB LZ 字典），
   放在 4-6 KB 的任务栈上直接 `Stack protection fault`。必须静态分配，
   用底层 `tinfl_decompress` 而非便捷包装函数。
2. **每次必须 `tinfl_init`**：持久上下文残留协程状态 `m_state`，不重置
   会让下一次解压直接跳进中间状态而失败。
3. **NON_WRAPPING 输出缓冲要传全容量**（本次偶发失败的根因）：
   `out_bytes` 输入语义是"输出缓冲总容量/回溯字典大小"。曾传精确输出
   尺寸 `w*h*2`，在"恰好写满最后一字节且流结束"的脆弱边界上 LZ 回溯
   引用失败，表现为同一张图忽好忽坏。改为传 `out_cap * 2`（18 KB），
   输出长度再用精确值校验。

配套兜底：worker 层解码失败重试最多 3 次（每次 `vTaskDelay(1)`），覆盖
SPI flash 瞬间缓存未命中；日志加 `retries=N` 字段区分"真失败"与"重试成功"。

过程插曲：最初尝试的"进入图册前 loading 遮罩"方案与上述根因修复一并
做过一版，随后**整体撤回**（本地 `git reset --hard` + 远程
`push --force-with-lease` 回退到 `3de32b9`），只保留根因修复重新提交。

---

## 阶段 7：信息完整显示与无缝循环滚动

需求："属性名称要显示完全比如 Equipm……要显示出来而不是打省略号；
人物的介绍如果字很多就上下滚动等；总之信息要完全显示而不是省略"
（屏幕没有富余按键分配给"查看更多"，长内容只能自动滚动）。

- 详情页下半部合并为**信息面板**视口：描述 + 属性行按实测高度**流式
  竖排**（先设文本 → `lv_obj_update_layout` → 逐个量高定位），任何
  文本不截断。
- 属性行改 "Name: Value" 同行接排，值折行后悬挂缩进。
- **配色区分**否决了 LVGL recolor：检查数据发现 4/2934 条值含 `#`
  （如 "Secret Note #2"），会被 recolor 的 `#RRGGBB#` 语法误解析且无
  转义机制。改用**双 label 同行拼接**：名称灰（`#9A8C74`）、值墨
  （`#3B2F1E`），名称宽度自适应，值宽 = 216 - 名称宽 - 4（保底 60）。
- 滚动方案演进：第一版"滚到底停 1.5s 回顶"被用户指出回顶跳变生硬，
  改为**无缝 marquee**——内容渲染两份（组 A + 组 B，间隔 24px），
  匀速下移滚过一个周期（组 A 高 + 间隔）时瞬时回卷到顶部；组 B 开头
  与组 A 完全相同，画面无跳变。内容不足一屏则不滚。

---

## 踩坑总结（给后来者）

| # | 坑 | 教训 |
|---|-----|------|
| 1 | tinfl 上下文放任务栈 → Stack protection fault | ~33 KB 上下文必须静态分配，第三方库包装函数先查栈足迹 |
| 2 | 复用 tinfl 上下文不解压失败 | 每次解压前必须 `tinfl_init` 重置协程状态 |
| 3 | NON_WRAPPING out_bytes 传精确输出尺寸 → 偶发失败 | 该参数是"缓冲总容量/回溯字典大小"，必须传全容量 |
| 4 | 同一张图忽有忽无 | 解码路径配 3 次重试兜底 SPI flash 瞬态；日志带 retries=N |
| 5 | 跨线程共用图像描述符 → 撕裂 | 双缓冲 + 独立描述符，读写分离 |
| 6 | 快速连点图片不更新 | 布尔标志轮询丢帧，改递增序列号消费 |
| 7 | 位置 chip 与电量重叠 | chip 锚定类别名右侧而非固定偏移，用最坏宽度组合验算 |
| 8 | 属性值含 `#` 会被 recolor 误解析 | 用双 label 拼接替代 recolor，数据先行检查再定方案 |
| 9 | 中文显示变方块 | Montserrat 无中文字形，v1 用英文界面 |
| 10 | LVGL 非线程安全 | 一切 `lv_*` 调用都在持 `bsp_lvgl_lock` 的线程内完成 |

---

## 提交历史（分步提交记录）

```
c90dde9 feat: seamless marquee loop for the info panel
c1c03a0 feat: full-text info panel with auto-scroll on detail page
26a32fc fix: stabilize sprite decode with full tinfl dict and retries
3de32b9 feat: label header battery with a BAT: prefix
8524c66 docs: add bilingual project README
4404e53 feat: implement offline Stardew Valley dex firmware
f2b9777 feat: add dex data generation and validation pipeline
042d7ea chore: scaffold ESP-IDF build, BSP and recovery boot hook
2f4a1c2 Initial commit
```

## 后续可玩方向

- 音效（确认/翻页提示音，bsp_audio 已就绪）
- 条目收藏与"我的收藏"列表（NVS 位图）
- 英文前缀搜索/快速跳转
- 界面中文化（需自制中文字库）
- factory 余量有限，若扩充数据需考虑外置资源分区
