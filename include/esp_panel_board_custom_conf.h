#pragma once

// *INDENT-OFF*

#ifndef ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM
#define ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM  (0)
#endif

#if ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM

#include <driver/gpio.h>
#include <driver/i2c.h>

#define ESP_PANEL_BOARD_NAME                "Waveshare:ESP32_S3_TOUCH_LCD_7B"

#define ESP_PANEL_BOARD_WIDTH               (1024)
#define ESP_PANEL_BOARD_HEIGHT              (600)

#define ESP_PANEL_BOARD_USE_LCD             (1)

#if ESP_PANEL_BOARD_USE_LCD
#define ESP_PANEL_BOARD_LCD_CONTROLLER      ST7262
#define ESP_PANEL_BOARD_LCD_BUS_TYPE        (ESP_PANEL_BUS_TYPE_RGB)

#if ESP_PANEL_BOARD_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_RGB
#define ESP_PANEL_BOARD_LCD_RGB_USE_CONTROL_PANEL       (0)
#define ESP_PANEL_BOARD_LCD_RGB_CLK_HZ                  (30 * 1000 * 1000)
#define ESP_PANEL_BOARD_LCD_RGB_HPW                     (162)
#define ESP_PANEL_BOARD_LCD_RGB_HBP                     (152)
#define ESP_PANEL_BOARD_LCD_RGB_HFP                     (48)
#define ESP_PANEL_BOARD_LCD_RGB_VPW                     (45)
#define ESP_PANEL_BOARD_LCD_RGB_VBP                     (13)
#define ESP_PANEL_BOARD_LCD_RGB_VFP                     (3)
#define ESP_PANEL_BOARD_LCD_RGB_PCLK_ACTIVE_NEG         (1)
#define ESP_PANEL_BOARD_LCD_RGB_DATA_WIDTH              (16)
#define ESP_PANEL_BOARD_LCD_RGB_PIXEL_BITS              (ESP_PANEL_LCD_COLOR_BITS_RGB565)
#define ESP_PANEL_BOARD_LCD_RGB_BOUNCE_BUF_SIZE         (ESP_PANEL_BOARD_WIDTH * 10)
#define ESP_PANEL_BOARD_LCD_RGB_IO_HSYNC                (46)
#define ESP_PANEL_BOARD_LCD_RGB_IO_VSYNC                (3)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DE                   (5)
#define ESP_PANEL_BOARD_LCD_RGB_IO_PCLK                 (7)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DISP                 (-1)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA0                (14)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA1                (38)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA2                (18)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA3                (17)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA4                (10)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA5                (39)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA6                (0)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA7                (45)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA8                (48)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA9                (47)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA10               (21)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA11               (1)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA12               (2)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA13               (42)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA14               (41)
#define ESP_PANEL_BOARD_LCD_RGB_IO_DATA15               (40)
#endif

#define ESP_PANEL_BOARD_LCD_COLOR_BITS                  (ESP_PANEL_LCD_COLOR_BITS_RGB888)
#define ESP_PANEL_BOARD_LCD_COLOR_BGR_ORDER             (0)
#define ESP_PANEL_BOARD_LCD_COLOR_INEVRT_BIT            (0)
#define ESP_PANEL_BOARD_LCD_SWAP_XY                     (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_X                    (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_Y                    (0)
#define ESP_PANEL_BOARD_LCD_GAP_X                       (0)
#define ESP_PANEL_BOARD_LCD_GAP_Y                       (0)
#define ESP_PANEL_BOARD_LCD_RST_IO                      (-1)
#define ESP_PANEL_BOARD_LCD_RST_LEVEL                   (0)
#endif

#define ESP_PANEL_BOARD_USE_TOUCH               (1)

#if ESP_PANEL_BOARD_USE_TOUCH
#define ESP_PANEL_BOARD_TOUCH_CONTROLLER                GT911
#define ESP_PANEL_BOARD_TOUCH_BUS_TYPE                  (ESP_PANEL_BUS_TYPE_I2C)
#define ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST        (1)
#define ESP_PANEL_BOARD_TOUCH_I2C_HOST_ID               (0)
#define ESP_PANEL_BOARD_TOUCH_I2C_CLK_HZ                (400 * 1000)
#define ESP_PANEL_BOARD_TOUCH_I2C_SCL_PULLUP            (1)
#define ESP_PANEL_BOARD_TOUCH_I2C_SDA_PULLUP            (1)
#define ESP_PANEL_BOARD_TOUCH_I2C_IO_SCL                (9)
#define ESP_PANEL_BOARD_TOUCH_I2C_IO_SDA                (8)
#define ESP_PANEL_BOARD_TOUCH_I2C_ADDRESS               (0x5D)
#define ESP_PANEL_BOARD_TOUCH_SWAP_XY                   (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_X                  (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_Y                  (0)
#define ESP_PANEL_BOARD_TOUCH_RST_IO                    (-1)
#define ESP_PANEL_BOARD_TOUCH_RST_LEVEL                 (0)
#define ESP_PANEL_BOARD_TOUCH_INT_IO                    (4)
#define ESP_PANEL_BOARD_TOUCH_INT_LEVEL                 (0)
#endif

#define ESP_PANEL_BOARD_USE_BACKLIGHT           (1)

#if ESP_PANEL_BOARD_USE_BACKLIGHT
#define ESP_PANEL_BOARD_BACKLIGHT_TYPE                  (ESP_PANEL_BACKLIGHT_TYPE_CUSTOM)
#define ESP_PANEL_BOARD_BACKLIGHT_IO                    (2)
#define ESP_PANEL_BOARD_BACKLIGHT_ON_LEVEL              (1)
#define ESP_PANEL_BOARD_BACKLIGHT_IDLE_OFF              (0)
#endif

#define ESP_PANEL_BOARD_USE_EXPANDER            (0)

#if ESP_PANEL_BOARD_USE_EXPANDER
#define ESP_PANEL_BOARD_EXPANDER_CHIP                   TCA95XX_8BIT
#define ESP_PANEL_BOARD_EXPANDER_SKIP_INIT_HOST         (0)
#define ESP_PANEL_BOARD_EXPANDER_I2C_HOST_ID            (0)
#define ESP_PANEL_BOARD_EXPANDER_I2C_CLK_HZ             (400 * 1000)
#define ESP_PANEL_BOARD_EXPANDER_I2C_SCL_PULLUP         (1)
#define ESP_PANEL_BOARD_EXPANDER_I2C_SDA_PULLUP         (1)
#define ESP_PANEL_BOARD_EXPANDER_I2C_IO_SCL             (9)
#define ESP_PANEL_BOARD_EXPANDER_I2C_IO_SDA             (8)
#define ESP_PANEL_BOARD_EXPANDER_I2C_ADDRESS            (0x24)
#endif

#ifndef BIT
#define BIT(n) (1U << (n))
#endif

static constexpr i2c_port_t SP7_7B_I2C_PORT = I2C_NUM_0;
static constexpr uint8_t SP7_7B_IO_ADDR = 0x24;
static constexpr uint8_t SP7_7B_IO_REG_MODE = 0x02;
static constexpr uint8_t SP7_7B_IO_REG_OUTPUT = 0x03;
static constexpr TickType_t SP7_7B_I2C_TIMEOUT = pdMS_TO_TICKS(100);

static constexpr uint8_t SP7_7B_IO_TOUCH_RST = 1;
static constexpr uint8_t SP7_7B_IO_BACKLIGHT = 2;
static constexpr uint8_t SP7_7B_IO_LCD_RST = 3;

// Follow the official Waveshare demo default: all outputs high.
static uint8_t sp7_7b_io_output_state = 0xFF;
static bool sp7_7b_i2c_ready = false;

static inline bool sp7_7b_i2c_begin()
{
    if (sp7_7b_i2c_ready) {
        return true;
    }

    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = static_cast<gpio_num_t>(ESP_PANEL_BOARD_TOUCH_I2C_IO_SDA),
        .scl_io_num = static_cast<gpio_num_t>(ESP_PANEL_BOARD_TOUCH_I2C_IO_SCL),
        .sda_pullup_en = ESP_PANEL_BOARD_TOUCH_I2C_SDA_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .scl_pullup_en = ESP_PANEL_BOARD_TOUCH_I2C_SCL_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .master = {
            .clk_speed = static_cast<uint32_t>(ESP_PANEL_BOARD_TOUCH_I2C_CLK_HZ),
        },
        .clk_flags = 0,
    };

    if (i2c_param_config(SP7_7B_I2C_PORT, &config) != ESP_OK) {
        return false;
    }

    esp_err_t err = i2c_driver_install(SP7_7B_I2C_PORT, config.mode, 0, 0, 0);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return false;
    }

    sp7_7b_i2c_ready = true;
    return true;
}

static inline bool sp7_7b_write_reg(uint8_t reg, uint8_t value)
{
    if (!sp7_7b_i2c_begin()) {
        return false;
    }

    const uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(
               SP7_7B_I2C_PORT, SP7_7B_IO_ADDR, data, sizeof(data), SP7_7B_I2C_TIMEOUT
           ) == ESP_OK;
}

static inline bool sp7_7b_sync_outputs()
{
    return sp7_7b_write_reg(SP7_7B_IO_REG_OUTPUT, sp7_7b_io_output_state);
}

static inline bool sp7_7b_set_io(uint8_t pin, bool level_high)
{
    if (level_high) {
        sp7_7b_io_output_state |= BIT(pin);
    } else {
        sp7_7b_io_output_state &= static_cast<uint8_t>(~BIT(pin));
    }
    return sp7_7b_sync_outputs();
}

static inline bool sp7_7b_init_io_extension()
{
    if (!sp7_7b_i2c_begin()) {
        return false;
    }
    if (!sp7_7b_write_reg(SP7_7B_IO_REG_MODE, 0xFF)) {
        return false;
    }
    sp7_7b_io_output_state = 0xFF;
    return sp7_7b_sync_outputs();
}

static inline bool sp7_7b_set_backlight_percent(int percent)
{
    if (!sp7_7b_i2c_begin()) {
        return false;
    }

    return sp7_7b_set_io(SP7_7B_IO_BACKLIGHT, percent > 0);
}

#define ESP_PANEL_BOARD_PRE_BEGIN_FUNCTION(p) \
    { \
        (void)p; \
        return sp7_7b_init_io_extension(); \
    }

#define ESP_PANEL_BOARD_LCD_PRE_BEGIN_FUNCTION(p) \
    {  \
        (void)p; \
        if (!sp7_7b_init_io_extension()) { \
            return false; \
        } \
        if (!sp7_7b_set_io(SP7_7B_IO_LCD_RST, false)) { \
            return false; \
        } \
        vTaskDelay(pdMS_TO_TICKS(10)); \
        if (!sp7_7b_set_io(SP7_7B_IO_LCD_RST, true)) { \
            return false; \
        } \
        vTaskDelay(pdMS_TO_TICKS(100)); \
        return true; \
    }

#define ESP_PANEL_BOARD_TOUCH_PRE_BEGIN_FUNCTION(p) \
    {  \
        (void)p; \
        constexpr gpio_num_t TP_INT = static_cast<gpio_num_t>(ESP_PANEL_BOARD_TOUCH_INT_IO); \
        gpio_config_t tp_int_config = { \
            .pin_bit_mask = BIT64(TP_INT), \
            .mode = GPIO_MODE_OUTPUT, \
            .pull_up_en = GPIO_PULLUP_DISABLE, \
            .pull_down_en = GPIO_PULLDOWN_DISABLE, \
            .intr_type = GPIO_INTR_DISABLE, \
        }; \
        if (!sp7_7b_init_io_extension()) { \
            return false; \
        } \
        if (gpio_config(&tp_int_config) != ESP_OK) { \
            return false; \
        } \
        if (!sp7_7b_set_io(SP7_7B_IO_TOUCH_RST, false)) { \
            return false; \
        } \
        vTaskDelay(pdMS_TO_TICKS(100)); \
        if (gpio_set_level(TP_INT, 0) != ESP_OK) { \
            return false; \
        } \
        vTaskDelay(pdMS_TO_TICKS(100)); \
        if (!sp7_7b_set_io(SP7_7B_IO_TOUCH_RST, true)) { \
            return false; \
        } \
        vTaskDelay(pdMS_TO_TICKS(200)); \
        gpio_reset_pin(TP_INT); \
        return true; \
    }

#define ESP_PANEL_BOARD_BACKLIGHT_CUSTOM_FUNCTION(percent, user_data) \
    { \
        (void)user_data; \
        return sp7_7b_set_backlight_percent(percent); \
    }

#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR 2
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH 0

#endif

// *INDENT-ON*
