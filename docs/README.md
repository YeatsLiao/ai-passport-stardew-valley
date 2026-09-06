# 开发文档

## 项目概述

AI Passport Stardew Valley Dex 是基于 FoloToy AI Passport 硬件（ESP32-C3、
8 MB Flash、240x320 ST7789 屏、三按键 ADC 键盘）的星露谷物语**离线图鉴**
固件。设备完全离线运行：无 WiFi、无蓝牙、无网络。全部数据 —— 30 个类别、
1161 条条目、1092 张像素图 —— 均嵌入 3 MB factory 应用镜像内。

## 页面与按键

三页 UI 状态机：分类网格 → 条目列表 → 条目详情。

| 页面 | 按键 | 动作 |
|------|------|------|
| 分类网格 | UP / DOWN（单击） | 移动选择 |
| 分类网格 | OK（单击） | 进入类别 |
| 条目列表 | UP / DOWN（单击） | 移动选择 |
| 条目列表 | UP / DOWN（长按） | 跳 +-10 条 |
| 条目列表 | OK（单击） | 打开条目 |
| 条目列表 | OK（长按） | 返回分类 |
| 详情页 | UP / DOWN（单击） | 上 / 下一条（±1） |
| 详情页 | UP / DOWN（长按） | 快速翻页（±10） |
| 详情页 | UP / DOWN（双击） | 跳到首条 / 末条 |
| 详情页 | OK（长按） | 返回列表 |

### 屏幕显示

- **分类页**：标题 `Stardew Valley` + 2 列类别卡片（名称与条目数）。
- **列表页**：LVGL Roller 全量条目名列表。
- **详情页**：
  - 顶部标题条：类别名、位置 chip（`12/150`，紧贴类别名右侧）、
    `BAT: xx%` 电量（CW2017 电量计，不在位时 `BAT: --`）。
  - 图片井 96x96：像素图居中（≤48px 的图自动 2x 最近邻放大）。
  - 信息面板：描述 + `Name: Value` 属性行，**完整显示不打省略号**；
    内容超出面板高度时以约 33px/s 匀速**无缝循环滚动**（marquee）。
  - 底部提示条：当前页可用按键。

## 技术架构

```
┌──────────────────────────────────────────────────────┐
│                        main/                          │
│  dex_ui.c        三页 UI 状态机 + 信息面板滚动         │
│  dex_core.c      静态数据访问 + 属性串解析             │
│  dex_sprite.c    运行时解压 (miniz tinfl) + 2x 缩放    │
│  dex_layout.c    240x320 布局几何（纯 C 可宿主校验）    │
│  dex_battery.c   电量指示 + 60s 空闲背光调暗            │
│  dex_static.c    生成产物：条目表 + 属性串池             │
│  dex_sprites.bin / dex_attrs.bin  生成产物：嵌入资源    │
├──────────────────────────────────────────────────────┤
│  vendor/miniz/   miniz（仅 tinfl，raw-DEFLATE 解压）   │
├──────────────────────────────────────────────────────┤
│               components/bsp/                         │
│  bsp_button / bsp_display / bsp_i2c / bsp_battery     │
├──────────────────────────────────────────────────────┤
│        ESP-IDF 5.5.x + LVGL 9.5 + NVS                 │
└──────────────────────────────────────────────────────┘
```

### 数据管线

数据源自社区项目 [stardew-valley-data](https://github.com/chiefpansancolt/stardew-valley-data)
（data/*.json + images/），由 `tools/gen_dex_data.py` 一次性编译为固件内嵌资源：

```
data/*.json + images/          stardew-valley-data 仓库
        │  gen_dex_data.py
        ▼
dex_static.c/h                 条目表（cat/idx/sprite_idx/attrs_off/name/desc）
dex_sprites.bin                TOC(12B/条: off,len,w,h) + raw-DEFLATE RGB565 像素
dex_attrs.bin                  "Label|Value\0" 连续字符串池
        │  validate_dex_data.py
        ▼
一致性校验（TOC 越界 / 解压回读 / 尺寸上限 96x96）
```

关键处理：

- **透明色预混**：PNG 透明像素在生成期直接混合到星露谷纸色 `#F4F0E0`，
  运行时无需 alpha 混合，LVGL 直接按 RGB565 不透明图渲染。
- **raw-DEFLATE**：每张图独立压缩（平均压缩比约 50%），运行时用 miniz
  的 tinfl 解压到一块静态 18 KB 缓冲（96x96x2 字节）。
- **属性串池**：`"Label|Value\0"` 连续存放，条目只存偏移，运行时零拷贝解析。

### 线程模型

```
按键回调 (button 任务, 持 bsp_lvgl_lock)
  只改 RAM 状态 / 更新 LVGL 控件 / 投递消息
        │ 队列
        ▼
dex_work 任务 (优先级 5)
  MSG_SPRITE: 解码精灵图到双缓冲后备块 + 提交序列号 s_seq++
  MSG_SAVE:   NVS 位置落盘（2s 空闲去抖，连续翻页只落最后位置）
        │
        ▼
LVGL 定时器 poll_tick (30ms)
  对比 s_seq 感知新帧 → 读就绪描述符 → 无缝替换旧图
```

- worker 与 LVGL 通过**双缓冲 + 独立描述符 + 递增序列号**协作：
  写者只写后备块，读者只读就绪块，序列号避免布尔标志在快速连点下丢帧。
- LVGL 非线程安全：一切 `lv_*` 调用都在持 `bsp_lvgl_lock` 的线程内完成。

## 代码结构

```
├── main/
│   ├── dex_ui.c            # 三页 UI 状态机、信息面板、无缝滚动
│   ├── dex_core.c/h        # 数据访问层（类别/条目/属性解析）
│   ├── dex_sprite.c/h      # tinfl 解压 + 2x 最近邻缩放
│   ├── dex_layout.c/h      # 布局几何（纯 C，带宿主校验辅助函数）
│   ├── dex_battery.c/h     # 电量 + 空闲息屏
│   ├── dex_static.c/h      # 生成产物：1161 条目表 + 属性池引用
│   ├── dex_sprites.bin     # 生成产物：精灵图集
│   ├── dex_attrs.bin       # 生成产物：属性字符串池
│   └── vendor/miniz/       # miniz（仅 tinfl 相关源文件）
├── components/bsp/         # 官方 AI Passport 硬件驱动
├── bootloader_components/recovery_boot_hook/
│                           # 5 秒 UP 键进入 Recovery（模板契约）
├── tools/
│   ├── gen_dex_data.py     # 数据管线：JSON/图片 → C 表 + bin 资源
│   └── validate_dex_data.py# 产物一致性校验
├── docs/
│   ├── README.md           # 本文档
│   └── development-log.md  # 完整开发日志（方案演进与踩坑记录）
├── CMakeLists.txt / sdkconfig.defaults / partitions.csv
└── README.md / README.zh_CN.md
```

## 构建与烧录

### 环境要求

- ESP-IDF 5.5.x（开发验证于 5.5.5），目标 esp32c3
- 托管组件（LVGL 9.5、esp_lvgl_port、espressif/button）首次配置自动拉取
- 重新生成数据资源才需要 Python 3.11 + Pillow

### 构建步骤

```bash
idf.py set-target esp32c3
idf.py build
idf.py merge-bin        # 完整镜像 build/merged-binary.bin（约 1.5 MB）
idf.py -p COMx flash monitor
```

或直接用 esptool 烧录合并镜像：

```bash
python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset write_flash 0x0 build/merged-binary.bin
```

重新生成数据资源（在同级 stardew-valley-data 检出存在时）：

```bash
python tools/gen_dex_data.py --data D:\path\to\stardew-valley-data
python tools/validate_dex_data.py --data D:\path\to\stardew-valley-data
```

### 分区契约

完整保留模板分区布局 —— 小程序 BLE 安装器依赖它：

| 名称 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 24 KB | 键值存储（上次浏览位置） |
| factory | 0x10000 | 3 MB | 应用（图鉴 + 嵌入资源） |
| cardid | 0x356000 | 16 KB | BLE 配对数据（保护区） |
| recovery | 0x700000 | 1 MB | 常驻 Recovery 应用（保护区） |

合并镜像止于约 `0x16EC00`，距 cardid 保护区 `0x356000` 余量充足。
引导阶段按住 UP 键 5 秒进入 Recovery 的钩子已保留。

## 关键实现细节

### 图片不显示的三层排查

串口日志 `dex_ui: sprite <idx> -> ok/none WxH buf<K> retries=<n>` 可直接判读：

| 现象 | 含义 | 处置 |
|------|------|------|
| `ok 96x96` | 解码成功 | —— |
| `none 0x0 retries=3` | 真·无图（`DEX_SPRITE_NONE`）或数据损坏 | 正常（7 条目无图）|
| `ok ... retries=1/2` | 曾瞬态失败后重试成功 | miniz 边界 / flash 缓存未命中已被兜住 |

### miniz tinfl 的三个坑

1. **栈溢出**：`tinfl_decompressor` 上下文约 33 KB，绝不能放任务栈
   （4-6 KB 栈直接触发 `Stack protection fault`），必须静态分配。
2. **每次必须 `tinfl_init`**：持久上下文残留协程状态 `m_state`，
   不重置会让下一次解压直接跳进中间状态而失败。
3. **NON_WRAPPING 输出缓冲要传全容量**：`out_bytes` 输入语义是
   "输出缓冲总容量/回溯字典大小"。传精确输出尺寸（`w*h*2`）会在
   "恰好写满最后一字节" 的脆弱边界上偶发 `TINFL_STATUS_FAILED`，
   表现为同一张图同一索引忽好忽坏。必须传 `out_cap * 2`（18 KB），
   输出长度再用精确值校验。

### 信息面板的无缝循环滚动

屏幕没有富余按键可分配给"查看更多"，长内容只能自动滚动。实现：

- 描述 + 属性行按实测高度**流式竖排**（先设文本 → `lv_obj_update_layout`
  → 逐个量高定位），任何文本都不截断。
- 内容渲染**两份**（组 A + 组 B，间隔 24px），滚过"组 A 高度 + 间隔"的
  周期点时瞬时回卷到顶部——组 B 开头与组 A 完全相同，画面无跳变。
- 内容不足一屏则不滚；属性行采用灰名（`#9A8C74`）+ 墨值（`#3B2F1E`）
  双 label 同行接排（不用 LVGL recolor：数据值里含 `#` 字符会被误解析）。

### 属性值与位置 chip 的排版

- 属性名宽度自适应（完整不截断），值紧随其右 4px 接排，折行后悬挂缩进。
- 详情页位置 chip 用 `lv_obj_align_to(chip, title, OUT_RIGHT_MID, 8)`
  锚定类别名右侧，而非固定偏移——不同长度的类别名和计数都不会与
  `BAT: 100%` 重叠。

## 故障排查

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 屏幕白屏 + `Stack protection fault in dex_work` | tinfl 上下文在栈上 | 已修复：33 KB 上下文改静态分配 |
| 同一张图忽有忽无 | miniz 精确 out_bytes 边界 / flash 缓存未命中 | 已修复：全容量字典 + 3 次重试；看日志 `retries=N` |
| 图片大面积错乱/撕裂 | 跨线程共用同一图像描述符 | 已修复：双缓冲 + 独立描述符 + 序列号 |
| 信息面板不滚动 | 内容不足一屏（`s_scroll_max = 0`） | 正常现象 |
| 电量显示 `BAT: --` | 板上无 CW2017 电量计 | 正常现象 |
| 快速连点后图片不更新 | 旧版布尔标志轮询丢帧 | 已修复：递增序列号 `s_seq` 消费 |
| 烧录后卡 Recovery | 误触 5 秒 UP 键钩子 | 松开按键重启即可 |

## 扩展方向

- 音效（确认/翻页提示音，bsp_audio 已就绪）
- 条目收藏与"我的收藏"列表（NVS 位图）
- 英文前缀搜索/快速跳转
- 界面中文化（需自制中文字库；Montserrat 无中文字形）
- 剩余 3 MB 余量有限，若扩充数据需考虑 SPIFFS 外置资源

## 参考资料

- [stardew-valley-data](https://github.com/chiefpansancolt/stardew-valley-data) —— 数据与像素图来源
- [FoloToy AI Passport 官方仓库](https://github.com/FoloToy/ai-passport) —— 硬件与 BSP
- [miniz](https://github.com/richgel999/miniz) —— raw-DEFLATE 解压（MIT）
- [LVGL 9 文档](https://docs.lvgl.io/9/)
- Stardew Valley 版权归 ConcernedApe 所有；本项目为非商业同人作品。
