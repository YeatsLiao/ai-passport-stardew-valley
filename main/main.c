// main/main.c —— ai-passport-stardew-valley:星露谷物语离线图鉴。
//
// 纯离线单体应用:上电即进图鉴(无 demo 菜单)。
// 数据与图片内置于固件(tools/gen_dex_data.py 生成,来源 stardew-valley-data)。
// 线程纪律:按键回调持 bsp_lvgl_lock() 后转发给 dex_ui_key;worker 任务负责
// 图片解压与 NVS 落盘,不触碰任何 lv_* 对象。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "dex_ui.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"

#include "lvgl.h"

static const char *TAG = "main";

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    dex_ui_key(btn, ev);
    bsp_lvgl_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "ai-passport-stardew-valley 启动");

    bsp_i2c_init();

    // 屏幕是本应用的 UI 载体,失败则没有继续的意义 —— 打清日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 电量计失败不阻塞:UI 上显示 "--"。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电池计不可用(CW2017 无应答)");
    }

    // 按键失败则只能重启 —— 图鉴的全部交互都依赖三键。
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败(ADC 电阻梯 GPIO0)");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        dex_ui_init();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "图鉴就绪");
}
