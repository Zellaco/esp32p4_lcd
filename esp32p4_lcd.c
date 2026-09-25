/*
 * 4D Systems Pty Ltd
 * www.4dsystems.com.au
 *
 * SPDX-FileCopyrightText: 
 *   - 4D Systems Pty Ltd
 * SPDX-License-Identifier: Apache-2.0
 * 
 */

/**
 * @file esp32p4_lcd.c
 * @brief Platform-specific initialization wrappers for 4D Systems ESP32-P4 LCD & Touch subsystems.
 * 
 * Initialization order recommendation:
 * 1. Configure I2C bus (for touch and onboard components)
 * 2. esp32p4_lcd_full_init()    -> creates panel handle for drawing
 * 3. esp32p4_lcd_touch_init()   -> prepares touch input events
 * 4. esp32p4_lcd_backlight_init() + set() -> enable display visibility
 */


#include <stdio.h>
#include "esp_lcd_touch_gt911.h"

#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"

#include "esp32p4_lcd.h"
#include "mipi_init.h"

static const char *TAG = "esp32p4_lcd";

static bool backlight_init = false;

esp_err_t esp32p4_lcd_backlight_init(void)
{
    if (backlight_init) return ESP_OK;
    
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = LCD_BL_PWM_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "backlight timer configuration failed");

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LCD_BL_GPIO_NUM,
        .duty           = 0,    // Start with 0% duty cycle
        .hpoint         = 0
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_channel), TAG, "backlight channel configuration failed");

    backlight_init = true;
    
    return ESP_OK;
}

esp_err_t esp32p4_lcd_backlight_set(uint8_t brightness)
{
    if (!backlight_init) esp32p4_lcd_backlight_init();
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness), TAG, "set backlight duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "update backlight duty failed");
    return ESP_OK;
}

esp_err_t esp32p4_lcd_full_init(esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = LCD_DSI_PHY_LDO_ID,
        .voltage_mv = LCD_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy), TAG, "MIPI DSI LDO init failed");

    ESP_LOGI(TAG, "Initialize MIPI DSI bus");
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = ESP32P4_LCD_MIPI_DSI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), TAG, "MIPI DSI bus init failed");

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = ESP32P4_LCD_MIPI_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io), TAG, "MIPI DBI IO init failed");

    ESP_LOGI(TAG, "Install JD9365 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_dpi_panel_config_t dpi_config = ESP32P4_LCD_MIPI_DPI_CONFIG();
    jd9365_vendor_config_t vendor_config = {
        .init_cmds = esp32p4_4d_init_cmds,
        .init_cmds_size = sizeof(esp32p4_4d_init_cmds) / sizeof(jd9365_lcd_init_cmd_t),
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_GPIO_NUM,           // Set to -1 if not use
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,     // Implemented by LCD command `36h`
        .bits_per_pixel = LCD_BITS_PER_PIXEL,    // Implemented by LCD command `3Ah` (16/18/24)
        .vendor_config = &vendor_config,
    };

    bool rotate = false;
#if defined(CONFIG_ESP32P4_LCD_LANDSCAPE)
    rotate = true;
#endif

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(mipi_dbi_io, &panel_config, &panel_handle, rotate), TAG, "panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "panel display on failed");

    ESP_RETURN_ON_ERROR(esp32p4_lcd_backlight_init(), TAG, "backlight init failed");
#if defined(CONFIG_ESP32P4_BACKLIGHT_AUTO_ON)
    ESP_RETURN_ON_ERROR(esp32p4_lcd_backlight_set(255), TAG, "backlight set failed");
#endif

    *ret_panel = panel_handle;

    return ESP_OK;
}

#if !defined(CONFIG_ESP32S3_LCD_NOTOUCH)
/* i2c_bus: caller's already-initialized bus, not created or deleted here. */
esp_err_t esp32p4_lcd_touch_init(i2c_master_bus_handle_t i2c_bus, esp_lcd_touch_handle_t *tp) {
    esp_err_t ret = ESP_OK;

    if (i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;

    esp_lcd_touch_config_t tp_cfg = {
#if defined(LCD_TOUCH_SWAP_MAX_XY) && (LCD_TOUCH_SWAP_MAX_XY == 1)
        .x_max = LCD_TOUCH_AREA_MAX_Y,
        .y_max = LCD_TOUCH_AREA_MAX_X,
#else
        .x_max = LCD_TOUCH_AREA_MAX_X,
        .y_max = LCD_TOUCH_AREA_MAX_Y,
#endif
        .rst_gpio_num = LCD_TOUCH_RST_GPIO_NUM,
        .int_gpio_num = LCD_TOUCH_INT_GPIO_NUM,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = LCD_TOUCH_SWAP_XY,
            .mirror_x = LCD_TOUCH_MIRROR_X,
            .mirror_y = LCD_TOUCH_MIRROR_Y,
        },
    };

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    touch_io_config.scl_speed_hz = 400000;

    ret = esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_config, &io_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_touch_io_gt911_config_t gt911_addr_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };
    tp_cfg.driver_data = &gt911_addr_cfg;

    ret = esp_lcd_touch_new_i2c_gt911(io_handle, &tp_cfg, tp);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(io_handle);
        return ret;
    }

    return ESP_OK;
}
#endif 