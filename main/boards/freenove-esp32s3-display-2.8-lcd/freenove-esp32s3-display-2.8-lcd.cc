#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include <cstdio>

#include <wifi_station.h>
#include "adc_battery_monitor.h"
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/stroke_tool.h"
#include "mcp_server.h"
#include "settings.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_lcd_ili9341.h"
#include "led/single_led.h"
#include "system_reset.h"

#define TAG "FreenoveESP32S3Display"

class FreenoveESP32S3Display : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_handle_ = nullptr;
    AdcBatteryMonitor* adc_battery_monitor_;
    esp_timer_handle_t sd_retry_timer_ = nullptr;

    static bool MountSdSdmmc() {
        esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = SD_CLK;
        slot.cmd = SD_CMD;
        slot.d0 = SD_DATA0;
        slot.d1 = SD_DATA1;
        slot.d2 = SD_DATA2;
        slot.d3 = SD_DATA3;
        slot.width = 4;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        sdmmc_card_t* card = nullptr;
        esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SDMMC 4-bit 挂载失败: %s", esp_err_to_name(err));
            return false;
        }
        sdmmc_card_print_info(stdout, card);
        return true;
    }

    static bool MountSdSdspi() {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_CMD,
            .miso_io_num = SD_DATA0,
            .sclk_io_num = SD_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 400000,
        };
        if (spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
            ESP_LOGW(TAG, "SDSPI 总线初始化失败");
            return false;
        }
        esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = SPI2_HOST;
        slot.gpio_cs = SD_DATA3;
        slot.gpio_cd = SDSPI_SLOT_NO_CD;
        slot.gpio_wp = GPIO_NUM_NC;
        slot.gpio_int = GPIO_NUM_NC;
        sdmmc_card_t* card = nullptr;
        esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SDSPI 挂载失败: %s", esp_err_to_name(err));
            spi_bus_free(SPI2_HOST);
            return false;
        }
        ESP_LOGI(TAG, "TF 卡 SDSPI 模式挂载成功");
        return true;
    }

    bool MountSdCard() { return MountSdSdmmc() || MountSdSdspi(); }

    static void OnSdRetry(void* arg) {
        auto* board = static_cast<FreenoveESP32S3Display*>(arg);
        if (board->MountSdCard()) {
            board->StopSdRetryTimer();
            StrokeStore::GetInstance().Init();
            ESP_LOGI(TAG, "TF 卡热插拔挂载成功，笔顺数据已加载");
        }
    }

    void StopSdRetryTimer() {
        if (sd_retry_timer_ != nullptr) {
            esp_timer_stop(sd_retry_timer_);
        }
    }

    void InitializeSdCard() {
        if (MountSdCard()) {
            StrokeStore::GetInstance().Init();
            return;
        }
        esp_timer_create_args_t timer_args = {
            .callback = &FreenoveESP32S3Display::OnSdRetry,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sd_retry",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&timer_args, &sd_retry_timer_) == ESP_OK) {
            esp_timer_start_periodic(sd_retry_timer_, 60ULL * 1000 * 1000);
            ESP_LOGW(TAG, "TF 卡未挂载，60s 后台重试中");
        }
    }

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ =
            new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    void InitializeTouch() {
        // The panel is rotated to landscape (DISPLAY_SWAP_XY), so the raw
        // portrait touch coordinates are swapped by the LVGL touch port.
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = 0x38;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.flags.disable_control_phase = 1;
        tp_io_config.scl_speed_hz = 400 * 1000;

        esp_err_t ret = esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &touch_io_handle_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create touch panel IO: %s", esp_err_to_name(ret));
            return;
        }

        // esp_lcd_touch mirrors the raw portrait axes BEFORE swapping them,
        // so x_max/y_max must stay 240/320 (raw) for mirroring to be correct.
        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = 240;
        tp_cfg.y_max = 320;
        tp_cfg.rst_gpio_num = GPIO_NUM_NC;
        tp_cfg.int_gpio_num = GPIO_NUM_NC;
        tp_cfg.levels.reset = 0;
        tp_cfg.levels.interrupt = 0;
        tp_cfg.flags.swap_xy = DISPLAY_SWAP_XY;
        // Portrait panel: the raw X axis already matches the visible layout,
        // mirroring it swapped left/right taps (seen as back/bluetooth swapped).
        tp_cfg.flags.mirror_x = false;
        tp_cfg.flags.mirror_y = false;

        ret = esp_lcd_touch_new_i2c_ft5x06(touch_io_handle_, &tp_cfg, &touch_handle_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize FT6236 touch: %s", esp_err_to_name(ret));
            return;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = touch_handle_,
        };
        if (touch_cfg.disp == nullptr) {
            ESP_LOGE(TAG, "Touch display is not initialized");
            return;
        }
        if (lvgl_port_add_touch(&touch_cfg) == nullptr) {
            ESP_LOGE(TAG, "Failed to add touch to LVGL");
            return;
        }
        ESP_LOGI(TAG, "FT6236 touch registered with LVGL");
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MIS0_PIN;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "Install LCD driver ILI9341");
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, true);
    }

    void InitializeTools() {
        auto* lcd = static_cast<LcdDisplay*>(GetDisplay());
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool(
            "self.stroke.show",
            "在屏幕上演示一个汉字的笔顺：逐笔动画 + 每笔名称 + 拼音，仅支持小学 1-6 年级字表内的"
            "字。传入要查询的汉字或包含它的句子，取其中最后一个汉字。",
            PropertyList({Property("char", kPropertyTypeString)}),
            [lcd](const PropertyList& properties) -> ToolResult {
                auto& store = StrokeStore::GetInstance();
                if (!store.Ready()) {
                    return std::string("字库还没准备好，请确认 TF 卡已插入。");
                }
                const std::string input = properties["char"].value<std::string>();
                const int cp = StrokeStore::Utf8SingleCjk(input);
                const int idx = cp >= 0 ? store.FindByChar(static_cast<uint16_t>(cp)) : -1;
                if (idx < 0) {
                    return std::string("这个字不在小学字表里，换一个 1-6 年级的常用字试试。");
                }
                std::string ch;
                ch += static_cast<char>(0xE0 | (cp >> 12));
                ch += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                ch += static_cast<char>(0x80 | (cp & 0x3F));
                Application::GetInstance().Schedule([lcd, idx]() { lcd->ShowStrokeScreen(idx); });
                return "正在屏幕上演示「" + ch + "」的笔顺：拼音 " + store.PinyinOf(idx) + "，共 " +
                       std::to_string(store.StrokeCountAt(idx)) + " 笔——" +
                       store.StrokeNamesText(idx) + "。动画播完会自动跳到下一个字。";
            });

        mcp.AddTool(
            "self.stroke.next",
            "在笔顺演示页翻字：dir 为 -1 切到上一个字，1 切到下一个字，同年级内循环。",
            PropertyList({Property("dir", kPropertyTypeInteger, -1, 1)}),
            [lcd](const PropertyList& properties) -> ToolResult {
                auto& store = StrokeStore::GetInstance();
                if (!store.Ready()) {
                    return std::string("字库还没准备好，请确认 TF 卡已插入。");
                }
                const int dir = properties["dir"].value<int>() < 0 ? -1 : 1;
                Application::GetInstance().Schedule([lcd, dir]() { lcd->StrokeChangeChar(dir); });
                return dir > 0 ? std::string("已切换到下一个字。")
                               : std::string("已切换到上一个字。");
            });

        mcp.AddTool(
            "self.stroke.close", "关闭笔顺演示页，回到正常聊天界面。用户说关掉或退出笔顺时调用。",
            PropertyList(), [lcd](const PropertyList&) -> ToolResult {
                Application::GetInstance().Schedule([lcd]() { lcd->ShowStrokeScreen(false); });
                return std::string("已关闭笔顺演示页。");
            });
    }

public:
    FreenoveESP32S3Display() : boot_button_(BOOT_BUTTON_GPIO) {
        // This build uses the dark UI (white glowing avatar on black).
        // Apply it once so a later manual theme change still persists.
        {
            Settings display_settings("display", true);
            if (!display_settings.GetBool("dark_default", false)) {
                display_settings.SetString("theme", "dark");
                display_settings.SetBool("dark_default", true);
            }
        }
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSdCard();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->SetBrightness(100);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, AUDIO_CODEC_I2C_NUM, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(FreenoveESP32S3Display);
