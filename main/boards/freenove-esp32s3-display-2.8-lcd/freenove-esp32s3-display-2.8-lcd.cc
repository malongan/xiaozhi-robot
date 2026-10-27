#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <esp_sleep.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include <dirent.h>
#include <cstdio>

#include <wifi_station.h>
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "freenove_power_monitor.h"
#if CONFIG_FEATURE_ENGLISH_DICTATION
#include "features/english_dictation/english_dictation_tools.h"
#include "features/english_dictation/english_tool.h"
#endif
#if CONFIG_FEATURE_STROKE_ORDER
#include "features/stroke_order/stroke_order_tools.h"
#include "features/stroke_order/stroke_tool.h"
#endif
#if CONFIG_FEATURE_CHAT_HISTORY
#include "features/chat_history/chat_log.h"
#endif
#include "display/lcd_display.h"
#include "power_save_timer.h"
#include "settings.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <driver/usb_serial_jtag.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_lcd_ili9341.h"
#include "led/single_led.h"
#include "system_reset.h"

#define TAG "FreenoveESP32S3Display"

class FreenoveESP32S3Display : public WifiBoard {
private:
    /* BOOT 长按关机后，下次开机的音量。关机时不再用 SetOutputVolume(0) 静音：
       它会把 NVS 里的 output_volume 一起写成 0，而 AudioCodec::Start() 读到
       <=0 会走「太小」兜底改成 10，等于关一次机音量就掉下去。 */
    static constexpr int kBootVolumeAfterPowerOff = 80;

    Button boot_button_;
    LcdDisplay* display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_handle_ = nullptr;
    FreenovePowerMonitor* power_monitor_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_timer_handle_t sd_retry_timer_ = nullptr;
    /* 关机流程需要直接操作屏幕与 SD：持有句柄才能发 SLPIN、卸载文件系统。 */
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    sdmmc_card_t* sd_card_ = nullptr;
    /* 轮询 LVGL 输入空闲时间：触摸屏幕即退出省电并重置空闲计时，
       否则进省电后背光一直暗着，只能靠按键或重启恢复。 */
    esp_timer_handle_t touch_wake_timer_ = nullptr;
    /* 档 2（深省电）期间的存活指示：每 10 秒让 WS2812 极暗闪一下。
       屏幕此时全黑，没有这点微光就分不清「在待机」还是「已关机」。 */
    esp_timer_handle_t led_pulse_timer_ = nullptr;
    esp_timer_handle_t led_pulse_off_timer_ = nullptr;

    bool MountSdSdmmc() {
        esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 10,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        // Keep the 20 MHz default: 40 MHz high-speed mode measured far slower
        // on this board (link/trace dependent), so do not raise it here.
        // High speed (40 MHz)：默认档只有 20 MHz，词库/音乐/聊天记录的读写都受限。
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
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
        sd_card_ = card;
        ESP_LOGI(TAG, "TF 卡 SDMMC 挂载成功");
        DIR* dir = opendir(SD_MOUNT_POINT);
        if (dir != nullptr) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                ESP_LOGI(TAG, "  /sdcard/%s", ent->d_name);
            }
            closedir(dir);
        } else {
            ESP_LOGW(TAG, "opendir(%s) 失败 errno=%d", SD_MOUNT_POINT, errno);
        }
        return true;
    }

    bool MountSdSdspi() {
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
            .max_files = 10,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        // High speed (40 MHz)：默认档只有 20 MHz，词库/音乐/聊天记录的读写都受限。
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
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
        sd_card_ = card;
        ESP_LOGI(TAG, "TF 卡 SDSPI 模式挂载成功");
        return true;
    }

#if CONFIG_FEATURE_ENGLISH_DICTATION
    // 音频自检每次 fopen 在 SD 卡上要 ~85ms，放到后台避免拖慢启动
    static void DeferredAudioCheck(void*) {
        vTaskDelay(pdMS_TO_TICKS(8000));
        EnglishStore::GetInstance().CheckAudio(20);
        vTaskDelete(nullptr);
    }

    static void ScheduleAudioCheck() {
        xTaskCreate(DeferredAudioCheck, "eng_chk", 4608, nullptr, 1, nullptr);
    }
#endif

    bool MountSdCard() { return MountSdSdmmc() || MountSdSdspi(); }

    static void OnSdRetry(void* arg) {
        auto* board = static_cast<FreenoveESP32S3Display*>(arg);
        if (board->MountSdCard()) {
            board->StopSdRetryTimer();
#if CONFIG_FEATURE_STROKE_ORDER
            StrokeStore::GetInstance().Init();
#endif
#if CONFIG_FEATURE_ENGLISH_DICTATION
            EnglishStore::GetInstance().Init();
            ScheduleAudioCheck();
#endif
#if CONFIG_FEATURE_CHAT_HISTORY
            ChatLog::GetInstance().Init();
#endif
            ESP_LOGI(TAG, "TF 卡热插拔挂载成功");
        }
    }

    void StopSdRetryTimer() {
        if (sd_retry_timer_ != nullptr) {
            esp_timer_stop(sd_retry_timer_);
        }
    }

    void InitializeSdCard() {
        if (MountSdCard()) {
#if CONFIG_FEATURE_STROKE_ORDER
            StrokeStore::GetInstance().Init();
#endif
#if CONFIG_FEATURE_ENGLISH_DICTATION
            EnglishStore::GetInstance().Init();
            ScheduleAudioCheck();
#endif
#if CONFIG_FEATURE_CHAT_HISTORY
            ChatLog::GetInstance().Init();
#endif
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
        // 板载 TP4054 的 CHRG 没有引到 ESP32，默认用电压变化推断充电状态；
        // 若把 CHRG 焊到空闲脚（IO2/IO3/IO14/IO21），把引脚填在这里即可直读。
        power_monitor_ = new FreenovePowerMonitor(BATTERY_CHARGING_DETECT_PIN);
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

    // 软件关机：先尽量关断外设，再进 deep sleep（BOOT 键低电平唤醒）。
    // deep sleep 只能把 ESP32 本体降到 µA 级，屏幕、codec、SD 卡、指示灯仍连着
    // 电源在吃电（整机约 1~3mA），所以关机前主动关：音频输出/PA、背光、WS2812、
    // 屏幕 SLPIN、SD 卸载。做完落在百 µA 量级，电池能放几个月。
    // 注意：插着 USB 时供电路径仍在，真正断电仍需拔线或拔电池。
    //
    // wait_button_release：长按 BOOT 触发时必须等手松开，否则低电平唤醒条件
    // 立刻满足，刚睡下就被自己叫醒；定时自动休眠没人按键，不必等。
    void PowerOff(bool wait_button_release = true) {
        ESP_LOGW(TAG, "Power off (manual=%d)", wait_button_release ? 1 : 0);
        if (wait_button_release) {
            if (display_ != nullptr) {
                display_->ShowNotification("正在关机", 1200);
            }
            // 留时间把提示画出来，再开始关外设
            vTaskDelay(pdMS_TO_TICKS(900));
        } else {
            /* 自动休眠时屏幕是全黑的，提示画了也看不见，让蓝色呼吸先播完即可 */
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (wait_button_release) {
            // 关键：必须等按键松开再睡，否则低电平唤醒条件立刻满足，
            // 刚进 deep sleep 就被唤醒，看起来像重启。
            for (int i = 0; i < 100 && gpio_get_level(BOOT_BUTTON_GPIO) == 0; ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            ESP_LOGW(TAG, "BOOT still held, skip power off");
            return;
        }

        // 1) 音频：先关 PA 输出与麦克风输入（此刻无声）。
        //    下次开机的音量直接落 NVS，刻意不走 SetOutputVolume：
        //    唤醒词任务此刻仍在 ReadAudioData()，发现麦被关掉会立刻
        //    EnableInput(true) 把 codec 重新打开（dev_ 重建），那时
        //    SetOutputVolume 就会真的推寄存器。写 NVS 语义明确且不碰硬件。
        if (auto* codec = GetAudioCodec(); codec != nullptr) {
            codec->EnableOutput(false);
            codec->EnableInput(false);
        }
        Settings audio_settings("audio", true);
        audio_settings.SetInt("output_volume", kBootVolumeAfterPowerOff);
        ESP_LOGI(TAG, "Next boot volume: %d", kBootVolumeAfterPowerOff);
        // 2) 背光与指示灯
        GetBacklight()->SetBrightness(0);
        if (auto* led = static_cast<SingleLed*>(GetLed()); led != nullptr) {
            led->TurnOff();
        }
        // 3) 屏幕：DISPOFF(0x28) + SLPIN(0x10)。下次开机驱动 init 会重新发
        //    SLPOUT，不用在这里考虑唤醒后的恢复。
        if (panel_io_ != nullptr) {
            esp_lcd_panel_io_tx_param(panel_io_, 0x28, nullptr, 0);
            esp_lcd_panel_io_tx_param(panel_io_, 0x10, nullptr, 0);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        // 4) SD 卡：停掉重试定时器并卸载文件系统，让卡进 idle 减少漏电
        if (sd_retry_timer_ != nullptr) {
            esp_timer_stop(sd_retry_timer_);
        }
        if (sd_card_ != nullptr) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card_);
            sd_card_ = nullptr;
        }

        // ext0 走 RTC IO，RTC 外设域必须保持供电，否则按键唤醒失效
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        esp_err_t err = esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ext0 wakeup on BOOT failed: %s", esp_err_to_name(err));
            esp_restart();
            return;
        }
        esp_deep_sleep_start();
    }

    /* 是否由外部（USB）供电。插着线时不做档 2，也不自动深度休眠：
       档 2 会让屏幕全黑，自动休眠则会让 USB 掉线、必须物理按键才能重新烧录。
       这台板没有 VBUS 检测，只能按可靠性分三层推断：
         1) USB Serial/JTAG 与主机枚举成功 —— 插在电脑上必定命中；
         2) 没装电池 —— 只可能靠 USB 供电；
         3) 正在充电 —— TP4054 正在从 USB 拉电流。
       注意边界：插充电器且电池已充满时三条都不命中（CHRG 没引到 GPIO、
       电压也不再上升），此时仍会进档 2。要彻底解决只能把 CHRG 飞线到 IO2/IO3/IO14/IO21。 */
    bool IsExternalPower() const {
        if (usb_serial_jtag_is_driver_installed() && usb_serial_jtag_is_connected()) {
            return true;
        }
        if (power_monitor_ == nullptr) {
            return false;
        }
        if (!power_monitor_->IsBatteryPresent()) {
            return true;
        }
        return power_monitor_->IsCharging();
    }

    /* Wi-Fi 省电策略。档 2 用 MAX_MODEM，只在 DTIM 间隔醒来收包，比长连接省
       十几 mA；恢复回 MIN_MODEM（IDF 默认档），避免对话/播放时因为睡得太深卡顿。
       未连上 Wi-Fi 时调用会返回 NOT_INIT，忽略即可。 */
    void SetWifiPowerSave(bool deep) {
        const wifi_ps_type_t mode = deep ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
        esp_err_t err = esp_wifi_set_ps(mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(%d) failed: %s", static_cast<int>(mode),
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Wi-Fi power save -> %s", deep ? "MAX_MODEM" : "MIN_MODEM");
        }
    }

    /* 档 2 的存活指示：屏幕全黑时每 10 秒让 WS2812 极暗闪一下，
       否则用户分不清「设备在待机」和「已经关机了」。 */
    void StartDeepSaveIndication() {
        auto* led = static_cast<SingleLed*>(GetLed());
        if (led == nullptr) {
            return;
        }
        led->TurnOff();

        if (led_pulse_off_timer_ == nullptr) {
            esp_timer_create_args_t off_args = {
                .callback = [](void* arg) {
                    auto self = static_cast<FreenoveESP32S3Display*>(arg);
                    if (auto* l = static_cast<SingleLed*>(self->GetLed()); l != nullptr) {
                        l->TurnOff();
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "led_pulse_off",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&off_args, &led_pulse_off_timer_) != ESP_OK) {
                led_pulse_off_timer_ = nullptr;
            }
        }

        if (led_pulse_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback = [](void* arg) {
                    auto self = static_cast<FreenoveESP32S3Display*>(arg);
                    if (auto* l = static_cast<SingleLed*>(self->GetLed()); l != nullptr) {
                        l->SetRgb(0, 0, 6);
                        if (self->led_pulse_off_timer_ != nullptr) {
                            esp_timer_start_once(self->led_pulse_off_timer_, 250 * 1000);
                        }
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "led_pulse",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &led_pulse_timer_) != ESP_OK) {
                led_pulse_timer_ = nullptr;
            }
        }

        if (led_pulse_timer_ != nullptr) {
            esp_timer_start_periodic(led_pulse_timer_, 10LL * 1000 * 1000);
            ESP_LOGI(TAG, "Deep save indicator on");
        }
    }

    void StopDeepSaveIndication() {
        if (led_pulse_timer_ != nullptr) {
            esp_timer_stop(led_pulse_timer_);
        }
        if (led_pulse_off_timer_ != nullptr) {
            esp_timer_stop(led_pulse_off_timer_);
        }
        // 交还给按设备状态驱动的常态灯效
        if (auto* led = GetLed(); led != nullptr) {
            led->OnStateChanged();
        }
    }

    /* 睡前蓝呼吸一次：此刻屏幕已黑，用指示灯「呼一口气」告诉用户它要睡了。 */
    void BreatheLedBlue() {
        auto* led = static_cast<SingleLed*>(GetLed());
        if (led == nullptr) {
            return;
        }
        constexpr int kSteps = 16;
        constexpr int kStepMs = 35;
        constexpr uint8_t kPeak = 64;
        for (int i = 0; i <= kSteps; ++i) {
            led->SetRgb(0, 0, static_cast<uint8_t>(kPeak * i / kSteps));
            vTaskDelay(pdMS_TO_TICKS(kStepMs));
        }
        for (int i = kSteps; i >= 0; --i) {
            led->SetRgb(0, 0, static_cast<uint8_t>(kPeak * i / kSteps));
            vTaskDelay(pdMS_TO_TICKS(kStepMs));
        }
        led->TurnOff();
    }

    void InitializePowerSaveTimer() {
        if (power_save_timer_ != nullptr) {
            return;
        }
        Settings power_settings("power", false);
        if (!power_settings.GetBool("power_save_en", true)) {
            ESP_LOGI(TAG, "Idle power save disabled by settings");
            return;
        }
        const int idle_seconds = static_cast<int>(power_settings.GetInt("idle_seconds", 60));
        const int deep_save_seconds =
            static_cast<int>(power_settings.GetInt("deep_save_sec", 3600));
        const int auto_sleep_seconds =
            static_cast<int>(power_settings.GetInt("auto_sleep_sec", 4 * 3600));
        /* cpu_max_freq 240 保持满频上限，空闲时由 PM 自动降到 40MHz。
           allow_light_sleep=false：不真睡，Wi-Fi 长连接不掉；
           keep_wake_word=true：省电时仍保持唤醒词和麦克风，喊一声就能醒，
           只靠降背光和降频省电。
           auto_sleep_seconds 传 0 表示关闭自动休眠。 */
        power_save_timer_ = new PowerSaveTimer(240, idle_seconds, auto_sleep_seconds, false, true,
                                               deep_save_seconds);
        power_save_timer_->OnEnterSleepMode([this]() {
            Settings settings("power", false);
            const uint8_t standby =
                static_cast<uint8_t>(settings.GetInt("standby_bright", 10));
            GetBacklight()->SetBrightness(standby, false);
            ESP_LOGI(TAG, "Idle power save: backlight -> %u%%", standby);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetBacklight()->RestoreBrightness();
            ESP_LOGI(TAG, "Idle power save: backlight restored");
        });
        /* 档 2：背光归零 + Wi-Fi 进 modem sleep，麦克风与唤醒词照常工作。
           屏幕虽然黑了，喊一声「你好小智」依然能唤醒。 */
        power_save_timer_->OnEnterDeepSaveMode([this]() {
            GetBacklight()->SetBrightness(0, false);
            StartDeepSaveIndication();
            SetWifiPowerSave(true);
            ESP_LOGI(TAG, "Deep save: backlight off, wake word kept");
        });
        power_save_timer_->OnExitDeepSaveMode([this]() {
            StopDeepSaveIndication();
            SetWifiPowerSave(false);
            /* 只退档 2（换电/插线）时不会走档 1 的恢复回调，这里补上待机亮度；
               真正的整机唤醒会紧接着由档 1 的 OnExitSleepMode 恢复满亮度。 */
            if (power_save_timer_ != nullptr && power_save_timer_->in_sleep_mode()) {
                Settings settings("power", false);
                GetBacklight()->SetBrightness(
                    static_cast<uint8_t>(settings.GetInt("standby_bright", 10)), false);
            }
            ESP_LOGI(TAG, "Deep save: exited");
        });
        /* 插着 USB 时封住档 2 与自动休眠，只保留档 1。 */
        power_save_timer_->SetDeepSaveAllowed([this]() { return !IsExternalPower(); });
        /* 长时间无交互 -> 真·深度休眠。PowerOff 里有秒级延时并最终 deep sleep，
           直接在 esp_timer 回调里做会堵住定时器服务任务，改开独立任务。 */
        power_save_timer_->OnShutdownRequest([this]() {
            BaseType_t ok = xTaskCreate(
                [](void* arg) {
                    auto self = static_cast<FreenoveESP32S3Display*>(arg);
                    self->BreatheLedBlue();
                    self->PowerOff(false);
                    vTaskDelete(nullptr);
                },
                "auto_sleep", 4096, this, 3, nullptr);
            if (ok != pdPASS) {
                ESP_LOGW(TAG, "auto sleep task create failed");
            }
        });
        power_save_timer_->SetEnabled(true);

        /* 每 500ms 看一次 LVGL 输入空闲时间，触摸后立即恢复背光并重置计时。 */
        if (touch_wake_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback = [](void* arg) {
                    auto self = static_cast<FreenoveESP32S3Display*>(arg);
                    if (self == nullptr || self->power_save_timer_ == nullptr) {
                        return;
                    }
                    if (!lvgl_port_lock(50)) {
                        return;
                    }
                    const uint32_t inactive_ms = lv_display_get_inactive_time(nullptr);
                    lvgl_port_unlock();
                    if (inactive_ms < 1200) {
                        self->power_save_timer_->WakeUp();
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "touch_wake",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &touch_wake_timer_) == ESP_OK) {
                esp_timer_start_periodic(touch_wake_timer_, 500000);
            } else {
                ESP_LOGW(TAG, "failed to create touch wake timer");
            }
        }
        ESP_LOGI(TAG, "Idle power save: light=%ds deep=%ds auto_sleep=%ds", idle_seconds,
                 deep_save_seconds, auto_sleep_seconds);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() { PowerOff(); });
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
        panel_io_ = panel_io;

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
#if CONFIG_FEATURE_STROKE_ORDER
        RegisterStrokeOrderTools(lcd);
#endif
#if CONFIG_FEATURE_ENGLISH_DICTATION
        RegisterEnglishDictationTools(lcd);
#endif
    }

public:
    // BOOT 键长按 1.5s 识别为关机（单击仍是原来切换对话/配网）
    FreenoveESP32S3Display() : boot_button_(BOOT_BUTTON_GPIO, false, 1500, 0) {
        // This build uses the dark UI (white glowing avatar on black).
        // Apply it once so a later manual theme change still persists.
        {
            Settings display_settings("display", true);
            if (!display_settings.GetBool("dark_default", false)) {
                display_settings.SetString("theme", "dark");
                display_settings.SetBool("dark_default", true);
            }
        }
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
            ESP_LOGW(TAG, "wake up from power-off (BOOT button)");
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
        InitializePowerSaveTimer();
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
        if (power_monitor_ == nullptr || !power_monitor_->IsBatteryPresent()) {
            return false;  // 没接电池时不显示电池图标
        }
        charging = power_monitor_->IsCharging();
        discharging = !charging;
        level = power_monitor_->GetBatteryLevel();
        return true;
    }

    /* 设置页改过四个省电项之后调一次。PowerSaveTimer 在构造时按值保存了三个档位，
       不推给它的话新设定要等重启才生效。开机时省电被关掉的情况 timer 是空的，
       这里按需补建。 */
    virtual void ApplyPowerSaveSettings() override {
        Settings power_settings("power", false);
        if (power_save_timer_ == nullptr) {
            /* 只有「用户刚把省电打开」才会走到这里；Initialize 内部会再读一次 NVS。 */
            if (power_settings.GetBool("power_save_en", true)) {
                InitializePowerSaveTimer();
            }
            return;
        }

        const bool enabled = power_settings.GetBool("power_save_en", true);
        const int idle_seconds = static_cast<int>(power_settings.GetInt("idle_seconds", 60));
        const int deep_save_seconds = static_cast<int>(power_settings.GetInt("deep_save_sec", 3600));
        const int auto_sleep_seconds =
            static_cast<int>(power_settings.GetInt("auto_sleep_sec", 4 * 3600));

        power_save_timer_->SetIntervals(idle_seconds, deep_save_seconds, auto_sleep_seconds);
        power_save_timer_->SetEnabled(enabled);

        /* 正停在档 1 时调「待机亮度」，得当场亮暗变化才看得出改没改。 */
        if (power_save_timer_->in_sleep_mode()) {
            const uint8_t standby =
                static_cast<uint8_t>(power_settings.GetInt("standby_bright", 10));
            GetBacklight()->SetBrightness(standby, false);
        }
        ESP_LOGI(TAG, "Power settings applied: light=%ds deep=%ds auto_sleep=%ds enabled=%d",
                 idle_seconds, deep_save_seconds, auto_sleep_seconds, enabled ? 1 : 0);
    }
};

DECLARE_BOARD(FreenoveESP32S3Display);
