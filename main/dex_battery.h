// main/dex_battery.h —— 电量显示与空闲降背光(LVGL 任务内定时器,天然持锁)。
#pragma once

#include "lvgl.h"

// 创建全局电量/空闲定时器(幂等)。label 由各页面创建后调 attach 注册。
void dex_battery_timers_start(void);

// 页面切换后注册当前电量 label(旧引用自动失效)。
void dex_battery_attach(lv_obj_t *label);

// 任意按键活动后调用,重置空闲计数并恢复背光。
void dex_battery_activity(void);
