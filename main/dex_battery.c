// main/dex_battery.c —— 电量显示与空闲降背光。
// lv_timer 回调运行在 LVGL 任务内,可直接操作对象。
#include "dex_battery.h"

#include "bsp_battery.h"
#include "bsp_display.h"

#include "lvgl.h"

#define DEX_IDLE_DIM_S 60 /* 无按键 60s 后背光降到 25% */

static lv_obj_t *s_label;
static lv_timer_t *s_bat_timer;
static lv_timer_t *s_idle_timer;
static int s_idle_sec;
static bool s_dimmed;

static void bat_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_label) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_label, "--");
    } else {
        lv_label_set_text_fmt(s_label, "%d%%", soc);
    }
}

static void idle_tick(lv_timer_t *t)
{
    (void)t;
    if (s_dimmed) return;
    s_idle_sec++;
    if (s_idle_sec >= DEX_IDLE_DIM_S) {
        bsp_display_backlight(25);
        s_dimmed = true;
    }
}

void dex_battery_timers_start(void)
{
    if (s_bat_timer) return;
    s_bat_timer = lv_timer_create(bat_tick, 2000, NULL);
    s_idle_timer = lv_timer_create(idle_tick, 1000, NULL);
    bat_tick(NULL);
}

void dex_battery_attach(lv_obj_t *label) { s_label = label; }

void dex_battery_activity(void)
{
    s_idle_sec = 0;
    if (s_dimmed) {
        bsp_display_backlight(100);
        s_dimmed = false;
    }
}
