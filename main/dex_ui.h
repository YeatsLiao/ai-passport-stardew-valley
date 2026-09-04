// main/dex_ui.h —— 星露谷图鉴界面:分类页 → 列表页 → 详情页。
// dex_ui_key() 必须在已持 bsp_lvgl_lock() 的上下文(main.c 按键回调)调用。
#pragma once

#include "bsp_button.h"

// 初始化:构建分类页并启动 worker(图片解码/NVS 持久化)。需在 LVGL 就绪后调用。
void dex_ui_init(void);

// 按键分发(运行于 LVGL 任务,由 main.c 持锁转发)。
void dex_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
