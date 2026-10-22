#include "lcd_display.h"
#include <esp_system.h>
#include <wifi_manager.h>
#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#if CONFIG_FEATURE_ENGLISH_DICTATION
#include "features/english_dictation/english_player.h"
#include "features/english_dictation/english_tool.h"
#endif
#include "gif/lvgl_gif.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#if CONFIG_FEATURE_BLE_KEYBOARD
#include "features/ble_keyboard/media_ble.h"
#endif
#include "wifi_board.h"
#if CONFIG_FEATURE_WEB_FILE_TRANSFER
#include "features/web_file_transfer/web_file_server.h"
#endif

LV_FONT_DECLARE(font_zzz_30);
#if CONFIG_FEATURE_STROKE_ORDER
LV_FONT_DECLARE(font_pinyin_20);
#endif
LV_FONT_DECLARE(font_english_20);
LV_FONT_DECLARE(ui_font_16_4);

#include <cstdio>
#include <functional>
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sys/statvfs.h>
#include <vector>

#include "backlight.h"
#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

namespace {
constexpr uint32_t kPaperPage = 0xF0E0BC;
constexpr uint32_t kPaperSheet = 0xFAF0D8;
constexpr uint32_t kPaperEdge = 0xD3BE93;
constexpr uint32_t kPaperGrid = 0xE4D2AC;
constexpr uint32_t kPaperInk = 0x4A3423;
constexpr uint32_t kPaperGuide = 0xE0AEA1;
constexpr uint32_t kSkyBgTop = 0xEAF5FF;
constexpr uint32_t kSkyBgBottom = 0xD3E9FB;
constexpr uint32_t kSkyCard = 0xFFFFFF;
constexpr uint32_t kSkyBorder = 0xC9DEF2;
constexpr uint32_t kSkyInk = 0x14324F;
constexpr uint32_t kSkyMuted = 0x5B7A99;
constexpr uint32_t kSkyAccent = 0x3E8ED0;
constexpr uint32_t kSkyAccentDeep = 0x2B6FAE;
constexpr uint32_t kSkySelect = 0xBFE0FA;
constexpr uint32_t kSkySelectEdge = 0x7FB6E6;
constexpr uint32_t kSkyGood = 0x3E9B6B;
constexpr uint32_t kDarkTeal = 0x2FB6A8;
constexpr uint32_t kDarkTealDeep = 0x1E8E86;
constexpr uint32_t kDarkTealInk = 0x04201D;
constexpr uint32_t kDarkCard = 0x14161D;
constexpr uint32_t kDarkCardControl = 0x242733;
constexpr uint32_t kDarkCardBorder = 0x262A36;
constexpr uint32_t kDarkDivider = 0x232733;
constexpr uint32_t kDarkBlue = 0x4A8FE8;
constexpr uint32_t kDarkOrange = 0xF0A03C;
constexpr uint32_t kDarkPurple = 0x8B6FE8;
constexpr uint32_t kDarkMuted = 0x939BAA;
constexpr uint32_t kDarkDim = 0x6D7585;
constexpr uint32_t kDarkGood = 0x4CD964;
constexpr uint32_t kDarkDanger = 0xE06C75;
// 内容列表每页行数；列表改成翻页后不再滚动，避免滑动与点选冲突。
constexpr int kEnglishSetupRowsPerPage = 6;
}  // namespace

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy, bool double_buffer)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        // Base TLSF pool is CONFIG_LV_MEM_SIZE_KILOBYTES in PSRAM; grow it so
        // the 2MB decoded-image cache does not exhaust a 2MB-PSRAM-safe pool.
        // TLSF rejects any single pool larger than LV_MEM_SIZE when
        // LV_MEM_POOL_EXPAND_SIZE is 0 (block_size_max = 1 << ceil(log2(LV_MEM_SIZE))).
        const size_t extra_total = 2560 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    // SPI LCD still needs an internal DMA bounce buffer for PSRAM sources.
    // A full-frame transfer (~150KB) cannot allocate that bounce buffer, so
    // keep a small DMA strip in internal SRAM and partial refresh.
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 24),
        .double_buffer = double_buffer,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 24),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

#if CONFIG_FEATURE_WEB_FILE_TRANSFER
    WebFileServer::GetInstance().SetStatusCallback(nullptr);
    WebFileServer::GetInstance().SetActivityCallback(nullptr);
#endif

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (avatar_anim_timer_ != nullptr) {
        lv_timer_delete(avatar_anim_timer_);
        avatar_anim_timer_ = nullptr;
    }

    if (stroke_anim_timer_ != nullptr) {
        lv_timer_delete(stroke_anim_timer_);
        stroke_anim_timer_ = nullptr;
    }
    if (drawer_hint_timer_ != nullptr) {
        lv_timer_delete(drawer_hint_timer_);
        drawer_hint_timer_ = nullptr;
    }
    if (stroke_canvas_buf_ != nullptr) {
        heap_caps_free(stroke_canvas_buf_);
        stroke_canvas_buf_ = nullptr;
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

const LcdDisplay::DrawerTileSpec LcdDisplay::kDrawerSpecs[LcdDisplay::kDrawerTileCount] = {
    {MATERIAL_SYMBOLS_SETTINGS, "设置", &LcdDisplay::OpenSettingsPage,
     &LcdDisplay::DrawerAlwaysEnabled, false, "", 0x5B6C8A, 0x3E4C63},
#if CONFIG_FEATURE_WEB_FILE_TRANSFER
    {MATERIAL_SYMBOLS_WIFI, "网络", &LcdDisplay::OpenFileServicePage,
     &LcdDisplay::DrawerAlwaysEnabled, false, "", 0x2FB6A8, 0x1E8E86},
#else
    {MATERIAL_SYMBOLS_WIFI, "网络", &LcdDisplay::OpenFileServicePage,
     &LcdDisplay::DrawerAlwaysEnabled, true, "网络文件传输未启用", 0x2FB6A8, 0x1E8E86},
#endif
    {MATERIAL_SYMBOLS_LANGUAGE, "听写", &LcdDisplay::OpenEnglishPage,
     &LcdDisplay::DrawerEnglishEnabled, false, "未找到词库，请插入存储卡", 0xF0A03C, 0xD97B22},
    {MATERIAL_SYMBOLS_EDIT_SQUARE, "笔画", &LcdDisplay::OpenStrokePage,
     &LcdDisplay::DrawerStrokeEnabled, false, "笔顺数据未就绪", 0x8B6FE8, 0x6A4FD0},
    {MATERIAL_SYMBOLS_BLUETOOTH, "键盘", &LcdDisplay::OpenMediaPage,
     &LcdDisplay::DrawerBleKeyboardEnabled, false, "蓝牙键盘未启用", 0x4A8FE8, 0x2E6BC8},
};

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
namespace {

constexpr size_t kMaxChatMessages = 40;
constexpr size_t kMaxChatTextBytes = 1000;
constexpr lv_coord_t kChatSidePad = 8;
constexpr lv_coord_t kAvatarSize = 80;
constexpr lv_coord_t kAvatarTopGap = 5;
constexpr lv_coord_t kAvatarGap = 10;
constexpr uint32_t kAvatarFaceColor = 0xF2F6FF;
constexpr uint32_t kAvatarEyeColor = 0x0B0B0D;
constexpr uint16_t kAvatarLongPressMs = 2000;
constexpr lv_coord_t kTopBarPad = 6;
constexpr lv_coord_t kTopBarIconGap = 4;

const char* RolePrefix(const std::string& role) {
    if (role == "user") {
        return "你：";
    }
    if (role == "assistant") {
        return "小智：";
    }
    return "系统：";
}

uint32_t Utf8CharLen(uint8_t lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead >> 5) == 0x6) {
        return 2;
    }
    if ((lead >> 4) == 0xE) {
        return 3;
    }
    if ((lead >> 3) == 0x1E) {
        return 4;
    }
    return 1;
}

uint32_t Utf8Decode(const std::string& text, size_t index) {
    const uint8_t lead = static_cast<uint8_t>(text[index]);
    const uint32_t len = Utf8CharLen(lead);
    uint32_t codepoint = 0;
    if (len == 1) {
        return lead;
    }
    if (len == 2) {
        codepoint = lead & 0x1F;
    } else if (len == 3) {
        codepoint = lead & 0x0F;
    } else {
        codepoint = lead & 0x07;
    }
    for (uint32_t i = 1; i < len && index + i < text.size(); ++i) {
        codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index + i]) & 0x3F);
    }
    return codepoint;
}

/* Word/character wrapping used for pagination. Lines are stored as byte
   offsets; pages are rendered with explicit line breaks, so this only has to
   produce lines that fit the viewport width. */
void WrapChatText(const std::string& text, const lv_font_t* font, lv_coord_t max_width,
                  std::vector<uint32_t>& line_starts) {
    line_starts.clear();
    if (font == nullptr || text.empty()) {
        return;
    }
    line_starts.push_back(0);
    lv_coord_t line_width = 0;
    size_t i = 0;
    while (i < text.size()) {
        const uint8_t lead = static_cast<uint8_t>(text[i]);
        const uint32_t len = Utf8CharLen(lead);
        if (i + len > text.size()) {
            break;  // Truncated UTF-8 sequence: stop here
        }
        const uint32_t codepoint = Utf8Decode(text, i);
        if (codepoint == '\n' || codepoint == '\r') {
            i += len;
            if (i < text.size()) {
                line_starts.push_back(static_cast<uint32_t>(i));
                line_width = 0;
            }
            continue;
        }
        uint32_t next_codepoint = 0;
        if (i + len < text.size()) {
            next_codepoint = Utf8Decode(text, i + len);
        }
        const lv_coord_t glyph_width = lv_font_get_glyph_width(font, codepoint, next_codepoint);
        if (line_width + glyph_width > max_width && i > line_starts.back()) {
            line_starts.push_back(static_cast<uint32_t>(i));
            line_width = 0;
        }
        line_width += glyph_width;
        i += len;
    }
}

/* Keep only the tail of an over-long message: the history is a live viewport,
   not an archive. Cuts on a UTF-8 boundary. */
void TrimChatText(std::string& text, size_t limit) {
    if (text.size() <= limit) {
        return;
    }
    size_t start = text.size() - limit;
    while (start < text.size() && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80) {
        ++start;
    }
    text.erase(0, start);
}

}  // namespace

void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);
    /* The screen is scrollable by default; with a fixed page layout an
       overflowing child would draw a scrollbar, so forbid it outright. */
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    ESP_LOGI(TAG, "Layout canvas: %dx%d", (int)LV_HOR_RES, (int)LV_VER_RES);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    /* Top bar (official layout): WiFi icon left, clock / conversation state in
       the middle, mute + battery right. LvglDisplay::UpdateStatusBar() drives
       all three icons and writes HH:MM into status_label_ while idle; during a
       conversation the same slot shows the device state instead. */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_CLICKABLE);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    mute_label_ = lv_label_create(top_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);  // no mic/mute icon in the status bar
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(top_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

    /* Center of the top bar: clock when idle, conversation state otherwise */
    status_label_ = lv_label_create(top_bar_);
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_scrollbar_mode(status_label_, LV_SCROLLBAR_MODE_OFF);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(top_bar_);
    lv_obj_set_style_text_font(notification_label_, text_font, 0);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_scrollbar_mode(notification_label_, LV_SCROLLBAR_MODE_OFF);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    /* 2 second press = new session (history cleared immediately) */
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_long_press_time(indev, kAvatarLongPressMs);
        }
    }

    /* Avatar: tap = continue chatting (history is kept), 2s press = new
       session (the screen clearing is the feedback) */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_style_radius(emoji_box_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_transform_pivot_x(emoji_box_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(emoji_box_, lv_pct(50), 0);
    lv_obj_set_style_clip_corner(emoji_box_, true, 0);
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(emoji_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(emoji_box_, 8);
    lv_obj_add_event_cb(emoji_box_, &LcdDisplay::OnAvatarPressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(emoji_box_, &LcdDisplay::OnAvatarReleased, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(emoji_box_, &LcdDisplay::OnAvatarReleased, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(emoji_box_, &LcdDisplay::OnAvatarLongPressed, LV_EVENT_LONG_PRESSED, this);
    lv_obj_add_event_cb(emoji_box_, &LcdDisplay::OnMicButtonClicked, LV_EVENT_CLICKED, this);

    /* Procedural round face: white circle with two pill eyes (dark UI). */
    avatar_face_ = lv_obj_create(emoji_box_);
    lv_obj_remove_flag(avatar_face_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(avatar_face_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(avatar_face_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(avatar_face_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(avatar_face_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar_face_, lv_color_hex(kAvatarFaceColor), 0);
    lv_obj_set_style_bg_opa(avatar_face_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(avatar_face_, 0, 0);
    lv_obj_set_style_pad_all(avatar_face_, 0, 0);
    lv_obj_set_style_clip_corner(avatar_face_, true, 0);

    auto create_pill_eye = [this]() {
        lv_obj_t* eye = lv_obj_create(avatar_face_);
        lv_obj_remove_flag(eye, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(eye, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(eye, 8, 22);
        lv_obj_set_style_radius(eye, 4, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(kAvatarEyeColor), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_set_style_transform_pivot_x(eye, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(eye, lv_pct(50), 0);
        lv_obj_align(eye, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t* hl = lv_obj_create(eye);
        lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(hl, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(hl, 3, 4);
        lv_obj_set_style_radius(hl, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(hl, lv_color_hex(0x4A4A52), 0);
        lv_obj_set_style_bg_opa(hl, LV_OPA_60, 0);
        lv_obj_set_style_border_width(hl, 0, 0);
        lv_obj_align(hl, LV_ALIGN_TOP_MID, 0, 2);
        return eye;
    };
    avatar_eye_left_ = create_pill_eye();
    avatar_eye_right_ = create_pill_eye();
    avatar_eye_hl_left_ = lv_obj_get_child(avatar_eye_left_, 0);
    avatar_eye_hl_right_ = lv_obj_get_child(avatar_eye_right_, 0);

    auto create_happy_eye = [this]() {
        lv_obj_t* arc = lv_arc_create(avatar_face_);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(arc, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(arc, 20, 20);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(arc, 0, 0);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(kAvatarEyeColor), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
        lv_arc_set_bg_angles(arc, 200, 340);
        lv_arc_set_angles(arc, 0, 0);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        return arc;
    };
    avatar_eye_happy_left_ = create_happy_eye();
    avatar_eye_happy_right_ = create_happy_eye();

    avatar_eye_happy_right_ = create_happy_eye();

    auto create_sleep_eye = [this]() {
        lv_obj_t* arc = lv_arc_create(avatar_face_);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(arc, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(arc, 20, 20);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(arc, 0, 0);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(kAvatarEyeColor), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
        lv_arc_set_bg_angles(arc, 20, 160);  // smile-shaped closed eye
        lv_arc_set_angles(arc, 0, 0);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        return arc;
    };

    avatar_think_dots_ = lv_label_create(screen);
    lv_obj_set_style_text_font(avatar_think_dots_, text_font, 0);
    lv_obj_set_style_text_color(avatar_think_dots_, lvgl_theme->text_color(), 0);
    lv_label_set_text(avatar_think_dots_, "···");
    lv_obj_add_flag(avatar_think_dots_, LV_OBJ_FLAG_HIDDEN);

    sleep_zzz_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sleep_zzz_, &font_zzz_30, 0);
    lv_obj_set_style_text_color(sleep_zzz_, lv_color_hex(0xD8D8E0), 0);
    lv_obj_set_style_text_opa(sleep_zzz_, LV_OPA_80, 0);
    lv_obj_set_style_transform_pivot_x(sleep_zzz_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(sleep_zzz_, lv_pct(50), 0);
    lv_label_set_text(sleep_zzz_, "Zzz");
    lv_obj_add_flag(sleep_zzz_, LV_OBJ_FLAG_HIDDEN);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_set_style_transform_scale(emoji_label_, 200, 0);
    lv_obj_set_style_transform_pivot_x(emoji_label_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(emoji_label_, lv_pct(50), 0);
    lv_obj_center(emoji_label_);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Long-press progress ring around the avatar */
    page_progress_ = lv_arc_create(screen);
    lv_obj_remove_flag(page_progress_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(page_progress_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(page_progress_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_progress_, 0, 0);
    lv_obj_set_style_arc_width(page_progress_, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(page_progress_, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_color(page_progress_, lvgl_theme->border_color(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(page_progress_, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(page_progress_, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(page_progress_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(page_progress_, 0, LV_PART_KNOB);
    lv_arc_set_range(page_progress_, 0, 100);
    lv_arc_set_value(page_progress_, 0);
    lv_obj_add_flag(page_progress_, LV_OBJ_FLAG_HIDDEN);

    /* Fixed five-line conversation viewport. Left half = previous page, right
       half = next page; the page content itself never scrolls. */
    chat_area_ = lv_obj_create(screen);
    lv_obj_set_style_bg_opa(chat_area_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_area_, 0, 0);
    lv_obj_set_style_pad_all(chat_area_, 0, 0);
    lv_obj_set_style_radius(chat_area_, 0, 0);
    lv_obj_remove_flag(chat_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(chat_area_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(chat_area_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chat_area_, &LcdDisplay::OnChatAreaClicked, LV_EVENT_CLICKED, this);

    chat_message_label_ = lv_label_create(chat_area_);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(chat_message_label_, text_font, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_pos(chat_message_label_, 0, 0);

    /* Middle layer: preview_image_ - centered display, above the chat area */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    /* Bottom media entry: line-art keyboard icon, shown only while idle. */
    media_bar_ = lv_obj_create(screen);
    lv_obj_set_size(media_bar_, 72, 44);
    lv_obj_set_style_bg_opa(media_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(media_bar_, 0, 0);
    lv_obj_set_style_pad_all(media_bar_, 0, 0);
    lv_obj_remove_flag(media_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(media_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(media_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(media_bar_, 4);
    lv_obj_set_ext_click_area(media_bar_, 12);
    lv_obj_add_event_cb(media_bar_, &LcdDisplay::OnDrawerHandleClicked, LV_EVENT_PRESSED, this);

    // Minimal media-entry glyph: two rows of four light-grey dots, no frame.
    lv_obj_t* keyboard = lv_obj_create(media_bar_);
    lv_obj_set_size(keyboard, 32, 20);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(keyboard, 0, 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    lv_obj_set_style_pad_all(keyboard, 0, 0);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(keyboard, LV_SCROLLBAR_MODE_OFF);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 4; ++col) {
            lv_obj_t* key = lv_obj_create(keyboard);
            lv_obj_set_size(key, 4, 4);
            lv_obj_set_pos(key, 2 + col * 8, 3 + row * 9);
            lv_obj_set_style_radius(key, 2, 0);
            lv_obj_set_style_bg_color(key, lv_color_hex(0xB8B8B8), 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(key, 0, 0);
            lv_obj_set_style_pad_all(key, 0, 0);
            lv_obj_remove_flag(key, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(key, LV_OBJ_FLAG_CLICKABLE);
        }
    }

#if CONFIG_FEATURE_BLE_KEYBOARD
    /* Media screen: full-screen overlay with six outline media keys. */
    media_screen_ = lv_obj_create(screen);
    lv_obj_set_size(media_screen_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(media_screen_, 0, 0);
    lv_obj_set_style_bg_color(media_screen_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(media_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(media_screen_, 0, 0);
    lv_obj_set_style_pad_all(media_screen_, 0, 0);
    lv_obj_remove_flag(media_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(media_screen_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(media_screen_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* media_back = lv_button_create(media_screen_);
    lv_obj_set_size(media_back, 44, 36);
    lv_obj_align(media_back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_opa(media_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(media_back, 0, 0);
    lv_obj_set_style_shadow_width(media_back, 0, 0);
    lv_obj_set_style_pad_all(media_back, 0, 0);
    lv_obj_set_ext_click_area(media_back, 8);
    lv_obj_add_event_cb(media_back, &LcdDisplay::OnMediaBackClicked, LV_EVENT_PRESSED, this);
    lv_obj_t* media_back_icon = lv_label_create(media_back);
    lv_obj_set_style_text_font(media_back_icon, icon_font, 0);
    lv_obj_set_style_text_color(media_back_icon, lvgl_theme->text_color(), 0);
    lv_label_set_text(media_back_icon, MATERIAL_SYMBOLS_ARROW_BACK);
    lv_obj_center(media_back_icon);

    media_title_ = lv_label_create(media_screen_);
    lv_obj_set_style_text_font(media_title_, text_font, 0);
    lv_obj_set_style_text_color(media_title_, lvgl_theme->text_color(), 0);
    lv_label_set_text(media_title_, "媒 体");
    lv_obj_align(media_title_, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t* bt_entry = lv_button_create(media_screen_);
    lv_obj_set_size(bt_entry, 44, 36);
    lv_obj_align(bt_entry, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_set_style_bg_opa(bt_entry, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bt_entry, 0, 0);
    lv_obj_set_style_shadow_width(bt_entry, 0, 0);
    lv_obj_set_style_pad_all(bt_entry, 0, 0);
    lv_obj_set_ext_click_area(bt_entry, 8);
    lv_obj_add_event_cb(bt_entry, &LcdDisplay::OnBluetoothEntryClicked, LV_EVENT_PRESSED, this);
    media_bt_icon_ = lv_label_create(bt_entry);
    lv_obj_set_style_text_font(media_bt_icon_, icon_font, 0);
    lv_obj_set_style_text_color(media_bt_icon_, lv_color_hex(0x777777), 0);
    lv_label_set_text(media_bt_icon_, MATERIAL_SYMBOLS_BLUETOOTH);
    lv_obj_center(media_bt_icon_);

    const char* media_icons[6] = {
        MATERIAL_SYMBOLS_SKIP_PREVIOUS, MATERIAL_SYMBOLS_PLAY_ARROW, MATERIAL_SYMBOLS_SKIP_NEXT,
        MATERIAL_SYMBOLS_VOLUME_DOWN,   MATERIAL_SYMBOLS_VOLUME_OFF, MATERIAL_SYMBOLS_VOLUME_UP,
    };
    static const char* const kShortcutLabels[10] = {"调度", "切换", "锁屏", "截图", "聚焦",
                                                    "全屏", "关闭", "桌面", "复制", "粘贴"};
    for (int i = 0; i < 16; ++i) {
        lv_obj_t* key = lv_button_create(media_screen_);
        media_keys_[i] = key;
        lv_obj_set_size(key, 52, 52);
        int row = 0;
        int col = 0;
        if (i < 6) {  // media block: 3 columns x 2 rows
            row = i / 3;
            col = i % 3;
        } else if (i == 6) {  // 调度 in the 4th column
            row = 0;
            col = 3;
        } else if (i == 7) {  // 切换
            row = 1;
            col = 3;
        } else {  // remaining shortcuts in two 4-wide rows
            row = 2 + (i - 8) / 4;
            col = (i - 8) % 4;
        }
        lv_obj_set_pos(key, 4 + col * 60, 46 + row * 66);
        lv_obj_set_style_radius(key, 12, 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(key, 2, 0);
        lv_obj_set_style_border_color(key, lvgl_theme->text_color(), 0);
        lv_obj_set_style_shadow_width(key, 0, 0);
        lv_obj_set_style_pad_all(key, 0, 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(key, lvgl_theme->text_color(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(key, &LcdDisplay::OnMediaKeyClicked, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(key, &LcdDisplay::OnMediaKeyClicked, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(key, &LcdDisplay::OnMediaKeyClicked, LV_EVENT_PRESS_LOST, this);
        lv_obj_t* label = lv_label_create(key);
        if (i < 6) {
            media_key_icons_[i] = label;
            lv_obj_set_style_text_font(label, large_icon_font, 0);
            lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
            lv_label_set_text(label, media_icons[i]);
        } else {
            if (shortcut_label_count_ < 10) {
                shortcut_labels_[shortcut_label_count_++] = label;
            }
            lv_label_set_text(label, kShortcutLabels[i - 6]);
        }
        lv_obj_center(label);
    }
    struct KeyDef {
        const char* label;
        uint8_t code;
        uint8_t mods;
        int16_t x;
        int16_t y;
        int16_t w;
        int16_t h;
        bool icon;
    };
    static const KeyDef kFullKeys[] = {
        {"A", 0x04, 0, 2, 44, 36, 37, false},   {"B", 0x05, 0, 42, 44, 36, 37, false},
        {"C", 0x06, 0, 82, 44, 36, 37, false},  {"D", 0x07, 0, 122, 44, 36, 37, false},
        {"E", 0x08, 0, 162, 44, 36, 37, false}, {"F", 0x09, 0, 202, 44, 36, 37, false},
        {"G", 0x0A, 0, 2, 83, 36, 37, false},   {"H", 0x0B, 0, 42, 83, 36, 37, false},
        {"I", 0x0C, 0, 82, 83, 36, 37, false},  {"J", 0x0D, 0, 122, 83, 36, 37, false},
        {"K", 0x0E, 0, 162, 83, 36, 37, false}, {"L", 0x0F, 0, 202, 83, 36, 37, false},
        {"M", 0x10, 0, 2, 122, 36, 37, false},  {"N", 0x11, 0, 42, 122, 36, 37, false},
        {"O", 0x12, 0, 82, 122, 36, 37, false}, {"P", 0x13, 0, 122, 122, 36, 37, false},
        {"Q", 0x14, 0, 162, 122, 36, 37, false}, {"R", 0x15, 0, 202, 122, 36, 37, false},
        {"S", 0x16, 0, 2, 161, 36, 37, false},  {"T", 0x17, 0, 42, 161, 36, 37, false},
        {"U", 0x18, 0, 82, 161, 36, 37, false}, {"V", 0x19, 0, 122, 161, 36, 37, false},
        {"W", 0x1A, 0, 162, 161, 36, 37, false}, {"X", 0x1B, 0, 202, 161, 36, 37, false},
        {"Y", 0x1C, 0, 2, 200, 36, 37, false},  {"Z", 0x1D, 0, 42, 200, 36, 37, false},
        {":", 0x33, 0x02, 82, 200, 36, 37, false}, {"@", 0x1F, 0x02, 122, 200, 36, 37, false},
        {",", 0x36, 0, 162, 200, 36, 37, false}, {".", 0x37, 0, 202, 200, 36, 37, false},
        {"?", 0x38, 0x02, 2, 239, 36, 37, false}, {"!", 0x1E, 0x02, 42, 239, 36, 37, false},
        {"'", 0x34, 0, 82, 239, 36, 37, false}, {"-", 0x2D, 0, 122, 239, 36, 37, false},
        {"/", 0x38, 0, 162, 239, 36, 37, false}, {";", 0x33, 0, 202, 239, 36, 37, false},
        {"SH", 0xFF, 0, 2, 278, 36, 37, false}, {"中", 0xFE, 0, 42, 278, 36, 37, false},
        {"BSP", 0x2A, 0, 82, 278, 36, 37, false}, {"SPC", 0x2C, 0, 122, 278, 36, 37, false},
        {"发送", 0x28, 0, 162, 278, 76, 37, false},
    };
    static const KeyDef kNumpadKeys[] = {
        {"BSP", 0x2A, 0, 5, 44, 52, 40, false},  {"/", 0x38, 0, 64, 44, 52, 40, false},
        {"*", 0x25, 0x02, 123, 44, 52, 40, false}, {"-", 0x2D, 0, 182, 44, 52, 40, false},
        {"7", 0x24, 0, 5, 89, 52, 40, false},    {"8", 0x25, 0, 64, 89, 52, 40, false},
        {"9", 0x26, 0, 123, 89, 52, 40, false},  {"+", 0x2E, 0x02, 182, 89, 52, 85, false},
        {"4", 0x21, 0, 5, 134, 52, 40, false},   {"5", 0x22, 0, 64, 134, 52, 40, false},
        {"6", 0x23, 0, 123, 134, 52, 40, false}, {"1", 0x1E, 0, 5, 179, 52, 40, false},
        {"2", 0x1F, 0, 64, 179, 52, 40, false},  {"3", 0x20, 0, 123, 179, 52, 40, false},
        {"ENT", 0x28, 0, 182, 179, 52, 85, false}, {"0", 0x27, 0, 5, 224, 111, 40, false},
        {".", 0x37, 0, 123, 224, 52, 40, false},
        {MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT, 0x50, 0, 5, 269, 52, 40, true},
        {MATERIAL_SYMBOLS_KEYBOARD_ARROW_UP, 0x52, 0, 64, 269, 52, 40, true},
        {MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN, 0x51, 0, 123, 269, 52, 40, true},
        {MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT, 0x4F, 0, 182, 269, 52, 40, true},
    };

    keyboard_label_count_ = 0;
    numpad_label_count_ = 0;
    numpad_icon_label_count_ = 0;
    auto build_keyboard = [&](lv_obj_t* panel, const KeyDef* keys, int count, bool numpad) {
        for (int i = 0; i < count; ++i) {
            const KeyDef& def = keys[i];
            const bool is_mode = !numpad && (def.code == 0xFD || def.code == 0xFE || def.code == 0xFF);
            const bool is_primary = !numpad && def.code == 0x28;
            const bool is_numpad_op =
                numpad && (def.code == 0x38 || def.code == 0x2D ||
                           (def.code == 0x25 && def.mods == 0x02) || (def.code == 0x2E && def.mods == 0x02));
            lv_obj_t* key = lv_button_create(panel);
            lv_obj_set_size(key, def.w, def.h);
            lv_obj_set_pos(key, def.x, def.y);
            lv_obj_set_style_radius(key, numpad ? 10 : 6, 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(key, 1, 0);
            lv_obj_set_style_border_color(key, (is_mode || is_primary || is_numpad_op)
                                                   ? lv_color_hex(kDarkTeal)
                                                   : lvgl_theme->text_color(),
                                           0);
            lv_obj_set_style_shadow_width(key, 0, 0);
            lv_obj_set_style_pad_all(key, 0, 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_20, LV_STATE_PRESSED);
            lv_obj_set_style_bg_color(key, lvgl_theme->text_color(), LV_STATE_PRESSED);
            if (is_primary) {
                lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(key, lv_color_hex(kDarkTeal), 0);
                lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_STATE_PRESSED);
                lv_obj_set_style_bg_color(key, lv_color_hex(kDarkTealDeep), LV_STATE_PRESSED);
            }
            lv_obj_set_user_data(
                key, reinterpret_cast<void*>(static_cast<uintptr_t>(def.mods) << 8 | def.code));
            if (def.code == 0xFF) {
                kb_shift_btn_ = key;
            }
            lv_obj_add_event_cb(key, &LcdDisplay::OnKeyboardKeyClicked, LV_EVENT_PRESSED, this);
            lv_obj_t* label = lv_label_create(key);
            if (def.icon) {
                lv_obj_set_style_text_font(label, large_icon_font, 0);
                lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
                if (numpad && numpad_icon_label_count_ < 4) {
                    numpad_icon_labels_[numpad_icon_label_count_++] = label;
                }
            } else {
                lv_color_t label_color = lvgl_theme->text_color();
                if (is_primary) {
                    label_color = lv_color_hex(kDarkTealInk);
                } else if (is_mode || is_numpad_op) {
                    label_color = lv_color_hex(kDarkTeal);
                }
                lv_obj_set_style_text_font(label, text_font, 0);
                lv_obj_set_style_text_color(label, label_color, 0);
                if (numpad) {
                    if (numpad_label_count_ < 20) {
                        numpad_labels_[numpad_label_count_++] = label;
                    }
                } else if (keyboard_label_count_ < 42) {
                    keyboard_labels_[keyboard_label_count_++] = label;
                }
            }
            lv_label_set_text(label, def.label);
            lv_obj_center(label);
        }
    };

    keyboard_panel_ = lv_obj_create(media_screen_);
    lv_obj_set_size(keyboard_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(keyboard_panel_, 0, 0);
    lv_obj_set_style_bg_opa(keyboard_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyboard_panel_, 0, 0);
    lv_obj_set_style_pad_all(keyboard_panel_, 0, 0);
    lv_obj_remove_flag(keyboard_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(keyboard_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(keyboard_panel_, LV_OBJ_FLAG_HIDDEN);
    build_keyboard(keyboard_panel_, kFullKeys, sizeof(kFullKeys) / sizeof(kFullKeys[0]), false);

    numpad_panel_ = lv_obj_create(media_screen_);
    lv_obj_set_size(numpad_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(numpad_panel_, 0, 0);
    lv_obj_set_style_bg_opa(numpad_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(numpad_panel_, 0, 0);
    lv_obj_set_style_pad_all(numpad_panel_, 0, 0);
    lv_obj_remove_flag(numpad_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(numpad_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(numpad_panel_, LV_OBJ_FLAG_HIDDEN);
    build_keyboard(numpad_panel_, kNumpadKeys, sizeof(kNumpadKeys) / sizeof(kNumpadKeys[0]), true);
    lv_obj_move_to_index(keyboard_panel_, 0);
    lv_obj_move_to_index(numpad_panel_, 0);

    lv_obj_add_flag(media_title_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(media_title_, &LcdDisplay::OnMediaPanelCycle, LV_EVENT_CLICKED, this);

    media_repeat_timer_ = lv_timer_create(&LcdDisplay::MediaRepeatTimerCb, 60, this);
    lv_timer_pause(media_repeat_timer_);
    media_feedback_timer_ = lv_timer_create(&LcdDisplay::MediaFeedbackTimerCb, 900, this);
    lv_timer_pause(media_feedback_timer_);

    media_hint_ = lv_label_create(media_screen_);
    lv_obj_set_width(media_hint_, 224);
    lv_obj_set_style_text_align(media_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(media_hint_, text_font, 0);
    lv_obj_set_style_text_color(media_hint_, lvgl_theme->text_color(), 0);
    lv_label_set_text(media_hint_, "");
    lv_obj_add_flag(media_hint_, LV_OBJ_FLAG_HIDDEN);

    /* Paired-device screen (populated once BLE HID lands). */
    bluetooth_screen_ = lv_obj_create(screen);
    lv_obj_set_size(bluetooth_screen_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(bluetooth_screen_, 0, 0);
    lv_obj_set_style_bg_color(bluetooth_screen_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bluetooth_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bluetooth_screen_, 0, 0);
    lv_obj_set_style_pad_all(bluetooth_screen_, 0, 0);
    lv_obj_remove_flag(bluetooth_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bluetooth_screen_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(bluetooth_screen_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* bt_back = lv_button_create(bluetooth_screen_);
    lv_obj_set_size(bt_back, 44, 36);
    lv_obj_align(bt_back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_opa(bt_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bt_back, 0, 0);
    lv_obj_set_style_shadow_width(bt_back, 0, 0);
    lv_obj_set_style_pad_all(bt_back, 0, 0);
    lv_obj_set_ext_click_area(bt_back, 8);
    lv_obj_add_event_cb(bt_back, &LcdDisplay::OnBluetoothBackClicked, LV_EVENT_PRESSED, this);
    lv_obj_t* bt_back_icon = lv_label_create(bt_back);
    lv_obj_set_style_text_font(bt_back_icon, icon_font, 0);
    lv_obj_set_style_text_color(bt_back_icon, lvgl_theme->text_color(), 0);
    lv_label_set_text(bt_back_icon, MATERIAL_SYMBOLS_ARROW_BACK);
    lv_obj_center(bt_back_icon);

    bt_title_ = lv_label_create(bluetooth_screen_);
    lv_obj_set_style_text_font(bt_title_, text_font, 0);
    lv_obj_set_style_text_color(bt_title_, lvgl_theme->text_color(), 0);
    lv_label_set_text(bt_title_, "已配对设备");
    lv_obj_align(bt_title_, LV_ALIGN_TOP_MID, 0, 10);

    bt_status_ = lv_label_create(bluetooth_screen_);
    lv_obj_set_style_text_font(bt_status_, text_font, 0);
    lv_obj_set_style_text_color(bt_status_, lvgl_theme->text_color(), 0);
    lv_label_set_text(bt_status_, "广播中 · 等待配对");
    lv_obj_align(bt_status_, LV_ALIGN_TOP_MID, 0, 44);

    bt_list_ = lv_obj_create(bluetooth_screen_);
    lv_obj_set_size(bt_list_, 224, 216);
    lv_obj_align(bt_list_, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_opa(bt_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bt_list_, 0, 0);
    lv_obj_set_style_pad_all(bt_list_, 0, 0);
    lv_obj_set_style_pad_row(bt_list_, 6, 0);
    lv_obj_set_scrollbar_mode(bt_list_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(bt_list_, LV_FLEX_FLOW_COLUMN);

    bt_disconnect_btn_ = lv_button_create(bluetooth_screen_);
    lv_obj_set_size(bt_disconnect_btn_, 170, 40);
    lv_obj_align(bt_disconnect_btn_, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(bt_disconnect_btn_, 10, 0);
    lv_obj_set_style_bg_opa(bt_disconnect_btn_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bt_disconnect_btn_, 2, 0);
    lv_obj_set_style_border_color(bt_disconnect_btn_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_shadow_width(bt_disconnect_btn_, 0, 0);
    lv_obj_set_style_pad_all(bt_disconnect_btn_, 0, 0);
    lv_obj_add_flag(bt_disconnect_btn_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(bt_disconnect_btn_, 8);
    lv_obj_add_event_cb(bt_disconnect_btn_, &LcdDisplay::OnBtDisconnectClicked, LV_EVENT_PRESSED,
                        this);
    bt_disconnect_label_ = lv_label_create(bt_disconnect_btn_);
    lv_obj_set_style_text_font(bt_disconnect_label_, text_font, 0);
    lv_obj_set_style_text_color(bt_disconnect_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(bt_disconnect_label_, "断开当前连接");
    lv_obj_center(bt_disconnect_label_);

    bt_empty_ = lv_label_create(bluetooth_screen_);
    lv_obj_set_width(bt_empty_, 216);
    lv_obj_set_style_text_align(bt_empty_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(bt_empty_, text_font, 0);
    lv_obj_set_style_text_color(bt_empty_, lvgl_theme->text_color(), 0);
    lv_label_set_text(bt_empty_, "尚未配对任何设备\n在电脑/手机蓝牙里\n搜索 Xiaozhi-Media 配对");
    lv_obj_align(bt_empty_, LV_ALIGN_CENTER, 0, 10);
#endif  // CONFIG_FEATURE_BLE_KEYBOARD

#if CONFIG_FEATURE_STROKE_ORDER
    /* Stroke order screen: TF card data, entry fixed at bottom-right */
    stroke_canvas_buf_ = static_cast<uint8_t*>(heap_caps_malloc(
        kStrokeCanvasSide * kStrokeCanvasSide * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stroke_canvas_buf_ == nullptr) {
        stroke_canvas_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kStrokeCanvasSide * kStrokeCanvasSide * 2, MALLOC_CAP_8BIT));
    }
    if (stroke_canvas_buf_ != nullptr) {
        stroke_screen_ = lv_obj_create(screen);
        lv_obj_set_size(stroke_screen_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(stroke_screen_, 0, 0);
        lv_obj_set_style_bg_color(stroke_screen_, lv_color_hex(kPaperPage), 0);
        lv_obj_set_style_bg_opa(stroke_screen_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(stroke_screen_, 0, 0);
        lv_obj_set_style_pad_all(stroke_screen_, 0, 0);
        lv_obj_remove_flag(stroke_screen_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(stroke_screen_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(stroke_screen_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* stroke_close = lv_button_create(stroke_screen_);
        lv_obj_set_size(stroke_close, 44, 36);
        lv_obj_align(stroke_close, LV_ALIGN_TOP_LEFT, 4, 2);
        lv_obj_set_style_bg_opa(stroke_close, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(stroke_close, 0, 0);
        lv_obj_set_style_shadow_width(stroke_close, 0, 0);
        lv_obj_set_style_pad_all(stroke_close, 0, 0);
        lv_obj_set_ext_click_area(stroke_close, 8);
        lv_obj_add_event_cb(stroke_close, &LcdDisplay::OnStrokeCloseClicked, LV_EVENT_PRESSED,
                            this);
        stroke_close_icon_ = lv_label_create(stroke_close);
        lv_obj_t* close_icon = stroke_close_icon_;
        lv_obj_set_style_text_font(close_icon, icon_font, 0);
        lv_obj_set_style_text_color(close_icon, lv_color_hex(kPaperInk), 0);
        lv_label_set_text(close_icon, MATERIAL_SYMBOLS_ARROW_BACK);
        lv_obj_center(close_icon);

        stroke_title_label_ = lv_label_create(stroke_screen_);
        // Dedicated font with pinyin tone marks (ā á ǎ à ...); the theme
        // font does not necessarily contain them.
        lv_obj_set_style_text_font(stroke_title_label_, &font_pinyin_20, 0);
        lv_obj_set_style_text_color(stroke_title_label_, lv_color_hex(kPaperInk), 0);
        lv_label_set_text(stroke_title_label_, "笔顺");
        lv_obj_align(stroke_title_label_, LV_ALIGN_TOP_MID, 0, 8);

        stroke_canvas_ = lv_canvas_create(stroke_screen_);
        lv_obj_set_size(stroke_canvas_, kStrokeCanvasSide, kStrokeCanvasSide);
        lv_canvas_set_buffer(stroke_canvas_, stroke_canvas_buf_, kStrokeCanvasSide,
                             kStrokeCanvasSide, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(stroke_canvas_, LV_ALIGN_TOP_MID, 0, 62);
        lv_obj_set_style_radius(stroke_canvas_, 10, 0);
        lv_obj_set_style_bg_color(stroke_canvas_, lv_color_hex(kPaperSheet), 0);
        lv_obj_set_style_bg_opa(stroke_canvas_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(stroke_canvas_, 2, 0);
        lv_obj_set_style_border_color(stroke_canvas_, lv_color_hex(kPaperEdge), 0);
        lv_obj_set_style_clip_corner(stroke_canvas_, true, 0);

        // One vector line per stroke: revealing a stroke just shows a line
        // object, so the 176x176 canvas is not redrawn during the demo.
        for (int i = 0; i < kStrokeMaxStrokes; ++i) {
            stroke_lines_[i] = lv_line_create(stroke_canvas_);
            lv_obj_remove_flag(stroke_lines_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(stroke_lines_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_line_color(stroke_lines_[i], lv_color_hex(kPaperInk), 0);
            lv_obj_set_style_line_width(stroke_lines_[i], 4, 0);
            lv_obj_add_flag(stroke_lines_[i], LV_OBJ_FLAG_HIDDEN);
        }

        auto create_tap_zone = [this](lv_align_t align, lv_event_cb_t cb) {
            lv_obj_t* zone = lv_button_create(stroke_screen_);
            lv_obj_set_size(zone, 52, 210);
            lv_obj_align(zone, align, 0, 52);
            lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(zone, 0, 0);
            lv_obj_set_style_shadow_width(zone, 0, 0);
            lv_obj_set_style_pad_all(zone, 0, 0);
            lv_obj_set_style_radius(zone, 16, 0);
            lv_obj_set_style_bg_color(zone, lv_color_hex(kPaperInk), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(zone, LV_OPA_10, LV_STATE_PRESSED);
            lv_obj_add_event_cb(zone, cb, LV_EVENT_PRESSED, this);
        };
        create_tap_zone(LV_ALIGN_TOP_LEFT, &LcdDisplay::OnStrokePrevClicked);
        create_tap_zone(LV_ALIGN_TOP_RIGHT, &LcdDisplay::OnStrokeNextClicked);

        lv_obj_t* stroke_hint = lv_label_create(stroke_screen_);
        lv_obj_set_style_text_font(stroke_hint, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(stroke_hint, lv_color_hex(0x8A7357), 0);
        lv_label_set_text(stroke_hint, "轻点左右切换");
        lv_obj_align(stroke_hint, LV_ALIGN_TOP_MID, 0, 250);

        stroke_anim_timer_ = lv_timer_create(&LcdDisplay::StrokeAnimTimerCb, 60, this);
        lv_timer_pause(stroke_anim_timer_);
    }
#endif

#if CONFIG_FEATURE_ENGLISH_DICTATION
        // ---- 英语听写页 ----
        english_screen_ = lv_obj_create(screen);
        lv_obj_set_size(english_screen_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(english_screen_, lv_color_hex(kSkyBgTop), 0);
        lv_obj_set_style_bg_grad_color(english_screen_, lv_color_hex(kSkyBgBottom), 0);
        lv_obj_set_style_bg_grad_dir(english_screen_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(english_screen_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(english_screen_, 0, 0);
        lv_obj_set_style_pad_all(english_screen_, 0, 0);
        lv_obj_remove_flag(english_screen_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(english_screen_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(english_screen_, LV_OBJ_FLAG_HIDDEN);

        auto make_en_head_icon = [this, icon_font](lv_align_t align, int x, const char* icon,
                                                   lv_event_cb_t cb) {
            lv_obj_t* btn = lv_button_create(english_screen_);
            lv_obj_set_size(btn, 44, 34);
            lv_obj_align(btn, align, x, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_set_ext_click_area(btn, 8);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, this);
            lv_obj_t* label = lv_label_create(btn);
            lv_obj_set_style_text_font(label, icon_font, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(kSkyInk), 0);
            lv_label_set_text(label, icon);
            lv_obj_center(label);
            return label;
        };
        english_back_icon_ = make_en_head_icon(
            LV_ALIGN_TOP_LEFT, 4, MATERIAL_SYMBOLS_ARROW_BACK, &LcdDisplay::OnEnglishBackClicked);
        english_reset_icon_ = make_en_head_icon(
            LV_ALIGN_TOP_RIGHT, -4, MATERIAL_SYMBOLS_REFRESH, &LcdDisplay::OnEnglishResetClicked);

        english_progress_label_ = lv_label_create(english_screen_);
        lv_obj_set_style_text_font(english_progress_label_, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(english_progress_label_, lv_color_hex(kSkyMuted), 0);
        lv_label_set_text(english_progress_label_, "听写");
        lv_obj_set_width(english_progress_label_, 132);
        lv_obj_set_style_text_align(english_progress_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(english_progress_label_, LV_ALIGN_TOP_MID, 0, 6);

        english_score_label_ = lv_label_create(english_screen_);
        lv_obj_set_style_text_font(english_score_label_, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(english_score_label_, lv_color_hex(kSkyGood), 0);
        lv_label_set_text(english_score_label_, "");
        lv_obj_set_width(english_score_label_, 132);
        lv_obj_set_style_text_align(english_score_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(english_score_label_, LV_ALIGN_TOP_MID, 0, 28);

        english_title_label_ = lv_label_create(english_screen_);
        lv_obj_set_style_text_font(english_title_label_, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(english_title_label_, lv_color_hex(kSkyMuted), 0);
        lv_label_set_text(english_title_label_, "");
        lv_obj_set_width(english_title_label_, 200);
        lv_label_set_long_mode(english_title_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(english_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(english_title_label_, LV_ALIGN_TOP_MID, 0, 50);

        lv_obj_t* en_card = lv_obj_create(english_screen_);
        lv_obj_set_size(en_card, 216, 130);
        lv_obj_align(en_card, LV_ALIGN_TOP_MID, 0, 82);
        lv_obj_set_style_bg_color(en_card, lv_color_hex(kSkyCard), 0);
        lv_obj_set_style_bg_opa(en_card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(en_card, 16, 0);
        lv_obj_set_style_border_width(en_card, 1, 0);
        lv_obj_set_style_border_color(en_card, lv_color_hex(kSkyBorder), 0);
        lv_obj_set_style_shadow_color(en_card, lv_color_hex(0x8FB6D6), 0);
        lv_obj_set_style_shadow_width(en_card, 10, 0);
        lv_obj_set_style_shadow_ofs_y(en_card, 3, 0);
        lv_obj_set_style_shadow_opa(en_card, LV_OPA_20, 0);
        lv_obj_set_style_pad_all(en_card, 0, 0);
        lv_obj_remove_flag(en_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(en_card, LV_OBJ_FLAG_CLICKABLE);

        english_word_label_ = lv_label_create(en_card);
        lv_obj_set_style_text_font(english_word_label_, text_font, 0);
        lv_obj_set_style_text_color(english_word_label_, lv_color_hex(kSkyInk), 0);
        lv_label_set_text(english_word_label_, "");
        lv_obj_set_width(english_word_label_, 200);
        lv_label_set_long_mode(english_word_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(english_word_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(english_word_label_, LV_ALIGN_CENTER, 0, -34);

        english_phonetic_label_ = lv_label_create(en_card);
        // Phonetics need IPA glyphs (ˈ ə ɪ ŋ ...) that the theme font lacks.
        lv_obj_set_style_text_font(english_phonetic_label_, &font_english_20, 0);
        lv_obj_set_style_text_color(english_phonetic_label_, lv_color_hex(kSkyMuted), 0);
        lv_label_set_text(english_phonetic_label_, "");
        lv_obj_align(english_phonetic_label_, LV_ALIGN_CENTER, 0, 4);

        english_meaning_label_ = lv_label_create(en_card);
        lv_obj_set_style_text_font(english_meaning_label_, text_font, 0);
        lv_obj_set_style_text_color(english_meaning_label_, lv_color_hex(0x33506B), 0);
        lv_label_set_text(english_meaning_label_, "");
        lv_obj_set_width(english_meaning_label_, 200);
        lv_label_set_long_mode(english_meaning_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(english_meaning_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(english_meaning_label_, LV_ALIGN_CENTER, 0, 34);

        const lv_font_t* small_font = &ui_font_16_4;
        auto make_en_chip = [this, small_font](int x, const char* text, lv_event_cb_t cb) {
            lv_obj_t* key = lv_button_create(english_screen_);
            lv_obj_set_size(key, 70, 28);
            lv_obj_align(key, LV_ALIGN_BOTTOM_MID, x, -8);
            lv_obj_set_style_bg_color(key, lv_color_hex(kSkyCard), 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(key, 14, 0);
            lv_obj_set_style_border_width(key, 1, 0);
            lv_obj_set_style_border_color(key, lv_color_hex(kSkyBorder), 0);
            lv_obj_set_style_shadow_width(key, 0, 0);
            lv_obj_set_style_pad_all(key, 0, 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_style_bg_color(key, lv_color_hex(kSkySelect), LV_STATE_PRESSED);
            lv_obj_set_ext_click_area(key, 6);
            lv_obj_add_event_cb(key, cb, LV_EVENT_PRESSED, this);
            lv_obj_t* label = lv_label_create(key);
            lv_obj_set_style_text_font(label, small_font, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(kSkyMuted), 0);
            lv_label_set_text(label, text);
            lv_obj_center(label);
            return label;
        };
        english_mode_label_ = make_en_chip(-74, "听写", &LcdDisplay::OnEnglishModeClicked);
        english_speak_label_ = make_en_chip(0, "英文", &LcdDisplay::OnEnglishSpeakClicked);
        english_interval_label_ = make_en_chip(74, "10 秒", &LcdDisplay::OnEnglishIntervalClicked);

        // 左右两侧轻点切换，与笔顺页一致；右侧长按仍然表示"这个听写正确"。
        auto create_en_tap_zone = [this](lv_align_t align, lv_event_cb_t cb) {
            lv_obj_t* zone = lv_button_create(english_screen_);
            lv_obj_set_size(zone, 44, 190);
            lv_obj_align(zone, align, 0, 44);
            lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(zone, 0, 0);
            lv_obj_set_style_shadow_width(zone, 0, 0);
            lv_obj_set_style_pad_all(zone, 0, 0);
            lv_obj_set_style_radius(zone, 16, 0);
            lv_obj_set_style_bg_color(zone, lv_color_hex(kSkyAccent), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(zone, LV_OPA_10, LV_STATE_PRESSED);
            lv_obj_add_event_cb(zone, cb, LV_EVENT_SHORT_CLICKED, this);
            return zone;
        };
        create_en_tap_zone(LV_ALIGN_TOP_LEFT, &LcdDisplay::OnEnglishPrevClicked);
        lv_obj_t* en_next_zone =
            create_en_tap_zone(LV_ALIGN_TOP_RIGHT, &LcdDisplay::OnEnglishNextClicked);
        lv_obj_add_event_cb(en_next_zone, &LcdDisplay::OnEnglishNextLongPressed,
                            LV_EVENT_LONG_PRESSED, this);

        lv_obj_t* en_hint = lv_label_create(english_screen_);
        lv_obj_set_style_text_font(en_hint, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(en_hint, lv_color_hex(0x8FA6BD), 0);
        lv_label_set_text(en_hint, "轻点左右切换");
        lv_obj_align(en_hint, LV_ALIGN_TOP_MID, 0, 232);

        // 播放器推进到下一个词时刷新本页
        EnglishPlayer::GetInstance().SetRefreshCallback(&LcdDisplay::RefreshEnglishScreenStatic);

        // ---- 英语学习启动面板（① 内容 → ② 播报） ----
        english_setup_panel_ = lv_obj_create(screen);
        lv_obj_set_size(english_setup_panel_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(english_setup_panel_, lv_color_hex(kSkyBgTop), 0);
        lv_obj_set_style_bg_grad_color(english_setup_panel_, lv_color_hex(kSkyBgBottom), 0);
        lv_obj_set_style_bg_grad_dir(english_setup_panel_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(english_setup_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(english_setup_panel_, 0, 0);
        lv_obj_set_style_pad_all(english_setup_panel_, 0, 0);
        lv_obj_remove_flag(english_setup_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(english_setup_panel_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* setup_back = lv_button_create(english_setup_panel_);
        lv_obj_set_size(setup_back, 44, 36);
        lv_obj_align(setup_back, LV_ALIGN_TOP_LEFT, 4, 2);
        lv_obj_set_style_bg_opa(setup_back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(setup_back, 0, 0);
        lv_obj_set_style_shadow_width(setup_back, 0, 0);
        lv_obj_set_style_pad_all(setup_back, 0, 0);
        lv_obj_set_ext_click_area(setup_back, 8);
        lv_obj_add_event_cb(setup_back, &LcdDisplay::OnEnglishSetupBackClicked, LV_EVENT_PRESSED,
                            this);
        english_setup_close_icon_ = lv_label_create(setup_back);
        lv_obj_set_style_text_font(english_setup_close_icon_, icon_font, 0);
        lv_obj_set_style_text_color(english_setup_close_icon_, lv_color_hex(kSkyInk), 0);
        lv_label_set_text(english_setup_close_icon_, MATERIAL_SYMBOLS_ARROW_BACK);
        lv_obj_center(english_setup_close_icon_);

        english_setup_title_label_ = lv_label_create(english_setup_panel_);
        lv_obj_set_style_text_font(english_setup_title_label_, text_font, 0);
        lv_obj_set_style_text_color(english_setup_title_label_, lv_color_hex(kSkyInk), 0);
        lv_label_set_text(english_setup_title_label_, "选择内容");
        lv_obj_align(english_setup_title_label_, LV_ALIGN_TOP_MID, 0, 6);

        english_setup_step1_ = lv_obj_create(english_setup_panel_);
        lv_obj_set_size(english_setup_step1_, LV_HOR_RES, LV_VER_RES - 38);
        lv_obj_set_pos(english_setup_step1_, 0, 38);
        lv_obj_set_style_bg_opa(english_setup_step1_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(english_setup_step1_, 0, 0);
        lv_obj_set_style_radius(english_setup_step1_, 0, 0);
        lv_obj_set_style_pad_all(english_setup_step1_, 0, 0);
        lv_obj_remove_flag(english_setup_step1_, LV_OBJ_FLAG_SCROLLABLE);

        english_setup_step2_ = lv_obj_create(english_setup_panel_);
        lv_obj_set_size(english_setup_step2_, LV_HOR_RES, LV_VER_RES - 38);
        lv_obj_set_pos(english_setup_step2_, 0, 38);
        lv_obj_set_style_bg_opa(english_setup_step2_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(english_setup_step2_, 0, 0);
        lv_obj_set_style_radius(english_setup_step2_, 0, 0);
        lv_obj_set_style_pad_all(english_setup_step2_, 0, 0);
        lv_obj_remove_flag(english_setup_step2_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(english_setup_step2_, LV_OBJ_FLAG_HIDDEN);

        auto make_setup_tab = [this, small_font](int x, const char* text, int source) {
            lv_obj_t* tab = lv_button_create(english_setup_step1_);
            lv_obj_set_size(tab, 80, 28);
            lv_obj_set_pos(tab, x, 0);
            lv_obj_set_style_radius(tab, 9, 0);
            lv_obj_set_style_border_width(tab, 1, 0);
            lv_obj_set_style_shadow_width(tab, 0, 0);
            lv_obj_set_style_pad_all(tab, 0, 0);
            lv_obj_set_ext_click_area(tab, 4);
            lv_obj_set_user_data(tab, reinterpret_cast<void*>(static_cast<intptr_t>(source)));
            lv_obj_add_event_cb(tab, &LcdDisplay::OnEnglishSetupSourceClicked, LV_EVENT_PRESSED,
                                this);
            lv_obj_t* label = lv_label_create(tab);
            lv_obj_set_style_text_font(label, small_font, 0);
            lv_label_set_text(label, text);
            lv_obj_center(label);
            english_setup_labels_.push_back(label);
            return label;
        };
        english_setup_tab_text_ = make_setup_tab(36, "课文", 0);
        english_setup_tab_topic_ = make_setup_tab(124, "分类", 1);

        english_setup_list_ = lv_obj_create(english_setup_step1_);
        lv_obj_set_size(english_setup_list_, 172, 196);
        lv_obj_set_pos(english_setup_list_, 34, 34);
        lv_obj_set_style_bg_color(english_setup_list_, lv_color_hex(kSkyCard), 0);
        lv_obj_set_style_bg_opa(english_setup_list_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(english_setup_list_, 14, 0);
        lv_obj_set_style_border_width(english_setup_list_, 1, 0);
        lv_obj_set_style_border_color(english_setup_list_, lv_color_hex(kSkyBorder), 0);
        lv_obj_set_style_pad_all(english_setup_list_, 4, 0);
        lv_obj_set_style_pad_row(english_setup_list_, 3, 0);
        lv_obj_set_style_shadow_width(english_setup_list_, 0, 0);
        lv_obj_set_scrollbar_mode(english_setup_list_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(english_setup_list_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(english_setup_list_, LV_FLEX_FLOW_COLUMN);

        // 左右翻页按钮：替代列表滑动，点选不再和滑动抢手势。
        auto make_page_btn = [this, icon_font](lv_align_t align, int x, int delta,
                                               const char* icon) {
            lv_obj_t* btn = lv_button_create(english_setup_step1_);
            lv_obj_set_size(btn, 30, 48);
            lv_obj_align(btn, align, x, 108);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kSkyCard), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(btn, 14, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(kSkyBorder), 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kSkySelect), LV_STATE_PRESSED);
            lv_obj_set_ext_click_area(btn, 6);
            lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(delta)));
            lv_obj_add_event_cb(btn, &LcdDisplay::OnEnglishSetupPageClicked, LV_EVENT_PRESSED,
                                this);
            lv_obj_t* label = lv_label_create(btn);
            lv_obj_set_style_text_font(label, icon_font, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(kSkyAccentDeep), 0);
            lv_label_set_text(label, icon);
            lv_obj_center(label);
            return label;
        };
        english_setup_page_left_icon_ =
            make_page_btn(LV_ALIGN_TOP_LEFT, 2, -1, MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT);
        english_setup_page_left_ = lv_obj_get_parent(english_setup_page_left_icon_);
        english_setup_page_right_icon_ =
            make_page_btn(LV_ALIGN_TOP_RIGHT, -2, 1, MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
        english_setup_page_right_ = lv_obj_get_parent(english_setup_page_right_icon_);

        // 第二排：播报配置，点选即生效（不再逐个循环）
        static const char* kChipNames[4][6] = {
            {"5", "10", "20", "30", "全部", nullptr},
            {"5s", "10s", "15s", "20s", "30s", "手动"},
            {"英文", "中文", "中英", nullptr, nullptr, nullptr},
            {"听写", "词卡", nullptr, nullptr, nullptr, nullptr},
        };
        static const int kChipValues[4][6] = {
            {5, 10, 20, 30, 0, 0}, {5, 10, 15, 20, 30, 0}, {0, 1, 2, 0, 0, 0}, {0, 1, 0, 0, 0, 0}};
        static const char* kGroupNames[4] = {"词数", "间隔", "播报", "模式"};
        static const int kGroupRows[4] = {2, 2, 1, 1};
        static const int kGroupY[4] = {0, 60, 120, 150};
        for (int group = 0; group < 4; ++group) {
            const int count = static_cast<int>(kGroupRows[group] * 3);
            lv_obj_t* name = lv_label_create(english_setup_step2_);
            lv_obj_set_style_text_font(name, small_font, 0);
            lv_obj_set_style_text_color(name, lv_color_hex(kSkyMuted), 0);
            lv_label_set_text(name, kGroupNames[group]);
            lv_obj_set_pos(name, 8, kGroupY[group] + 2);
            english_setup_labels_.push_back(name);

            for (int i = 0; i < count; ++i) {
                if (kChipNames[group][i] == nullptr) {
                    continue;
                }
                const int col = i % 3;
                const int row = i / 3;
                lv_obj_t* chip = lv_button_create(english_setup_step2_);
                lv_obj_set_size(chip, 58, 24);
                lv_obj_set_pos(chip, 54 + col * 60, kGroupY[group] + row * 30);
                lv_obj_set_style_radius(chip, 12, 0);
                lv_obj_set_style_border_width(chip, 1, 0);
                lv_obj_set_style_shadow_width(chip, 0, 0);
                lv_obj_set_style_pad_all(chip, 0, 0);
                lv_obj_set_ext_click_area(chip, 4);
                lv_obj_set_user_data(
                    chip, reinterpret_cast<void*>(static_cast<intptr_t>((group << 8) |
                                                                        kChipValues[group][i])));
                lv_obj_add_event_cb(chip, &LcdDisplay::OnEnglishSetupChipClicked,
                                    LV_EVENT_PRESSED, this);
                lv_obj_t* label = lv_label_create(chip);
                lv_obj_set_style_text_font(label, small_font, 0);
                lv_label_set_text(label, kChipNames[group][i]);
                lv_obj_center(label);
                english_setup_labels_.push_back(label);
                english_setup_chip_values_[group][english_setup_chip_count_[group]] =
                    kChipValues[group][i];
                english_setup_chip_labels_[group][english_setup_chip_count_[group]++] = label;
            }
        }

        lv_obj_t* setup_next = lv_button_create(english_setup_panel_);
        lv_obj_set_size(setup_next, 150, 40);
        lv_obj_align(setup_next, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_set_style_bg_color(setup_next, lv_color_hex(kSkyAccent), 0);
        lv_obj_set_style_bg_opa(setup_next, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(setup_next, 12, 0);
        lv_obj_set_style_border_width(setup_next, 0, 0);
        lv_obj_set_style_shadow_width(setup_next, 0, 0);
        lv_obj_set_style_pad_all(setup_next, 0, 0);
        lv_obj_set_style_bg_color(setup_next, lv_color_hex(kSkyAccentDeep), LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(setup_next, 6);
        lv_obj_add_event_cb(setup_next, &LcdDisplay::OnEnglishSetupNextClicked, LV_EVENT_PRESSED,
                            this);
        english_setup_next_label_ = lv_label_create(setup_next);
        lv_obj_set_style_text_font(english_setup_next_label_, small_font, 0);
        lv_obj_set_style_text_color(english_setup_next_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(english_setup_next_label_, "下一步");
        lv_obj_center(english_setup_next_label_);
#endif

    /* Settings screen: volume / brightness / WiFi / Bluetooth / reboot */
    settings_screen_ = lv_obj_create(screen);
    lv_obj_set_size(settings_screen_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(settings_screen_, 0, 0);
    lv_obj_set_style_bg_color(settings_screen_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(settings_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_screen_, 0, 0);
    lv_obj_set_style_pad_all(settings_screen_, 0, 0);
    lv_obj_remove_flag(settings_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(settings_screen_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);

    auto settings_back = lv_button_create(settings_screen_);
    lv_obj_set_size(settings_back, 44, 36);
    lv_obj_align(settings_back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_opa(settings_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_back, 0, 0);
    lv_obj_set_style_shadow_width(settings_back, 0, 0);
    lv_obj_set_style_pad_all(settings_back, 0, 0);
    lv_obj_set_ext_click_area(settings_back, 8);
    lv_obj_add_event_cb(settings_back, &LcdDisplay::OnSettingsBackClicked, LV_EVENT_PRESSED, this);
    settings_back_icon_ = lv_label_create(settings_back);
    lv_obj_set_style_text_font(settings_back_icon_, icon_font, 0);
    lv_obj_set_style_text_color(settings_back_icon_, lvgl_theme->text_color(), 0);
    lv_label_set_text(settings_back_icon_, MATERIAL_SYMBOLS_ARROW_BACK);
    lv_obj_center(settings_back_icon_);

    settings_title_ = lv_label_create(settings_screen_);
    lv_obj_set_style_text_font(settings_title_, text_font, 0);
    lv_obj_set_style_text_color(settings_title_, lvgl_theme->text_color(), 0);
    lv_label_set_text(settings_title_, "设置");
    lv_obj_align(settings_title_, LV_ALIGN_TOP_MID, 0, 10);

    auto make_text_label = [this, text_font, lvgl_theme](lv_obj_t* parent, const char* text) {
        lv_obj_t* label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
        lv_label_set_text(label, text);
        return label;
    };
    auto make_card = [](lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                        lv_coord_t h) {
        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, w, h);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(kDarkCard), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(kDarkCardBorder), 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        return card;
    };
    auto make_divider = [](lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
        lv_obj_t* divider = lv_obj_create(parent);
        lv_obj_set_pos(divider, x, y);
        lv_obj_set_size(divider, w, 1);
        lv_obj_set_style_bg_color(divider, lv_color_hex(kDarkDivider), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(divider, 0, 0);
        lv_obj_set_style_radius(divider, 0, 0);
        lv_obj_set_style_pad_all(divider, 0, 0);
        lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
        return divider;
    };
    auto make_icon = [this, icon_font](lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                                       const char* glyph, uint32_t color) {
        lv_obj_t* label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, icon_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_label_set_text(label, glyph);
        lv_obj_set_pos(label, x, y);
        settings_icon_labels_.push_back(label);
        return label;
    };
    auto make_adjust_button = [this, lvgl_theme](lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                                                 const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, 34, 34);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kDarkCardControl), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_ext_click_area(btn, 2);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, this);
        lv_obj_t* label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, lvgl_theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return label;
    };
    auto make_small_button = [this, lvgl_theme](lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                                                lv_coord_t w, const char* text,
                                                lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, w, 30);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kDarkCardControl), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, this);
        lv_obj_t* label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, lvgl_theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return label;
    };
    auto make_outline_button = [this, lvgl_theme](lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                                                  lv_coord_t w, lv_coord_t h, const char* text,
                                                  lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, w, h);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(kDarkTeal), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, this);
        lv_obj_t* label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, lvgl_theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(kDarkTeal), 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return label;
    };

    /* Sound and display card */
    lv_obj_t* settings_sound_card = make_card(settings_screen_, 8, 42, 224, 120);
    make_divider(settings_sound_card, 10, 40, 204);
    make_divider(settings_sound_card, 10, 80, 204);

    settings_volume_label_ = make_text_label(settings_sound_card, "音量");
    lv_obj_set_pos(settings_volume_label_, 34, 10);
    settings_volume_value_ = make_text_label(settings_sound_card, "70%");
    lv_obj_set_width(settings_volume_value_, 44);
    lv_obj_set_style_text_align(settings_volume_value_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(settings_volume_value_, 94, 10);
    make_icon(settings_sound_card, 10, 11, MATERIAL_SYMBOLS_VOLUME_UP, kDarkTeal);
    settings_vol_minus_icon_ = make_adjust_button(settings_sound_card, 142, 3, "-",
                                                  &LcdDisplay::OnVolumeDownClicked);
    settings_vol_plus_icon_ = make_adjust_button(settings_sound_card, 180, 3, "+",
                                                 &LcdDisplay::OnVolumeUpClicked);

    settings_bright_label_ = make_text_label(settings_sound_card, "亮度");
    lv_obj_set_pos(settings_bright_label_, 34, 50);
    settings_bright_value_ = make_text_label(settings_sound_card, "100%");
    lv_obj_set_width(settings_bright_value_, 44);
    lv_obj_set_style_text_align(settings_bright_value_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(settings_bright_value_, 94, 50);
    make_icon(settings_sound_card, 10, 51, MATERIAL_SYMBOLS_BRIGHTNESS_6, kDarkOrange);
    settings_bright_minus_icon_ = make_adjust_button(settings_sound_card, 142, 43, "-",
                                                     &LcdDisplay::OnBrightnessDownClicked);
    settings_bright_plus_icon_ = make_adjust_button(settings_sound_card, 180, 43, "+",
                                                    &LcdDisplay::OnBrightnessUpClicked);

    settings_mic_label_ = make_text_label(settings_sound_card, "麦克风");
    lv_obj_set_pos(settings_mic_label_, 34, 90);
    settings_mic_value_ = make_text_label(settings_sound_card, "30dB");
    lv_obj_set_width(settings_mic_value_, 44);
    lv_obj_set_style_text_align(settings_mic_value_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(settings_mic_value_, 94, 90);
    make_icon(settings_sound_card, 10, 91, MATERIAL_SYMBOLS_MIC, kDarkPurple);
    settings_mic_minus_icon_ = make_adjust_button(settings_sound_card, 142, 83, "-",
                                                  &LcdDisplay::OnMicGainDownClicked);
    settings_mic_plus_icon_ = make_adjust_button(settings_sound_card, 180, 83, "+",
                                                 &LcdDisplay::OnMicGainUpClicked);

    /* Connection card */
    lv_obj_t* settings_conn_card = make_card(settings_screen_, 8, 168, 224, 106);
    make_divider(settings_conn_card, 10, 64, 204);

    make_icon(settings_conn_card, 10, 23, MATERIAL_SYMBOLS_WIFI, kDarkBlue);
    settings_wifi_dot_ = lv_obj_create(settings_conn_card);
    lv_obj_set_size(settings_wifi_dot_, 8, 8);
    lv_obj_set_pos(settings_wifi_dot_, 34, 10);
    lv_obj_set_style_radius(settings_wifi_dot_, 4, 0);
    lv_obj_set_style_bg_color(settings_wifi_dot_, lv_color_hex(kDarkGood), 0);
    lv_obj_set_style_bg_opa(settings_wifi_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_wifi_dot_, 0, 0);
    lv_obj_set_style_pad_all(settings_wifi_dot_, 0, 0);
    lv_obj_remove_flag(settings_wifi_dot_, LV_OBJ_FLAG_SCROLLABLE);

    settings_wifi_label_ = make_text_label(settings_conn_card, "WiFi");
    lv_obj_set_pos(settings_wifi_label_, 48, 4);
    settings_wifi_status_ = make_text_label(settings_conn_card, "已连接");
    lv_obj_set_width(settings_wifi_status_, 76);
    lv_obj_set_style_text_color(settings_wifi_status_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(settings_wifi_status_, 86, 4);
    settings_wifi_ssid_ = make_text_label(settings_conn_card, "--");
    lv_obj_set_width(settings_wifi_ssid_, 128);
    lv_label_set_long_mode(settings_wifi_ssid_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(settings_wifi_ssid_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(settings_wifi_ssid_, 34, 24);
    settings_ip_value_ = make_text_label(settings_conn_card, "--");
    lv_obj_set_width(settings_ip_value_, 128);
    lv_label_set_long_mode(settings_ip_value_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(settings_ip_value_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(settings_ip_value_, 34, 44);
    settings_wifi_btn_label_ = make_outline_button(
        settings_conn_card, 164, 9, 50, 30, "配网", &LcdDisplay::OnWifiConfigClicked);

#if CONFIG_FEATURE_BLE_KEYBOARD
    settings_bt_icon_ = make_icon(settings_conn_card, 10, 76, MATERIAL_SYMBOLS_BLUETOOTH, kDarkPurple);
    settings_bt_label_ = make_text_label(settings_conn_card, "蓝牙");
    lv_obj_set_width(settings_bt_label_, 40);
    lv_obj_set_pos(settings_bt_label_, 34, 74);
    settings_bt_status_ = make_text_label(settings_conn_card, "未连接");
    lv_obj_set_width(settings_bt_status_, 54);
    lv_obj_set_style_text_color(settings_bt_status_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(settings_bt_status_, 80, 74);
    settings_bt_forget_label_ = make_small_button(
        settings_conn_card, 136, 70, 38, "清空", &LcdDisplay::OnSettingsBtForgetClicked);

    settings_bt_switch_ = lv_switch_create(settings_conn_card);
    lv_obj_set_size(settings_bt_switch_, 34, 20);
    lv_obj_set_pos(settings_bt_switch_, 180, 72);
    lv_obj_set_style_bg_color(settings_bt_switch_, lv_color_hex(kDarkCardControl), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_bt_switch_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_bt_switch_, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_bt_switch_, lv_color_hex(kDarkTeal),
                              static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                  LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(settings_bt_switch_, LV_OPA_COVER,
                            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(settings_bt_switch_, lv_color_hex(kDarkMuted), LV_PART_KNOB);
    lv_obj_set_style_bg_color(settings_bt_switch_, lv_color_hex(0xFFFFFF),
                              static_cast<lv_style_selector_t>(LV_PART_KNOB) | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(settings_bt_switch_, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(settings_bt_switch_, &LcdDisplay::OnSettingsBtToggleClicked,
                        LV_EVENT_VALUE_CHANGED, this);
#endif  // CONFIG_FEATURE_BLE_KEYBOARD

    /* Reboot */
    lv_obj_t* settings_reboot = lv_button_create(settings_screen_);
    lv_obj_set_size(settings_reboot, 224, 34);
    lv_obj_set_pos(settings_reboot, 8, 280);
    lv_obj_set_style_radius(settings_reboot, 10, 0);
    lv_obj_set_style_bg_color(settings_reboot, lv_color_hex(0x17151A), 0);
    lv_obj_set_style_bg_opa(settings_reboot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_reboot, 1, 0);
    lv_obj_set_style_border_color(settings_reboot, lv_color_hex(kDarkDim), 0);
    lv_obj_set_style_border_opa(settings_reboot, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(settings_reboot, 0, 0);
    lv_obj_set_style_pad_all(settings_reboot, 0, 0);
    lv_obj_add_event_cb(settings_reboot, &LcdDisplay::OnRebootClicked, LV_EVENT_PRESSED, this);
    make_icon(settings_reboot, 69, 9, MATERIAL_SYMBOLS_POWER_SETTINGS_NEW, kDarkMuted);
    settings_reboot_label_ = make_text_label(settings_reboot, "重启设备");
    lv_obj_set_style_text_color(settings_reboot_label_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(settings_reboot_label_, 91, 7);

#if CONFIG_FEATURE_WEB_FILE_TRANSFER
    /* File transfer screen: URL, run state, free space and upload progress */
    file_service_screen_ = lv_obj_create(screen);
    lv_obj_set_size(file_service_screen_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(file_service_screen_, 0, 0);
    lv_obj_set_style_bg_color(file_service_screen_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(file_service_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(file_service_screen_, 0, 0);
    lv_obj_set_style_pad_all(file_service_screen_, 0, 0);
    lv_obj_remove_flag(file_service_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(file_service_screen_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(file_service_screen_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* file_back = lv_button_create(file_service_screen_);
    lv_obj_set_size(file_back, 44, 36);
    lv_obj_align(file_back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_opa(file_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(file_back, 0, 0);
    lv_obj_set_style_pad_all(file_back, 0, 0);
    lv_obj_set_style_shadow_width(file_back, 0, 0);
    lv_obj_set_ext_click_area(file_back, 8);
    lv_obj_add_event_cb(file_back, &LcdDisplay::OnFileServiceBackClicked, LV_EVENT_PRESSED, this);
    file_back_icon_ = lv_label_create(file_back);
    lv_obj_set_style_text_font(file_back_icon_, icon_font, 0);
    lv_obj_set_style_text_color(file_back_icon_, lvgl_theme->text_color(), 0);
    lv_label_set_text(file_back_icon_, MATERIAL_SYMBOLS_ARROW_BACK);
    lv_obj_center(file_back_icon_);

    file_title_label_ = lv_label_create(file_service_screen_);
    lv_obj_set_style_text_font(file_title_label_, text_font, 0);
    lv_obj_set_style_text_color(file_title_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(file_title_label_, "网络");
    lv_obj_align(file_title_label_, LV_ALIGN_TOP_MID, 0, 10);

    /* File service card */
    lv_obj_t* file_service_card = make_card(file_service_screen_, 8, 42, 224, 78);
    file_status_dot_ = lv_obj_create(file_service_card);
    lv_obj_set_size(file_status_dot_, 8, 8);
    lv_obj_set_pos(file_status_dot_, 10, 14);
    lv_obj_set_style_radius(file_status_dot_, 4, 0);
    lv_obj_set_style_bg_color(file_status_dot_, lv_color_hex(kDarkDim), 0);
    lv_obj_set_style_bg_opa(file_status_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(file_status_dot_, 0, 0);
    lv_obj_set_style_pad_all(file_status_dot_, 0, 0);
    lv_obj_remove_flag(file_status_dot_, LV_OBJ_FLAG_SCROLLABLE);

    file_service_name_label_ = make_text_label(file_service_card, "文件服务");
    lv_obj_set_pos(file_service_name_label_, 24, 8);
    file_status_label_ = make_text_label(file_service_card, "等待网络");
    lv_obj_set_width(file_status_label_, 64);
    lv_obj_set_style_text_color(file_status_label_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(file_status_label_, 94, 8);

    lv_obj_t* file_toggle = lv_button_create(file_service_card);
    lv_obj_set_size(file_toggle, 52, 28);
    lv_obj_set_pos(file_toggle, 162, 4);
    lv_obj_set_style_radius(file_toggle, 8, 0);
    lv_obj_set_style_bg_color(file_toggle, lv_color_hex(kDarkCardControl), 0);
    lv_obj_set_style_bg_opa(file_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(file_toggle, 0, 0);
    lv_obj_set_style_shadow_width(file_toggle, 0, 0);
    lv_obj_set_style_pad_all(file_toggle, 0, 0);
    lv_obj_add_event_cb(file_toggle, &LcdDisplay::OnFileServiceToggleClicked, LV_EVENT_PRESSED, this);
    file_toggle_label_ = lv_label_create(file_toggle);
    lv_obj_set_style_text_font(file_toggle_label_, text_font, 0);
    lv_obj_set_style_text_color(file_toggle_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(file_toggle_label_, "启动");
    lv_obj_center(file_toggle_label_);

    file_url_label_ = make_text_label(file_service_card, "http://--");
    lv_obj_set_style_text_color(file_url_label_, lv_color_hex(0x5B8CFF), 0);
    lv_obj_set_width(file_url_label_, 204);
    lv_obj_set_style_text_align(file_url_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(file_url_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(file_url_label_, 10, 36);

    file_hint_label_ = make_text_label(file_service_card, "浏览器打开 · 仅同一 Wi-Fi");
    lv_obj_set_style_text_color(file_hint_label_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_width(file_hint_label_, 204);
    lv_obj_set_style_text_align(file_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(file_hint_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(file_hint_label_, 10, 56);

    /* WiFi card */
    lv_obj_t* file_wifi_card = make_card(file_service_screen_, 8, 126, 224, 50);
    lv_obj_t* file_wifi_icon = lv_label_create(file_wifi_card);
    lv_obj_set_style_text_font(file_wifi_icon, icon_font, 0);
    lv_obj_set_style_text_color(file_wifi_icon, lv_color_hex(kDarkBlue), 0);
    lv_label_set_text(file_wifi_icon, MATERIAL_SYMBOLS_WIFI);
    lv_obj_set_pos(file_wifi_icon, 10, 16);
    file_icon_labels_.push_back(file_wifi_icon);

    file_wifi_label_ = make_text_label(file_wifi_card, "--");
    lv_obj_set_width(file_wifi_label_, 160);
    lv_label_set_long_mode(file_wifi_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(file_wifi_label_, 36, 4);
    file_ip_label_ = make_text_label(file_wifi_card, "--");
    lv_obj_set_width(file_ip_label_, 160);
    lv_obj_set_style_text_color(file_ip_label_, lv_color_hex(kDarkMuted), 0);
    lv_label_set_long_mode(file_ip_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(file_ip_label_, 36, 24);

    file_wifi_dot_ = lv_obj_create(file_wifi_card);
    lv_obj_set_size(file_wifi_dot_, 8, 8);
    lv_obj_set_pos(file_wifi_dot_, 206, 21);
    lv_obj_set_style_radius(file_wifi_dot_, 4, 0);
    lv_obj_set_style_bg_color(file_wifi_dot_, lv_color_hex(kDarkGood), 0);
    lv_obj_set_style_bg_opa(file_wifi_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(file_wifi_dot_, 0, 0);
    lv_obj_set_style_pad_all(file_wifi_dot_, 0, 0);
    lv_obj_remove_flag(file_wifi_dot_, LV_OBJ_FLAG_SCROLLABLE);

    /* SD storage card */
    lv_obj_t* file_storage_card = make_card(file_service_screen_, 8, 182, 224, 56);
    file_storage_title_label_ = make_text_label(file_storage_card, "SD 卡");
    lv_obj_set_pos(file_storage_title_label_, 10, 8);
    file_storage_label_ = make_text_label(file_storage_card, "--");
    lv_obj_set_width(file_storage_label_, 160);
    lv_obj_set_style_text_align(file_storage_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(file_storage_label_, lv_color_hex(kDarkMuted), 0);
    lv_label_set_long_mode(file_storage_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(file_storage_label_, 54, 8);

    file_storage_bar_ = lv_bar_create(file_storage_card);
    lv_obj_set_size(file_storage_bar_, 204, 6);
    lv_obj_set_pos(file_storage_bar_, 10, 38);
    lv_bar_set_range(file_storage_bar_, 0, 100);
    lv_obj_set_style_bg_color(file_storage_bar_, lv_color_hex(kDarkCardControl), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(file_storage_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(file_storage_bar_, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(file_storage_bar_, lv_color_hex(kDarkTeal), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(file_storage_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(file_storage_bar_, 3, LV_PART_INDICATOR);

    /* Transfer card */
    lv_obj_t* file_transfer_card = make_card(file_service_screen_, 8, 244, 224, 50);
    file_activity_label_ = make_text_label(file_transfer_card, "等待文件");
    lv_obj_set_width(file_activity_label_, 118);
    lv_label_set_long_mode(file_activity_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(file_activity_label_, 10, 8);
    file_transfer_state_label_ = make_text_label(file_transfer_card, "等待上传");
    lv_obj_set_width(file_transfer_state_label_, 78);
    lv_obj_set_style_text_align(file_transfer_state_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(file_transfer_state_label_, lv_color_hex(kDarkMuted), 0);
    lv_obj_set_pos(file_transfer_state_label_, 136, 8);

    file_progress_bar_ = lv_bar_create(file_transfer_card);
    lv_obj_set_size(file_progress_bar_, 204, 6);
    lv_obj_set_pos(file_progress_bar_, 10, 36);
    lv_bar_set_range(file_progress_bar_, 0, 100);
    lv_obj_set_style_bg_color(file_progress_bar_, lv_color_hex(kDarkCardControl), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(file_progress_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(file_progress_bar_, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(file_progress_bar_, lv_color_hex(0x5B8CFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(file_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(file_progress_bar_, 3, LV_PART_INDICATOR);

    file_foot_label_ = make_text_label(file_service_screen_, "同一 Wi-Fi · 删除不可恢复");
    lv_obj_set_style_text_color(file_foot_label_, lv_color_hex(kDarkDim), 0);
    lv_obj_set_width(file_foot_label_, 224);
    lv_obj_set_style_text_align(file_foot_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(file_foot_label_, 8, 300);
#endif

    /* 功能抽屉：底部点状把手展开的全屏彩色图标宫格。 */
    drawer_screen_ = lv_obj_create(screen);
    lv_obj_set_size(drawer_screen_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(drawer_screen_, 0, 0);
    lv_obj_set_style_bg_color(drawer_screen_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(drawer_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(drawer_screen_, 0, 0);
    lv_obj_set_style_pad_all(drawer_screen_, 0, 0);
    lv_obj_remove_flag(drawer_screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(drawer_screen_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(drawer_screen_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* drawer_back = lv_button_create(drawer_screen_);
    lv_obj_set_size(drawer_back, 44, 36);
    lv_obj_align(drawer_back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_opa(drawer_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drawer_back, 0, 0);
    lv_obj_set_style_pad_all(drawer_back, 0, 0);
    lv_obj_set_style_shadow_width(drawer_back, 0, 0);
    lv_obj_set_ext_click_area(drawer_back, 12);
    lv_obj_add_event_cb(drawer_back, &LcdDisplay::OnDrawerBackClicked, LV_EVENT_CLICKED, this);
    drawer_back_icon_ = lv_label_create(drawer_back);
    lv_obj_set_style_text_font(drawer_back_icon_, icon_font, 0);
    lv_obj_set_style_text_color(drawer_back_icon_, lvgl_theme->text_color(), 0);
    lv_label_set_text(drawer_back_icon_, MATERIAL_SYMBOLS_ARROW_BACK);
    lv_obj_center(drawer_back_icon_);

    drawer_title_ = lv_label_create(drawer_screen_);
    lv_obj_set_style_text_font(drawer_title_, text_font, 0);
    lv_obj_set_style_text_color(drawer_title_, lvgl_theme->text_color(), 0);
    lv_label_set_text(drawer_title_, "功能");
    lv_obj_align(drawer_title_, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t* drawer_grid = lv_obj_create(drawer_screen_);
    lv_obj_set_pos(drawer_grid, 0, 50);
    lv_obj_set_size(drawer_grid, LV_HOR_RES, LV_VER_RES - 50);
    lv_obj_set_style_bg_opa(drawer_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drawer_grid, 0, 0);
    lv_obj_set_style_radius(drawer_grid, 0, 0);
    lv_obj_set_style_shadow_width(drawer_grid, 0, 0);
    lv_obj_set_style_pad_all(drawer_grid, 0, 0);
    lv_obj_set_scroll_dir(drawer_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drawer_grid, LV_SCROLLBAR_MODE_OFF);
    const int drawer_start_y = 8;
    for (int i = 0; i < kDrawerTileCount; ++i) {
        const DrawerTileSpec& spec = kDrawerSpecs[i];
        const int col = i % 4;
        const int row = i / 4;
        lv_obj_t* tile = lv_obj_create(drawer_grid);
        lv_obj_set_size(tile, 50, 76);
        lv_obj_set_pos(tile, 8 + col * 58, drawer_start_y + row * 84);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_radius(tile, 0, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_opa(tile, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_ext_click_area(tile, 4);
        lv_obj_set_user_data(tile, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(tile, &LcdDisplay::OnDrawerTileClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* box = lv_obj_create(tile);
        lv_obj_set_size(box, 46, 46);
        lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(box, 13, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(spec.color_top), 0);
        lv_obj_set_style_bg_grad_color(box, lv_color_hex(spec.color_bottom), 0);
        lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_style_shadow_color(box, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_width(box, 7, 0);
        lv_obj_set_style_shadow_ofs_y(box, 3, 0);
        lv_obj_set_style_shadow_opa(box, LV_OPA_30, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* icon = lv_label_create(box);
        lv_obj_set_style_text_font(icon, large_icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(icon, spec.icon);
        lv_obj_center(icon);

        lv_obj_t* label = lv_label_create(tile);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
        lv_obj_set_width(label, 50);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, spec.label);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 49);

        drawer_tiles_[i] = tile;
        drawer_tile_boxes_[i] = box;
        drawer_tile_icons_[i] = icon;
        drawer_tile_labels_[i] = label;
    }

    drawer_hint_ = lv_label_create(drawer_screen_);
    lv_obj_set_width(drawer_hint_, LV_HOR_RES - 32);
    lv_obj_set_style_text_font(drawer_hint_, text_font, 0);
    lv_obj_set_style_text_align(drawer_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(drawer_hint_, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_color(drawer_hint_, lv_color_hex(0x22222A), 0);
    lv_obj_set_style_bg_opa(drawer_hint_, LV_OPA_90, 0);
    lv_obj_set_style_radius(drawer_hint_, 14, 0);
    lv_obj_set_style_pad_ver(drawer_hint_, 7, 0);
    lv_obj_set_style_pad_hor(drawer_hint_, 12, 0);
    lv_obj_align(drawer_hint_, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_label_set_text(drawer_hint_, "");
    lv_obj_add_flag(drawer_hint_, LV_OBJ_FLAG_HIDDEN);


    lv_obj_add_event_cb(screen, &LcdDisplay::OnUserActivity, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(screen, &LcdDisplay::OnUserActivity, LV_EVENT_CLICKED, this);

    chat_lines_per_page_ = 5;  // idle layout: five lines + media entry icon
    ApplyChatLayout();

#if CONFIG_FEATURE_BLE_KEYBOARD
    // Bring up the BLE HID media keyboard and reflect the link state.
    MediaBle::GetInstance().SetStatusCallback([this](bool connected) {
        Application::GetInstance().Schedule([this, connected]() {
            DisplayLockGuard lock(this);
            if (media_bt_icon_ != nullptr) {
                lv_obj_set_style_text_color(
                    media_bt_icon_, connected ? lv_color_hex(0x4CD964) : lv_color_hex(0x777777), 0);
            }
            if (bluetooth_screen_visible_) {
                RefreshBluetoothList();
            }
        });
    });
    {
        Settings bt_settings("bluetooth", false);
        bt_user_enabled_ = bt_settings.GetBool("enabled", true);
    }
    if (bt_user_enabled_) {
        MediaBle::GetInstance().Start();
    }
#endif  // CONFIG_FEATURE_BLE_KEYBOARD

#if CONFIG_FEATURE_WEB_FILE_TRANSFER
    // File transfer page mirrors the server state and upload progress.
    WebFileServer::GetInstance().SetStatusCallback([this](bool running) {
        Application::GetInstance().Schedule([this, running]() {
            DisplayLockGuard lock(this);
            (void)running;
            if (file_service_visible_) {
                UpdateFileServiceValues();
            }
        });
    });
    WebFileServer::GetInstance().SetActivityCallback(
        [this](const std::string& name, size_t sent, size_t total, bool done, bool error) {
            const int percent = total > 0 ? static_cast<int>(sent * 100 / total) : -1;
            Application::GetInstance().Schedule([this, name, percent, done, error]() {
                DisplayLockGuard lock(this);
                if (file_service_visible_) {
                    UpdateFileTransferActivity(name, percent, done, error);
                }
            });
        });
#endif

    // 25 fps avatar animation: breathing, blink and state-driven motion.
    avatar_anim_epoch_ = lv_tick_get();
    avatar_blink_at_ = avatar_anim_epoch_ + 2600;
    avatar_anim_timer_ = lv_timer_create(&LcdDisplay::AvatarAnimTimerCb, 40, this);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but chat label not created)",
                     role, content);
        }
        return;
    }
    // State changes send empty strings; they must never wipe the history.
    if (content == nullptr || content[0] == '\0') {
        return;
    }

    const std::string role_str = role != nullptr ? role : "";
    const std::string text = content;

    // System notices (network, OTA, volume...) are transient: show them in the
    // top bar and keep the conversation pages free of them.
    if (role_str == "system") {
        ShowNotification(text, 3000);
        return;
    }

    const bool was_reviewing = !follow_latest_ && page_index_ >= 0;
    if (role_str == "assistant" && !chat_messages_.empty() &&
        chat_messages_.back().role == "assistant") {
        // Streamed assistant sentences extend the current reply.
        std::string& current = chat_messages_.back().text;
        current += text;
        TrimChatText(current, kMaxChatTextBytes);
        RewrapMessage(chat_messages_.size() - 1);
    } else {
        ChatMessage message;
        message.role = role_str;
        message.text = std::string(RolePrefix(role_str)) + text;
        TrimChatText(message.text, kMaxChatTextBytes);
        chat_messages_.push_back(std::move(message));
        while (chat_messages_.size() > kMaxChatMessages) {
            chat_messages_.pop_front();
        }
        RewrapMessage(chat_messages_.size() - 1);
    }

    RebuildChatPages();
    follow_latest_ = true;
    page_index_ = chat_pages_.empty() ? -1 : static_cast<int>(chat_pages_.size()) - 1;
    RenderCurrentPage();
    if (was_reviewing) {
        ShowNotification("新消息", 1200);
    }
}

void LcdDisplay::RepairDanglingFonts(const lv_font_t* text_font, const lv_font_t* icon_font,
                                     const lv_font_t* large_icon_font) {
    // The set of fonts that are still alive after the assets were replaced.
    // Anything else in a label is a pointer into freed memory.
    const lv_font_t* live[] = {text_font, icon_font, large_icon_font, &font_zzz_30,
#if CONFIG_FEATURE_STROKE_ORDER
                               &font_pinyin_20,
#endif
#if CONFIG_FEATURE_ENGLISH_DICTATION
                               &font_english_20,
#endif
    };
    int repaired = 0;

    // Walk the whole tree: hidden pages are laid out too, so a label inside a
    // hidden page can still crash the device.
    std::function<void(lv_obj_t*)> walk = [&](lv_obj_t* obj) {
        if (obj == nullptr) {
            return;
        }
        if (lv_obj_check_type(obj, &lv_label_class)) {
            const lv_font_t* current = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
            bool alive = false;
            for (const lv_font_t* candidate : live) {
                if (candidate != nullptr && current == candidate) {
                    alive = true;
                    break;
                }
            }
            if (!alive && current != nullptr) {
                lv_obj_set_style_text_font(obj, text_font, 0);
                repaired++;
            }
        }
        const uint32_t count = lv_obj_get_child_count(obj);
        for (uint32_t i = 0; i < count; ++i) {
            walk(lv_obj_get_child(obj, static_cast<int32_t>(i)));
        }
    };
    walk(lv_screen_active());

    if (repaired > 0) {
        ESP_LOGW(TAG, "修复了 %d 个指向失效字体的标签", repaired);
    }
}

const lv_font_t* LcdDisplay::SafeTextFont() const {
    // The theme font is kept alive by LvglDisplay::SetTextFont() until every
    // style has been rebound, so it is the right font to use here.  The chat
    // font is only a subset (no CJK) and must not be used for the UI.
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr &&
        lvgl_theme->text_font()->font() != nullptr) {
        return lvgl_theme->text_font()->font();
    }
    return nullptr;
}

const lv_font_t* LcdDisplay::GetChatFont() {
    // Chat now renders with the same 16px UI font: the dedicated 18px chat
    // font cost 1.09 MB of the assets partition for no real benefit.
    return SafeTextFont();
}

void LcdDisplay::RewrapMessage(size_t index) {
    if (index >= chat_messages_.size() || chat_area_ == nullptr) {
        return;
    }
    const lv_font_t* font = GetChatFont();
    if (font == nullptr) {
        return;
    }
    const lv_coord_t width = lv_obj_get_width(chat_area_);
    if (width <= 0) {
        return;
    }
    WrapChatText(chat_messages_[index].text, font, width, chat_messages_[index].line_starts);
}

void LcdDisplay::RewrapAllMessages() {
    for (size_t i = 0; i < chat_messages_.size(); ++i) {
        RewrapMessage(i);
    }
}

void LcdDisplay::RebuildChatPages() {
    chat_pages_.clear();
    for (size_t i = 0; i < chat_messages_.size(); ++i) {
        const int lines = static_cast<int>(chat_messages_[i].line_starts.size());
        const int pages = std::max(1, (lines + chat_lines_per_page_ - 1) / chat_lines_per_page_);
        for (int page = 0; page < pages; ++page) {
            chat_pages_.push_back(ChatPage{static_cast<uint16_t>(i),
                                           static_cast<uint16_t>(page * chat_lines_per_page_)});
        }
    }
    if (chat_pages_.empty()) {
        page_index_ = -1;
    } else if (page_index_ >= static_cast<int>(chat_pages_.size())) {
        page_index_ = static_cast<int>(chat_pages_.size()) - 1;
    }
}

std::string LcdDisplay::BuildPageText(const ChatPage& page) const {
    if (page.message >= chat_messages_.size()) {
        return "";
    }
    const ChatMessage& message = chat_messages_[page.message];
    if (message.line_starts.empty()) {
        return message.text;
    }
    const int first = page.line;
    const int last =
        std::min<int>(first + chat_lines_per_page_, static_cast<int>(message.line_starts.size()));
    std::string out;
    for (int line = first; line < last; ++line) {
        const uint32_t start = message.line_starts[line];
        const uint32_t end = line + 1 < static_cast<int>(message.line_starts.size())
                                 ? message.line_starts[line + 1]
                                 : static_cast<uint32_t>(message.text.size());
        size_t trimmed_end = end;
        while (trimmed_end > start &&
               (message.text[trimmed_end - 1] == '\n' || message.text[trimmed_end - 1] == '\r')) {
            --trimmed_end;
        }
        if (!out.empty()) {
            out += '\n';
        }
        out.append(message.text, start, trimmed_end - start);
    }
    return out;
}

void LcdDisplay::RenderCurrentPage() {
    if (chat_message_label_ == nullptr) {
        return;
    }
    if (page_index_ < 0 || page_index_ >= static_cast<int>(chat_pages_.size())) {
        lv_label_set_text(chat_message_label_, "");
        return;
    }
    const std::string text = BuildPageText(chat_pages_[page_index_]);
    lv_label_set_text(chat_message_label_, text.c_str());
}

void LcdDisplay::ShowPageIndicator() {
    if (chat_pages_.empty() || page_index_ < 0) {
        return;
    }
    const std::string indicator =
        std::to_string(page_index_ + 1) + "/" + std::to_string(chat_pages_.size());
    ShowNotification(indicator, 1200);
}

void LcdDisplay::GoToPage(int index, bool show_indicator) {
    if (chat_pages_.empty()) {
        return;
    }
    const int last = static_cast<int>(chat_pages_.size()) - 1;
    if (index < 0) {
        index = 0;
    }
    if (index > last) {
        index = last;
    }
    if (index != page_index_) {
        page_index_ = index;
        follow_latest_ = (index == last);
        RenderCurrentPage();
    }
    if (show_indicator) {
        ShowPageIndicator();
    }
}

void LcdDisplay::ApplyChatLayout() {
    if (chat_area_ == nullptr || chat_message_label_ == nullptr || top_bar_ == nullptr) {
        return;
    }
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* font = SafeTextFont();
    if (font == nullptr) {
        auto text_font = lvgl_theme->text_font();
        if (text_font == nullptr || text_font->font() == nullptr) {
            return;
        }
        font = text_font->font();
    }
    const lv_font_t* large_icon_font =
        lvgl_theme->large_icon_font() != nullptr ? lvgl_theme->large_icon_font()->font() : nullptr;
    RebindStrokeFonts(
        font, lvgl_theme->icon_font() != nullptr ? lvgl_theme->icon_font()->font() : nullptr,
        large_icon_font);
    RebindSettingsFonts(
        font, lvgl_theme->icon_font() != nullptr ? lvgl_theme->icon_font()->font() : nullptr,
        lvgl_theme->large_icon_font() != nullptr ? lvgl_theme->large_icon_font()->font() : nullptr);
    RebindEnglishFonts(
        font, lvgl_theme->icon_font() != nullptr ? lvgl_theme->icon_font()->font() : nullptr);
    /* Portrait 240x320 canvas: top bar (one line), avatar, then five text lines.
       The avatar sits right under the top bar and the text keeps a readable gap
       below the avatar. */
    const lv_coord_t line_h = font->line_height;
    const lv_coord_t top_h = line_h;
    const lv_coord_t icon_w =
        lvgl_theme->icon_font() != nullptr && lvgl_theme->icon_font()->font() != nullptr
            ? lvgl_theme->icon_font()->font()->line_height
            : 20;
    const lv_coord_t center_w = LV_HOR_RES - 2 * (kTopBarPad + 2 * icon_w + kTopBarIconGap);

    lv_obj_set_size(top_bar_, LV_HOR_RES, top_h);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_align(network_label_, LV_ALIGN_LEFT_MID, kTopBarPad, 0);
    lv_obj_align(mute_label_, LV_ALIGN_RIGHT_MID, -(kTopBarPad + icon_w + kTopBarIconGap), 0);
    lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -kTopBarPad, 0);

    lv_obj_set_style_text_font(status_label_, font, 0);
    lv_obj_set_width(status_label_, center_w);
    lv_obj_set_height(status_label_, top_h);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_style_text_font(notification_label_, font, 0);
    lv_obj_set_width(notification_label_, center_w);
    lv_obj_set_height(notification_label_, top_h);
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);

    const lv_font_t* chat_font = GetChatFont();
    const lv_coord_t chat_line_h =
        chat_font != nullptr ? chat_font->line_height : static_cast<lv_coord_t>(30);
    const lv_coord_t chat_line_space = 4;
    const lv_coord_t chat_pitch = chat_line_h + chat_line_space;
    const lv_coord_t chat_w = LV_HOR_RES - 2 * kChatSidePad;
    const lv_coord_t chat_h = (chat_lines_per_page_ - 1) * chat_pitch + chat_line_h;
    const lv_coord_t avatar_y = top_h + kAvatarTopGap;
    const lv_coord_t text_top = avatar_y + kAvatarSize + kAvatarGap;
    const lv_coord_t bottom_reserve = chat_lines_per_page_ == 5 ? 46 : 10;
    const lv_coord_t text_space = LV_VER_RES - bottom_reserve - text_top;
    const lv_coord_t chat_y = text_top + LV_MAX((text_space - chat_h) / 2, 0);

    const bool big_idle = avatar_mood_ == kAvatarMoodIdle;
    const lv_coord_t face_size = big_idle ? 150 : kAvatarSize;
    avatar_face_scale_ = static_cast<float>(face_size) / static_cast<float>(kAvatarSize);
    lv_obj_set_size(emoji_box_, face_size, face_size);
    if (big_idle) {
        // Idle: one large face drawn at native resolution in the middle.
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 8);
        lv_obj_add_flag(chat_area_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_ext_click_area(emoji_box_, 30);
    } else {
        lv_obj_align(emoji_box_, LV_ALIGN_TOP_MID, 0, avatar_y);
        lv_obj_remove_flag(chat_area_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_ext_click_area(emoji_box_, 8);
    }

    if (page_progress_ != nullptr) {
        lv_obj_set_size(page_progress_, face_size + 14, face_size + 14);
        lv_obj_align_to(page_progress_, emoji_box_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(page_progress_, LV_OBJ_FLAG_HIDDEN);
    }
    if (avatar_think_dots_ != nullptr) {
        lv_obj_align_to(avatar_think_dots_, emoji_box_, LV_ALIGN_TOP_RIGHT, 6, -4);
    }
    if (sleep_zzz_ != nullptr) {
        lv_obj_align_to(sleep_zzz_, emoji_box_, LV_ALIGN_TOP_MID, 26, -26);
    }
    if (media_bar_ != nullptr) {
        lv_obj_align(media_bar_, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    lv_obj_set_pos(chat_area_, kChatSidePad, chat_y);
    lv_obj_set_size(chat_area_, chat_w, chat_h);

    if (chat_font != nullptr) {
        lv_obj_set_style_text_font(chat_message_label_, chat_font, 0);
    }
    if (avatar_think_dots_ != nullptr) {
        // Rebind: SetTextFont() replaces the theme font when assets load.
        lv_obj_set_style_text_font(avatar_think_dots_, font, 0);
        lv_obj_set_style_text_color(avatar_think_dots_, lvgl_theme->text_color(), 0);
    }
#if CONFIG_FEATURE_BLE_KEYBOARD
    lv_obj_t* themed_labels[] = {media_title_, media_hint_, bt_title_,
                                 bt_empty_,    bt_status_,  bt_disconnect_label_};
    for (lv_obj_t* label : themed_labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
        }
    }
    auto keyboard_label_color = [&](lv_obj_t* label, bool numpad) {
        lv_obj_t* key = lv_obj_get_parent(label);
        const uintptr_t encoded =
            reinterpret_cast<uintptr_t>(lv_obj_get_user_data(key));
        const uint8_t code = static_cast<uint8_t>(encoded & 0xFF);
        const uint8_t mods = static_cast<uint8_t>((encoded >> 8) & 0xFF);
        if (!numpad) {
            if (code == 0x28) {
                return lv_color_hex(kDarkTealInk);
            }
            if (code == 0xFD || code == 0xFE || code == 0xFF) {
                return lv_color_hex(kDarkTeal);
            }
        } else if (code == 0x38 || code == 0x2D ||
                   (code == 0x25 && mods == 0x02) || (code == 0x2E && mods == 0x02)) {
            return lv_color_hex(kDarkTeal);
        }
        return lvgl_theme->text_color();
    };
    for (int i = 0; i < keyboard_label_count_; ++i) {
        lv_obj_set_style_text_font(keyboard_labels_[i], font, 0);
        lv_obj_set_style_text_color(keyboard_labels_[i], keyboard_label_color(keyboard_labels_[i], false), 0);
    }
    for (int i = 0; i < numpad_label_count_; ++i) {
        lv_obj_set_style_text_font(numpad_labels_[i], font, 0);
        lv_obj_set_style_text_color(numpad_labels_[i], keyboard_label_color(numpad_labels_[i], true), 0);
    }
    if (large_icon_font != nullptr) {
        for (int i = 0; i < numpad_icon_label_count_; ++i) {
            lv_obj_set_style_text_font(numpad_icon_labels_[i], large_icon_font, 0);
            lv_obj_set_style_text_color(numpad_icon_labels_[i], lv_color_hex(kDarkTeal), 0);
        }
    }
#endif  // CONFIG_FEATURE_BLE_KEYBOARD
    lv_obj_set_style_text_line_space(chat_message_label_, chat_line_space, 0);
    lv_obj_set_width(chat_message_label_, chat_w);

    RewrapAllMessages();
    RebuildChatPages();
    if (!chat_messages_.empty()) {
        if (follow_latest_ || page_index_ < 0) {
            page_index_ = static_cast<int>(chat_pages_.size()) - 1;
        } else if (page_index_ >= static_cast<int>(chat_pages_.size())) {
            page_index_ = static_cast<int>(chat_pages_.size()) - 1;
        }
        RenderCurrentPage();
    } else {
        page_index_ = -1;
        lv_label_set_text(chat_message_label_, "");
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::RebindStrokeFonts(const lv_font_t* text_font, const lv_font_t* icon_font,
                                   const lv_font_t* large_icon_font) {
#if CONFIG_FEATURE_STROKE_ORDER
    (void)large_icon_font;
    // The stroke page is built once in SetupUI with the theme fonts of that
    // moment.  When assets reload and the old fonts are freed we must move all
    // of its labels to the new fonts *before* any style change triggers a
    // label relayout, otherwise LVGL calls through a stale font pointer.
    lv_obj_t* text_labels[] = {stroke_pinyin_label_, stroke_info_label_};
    for (lv_obj_t* label : text_labels) {
        if (label != nullptr && text_font != nullptr) {
            lv_obj_set_style_text_font(label, text_font, 0);
        }
    }
    for (int i = 0; i < kStrokeMaxStrokes; ++i) {
        if (stroke_badge_labels_[i] != nullptr && text_font != nullptr) {
            lv_obj_set_style_text_font(stroke_badge_labels_[i], text_font, 0);
        }
    }
    if (stroke_close_icon_ != nullptr && icon_font != nullptr) {
        lv_obj_set_style_text_font(stroke_close_icon_, icon_font, 0);
    }
#else
    (void)text_font;
    (void)icon_font;
    (void)large_icon_font;
#endif
}

void LcdDisplay::ShowSettingsScreen(bool show) {
    if (settings_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    settings_screen_visible_ = show;
    if (show) {
        UpdateSettingsValues();
        lv_obj_remove_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(settings_screen_);
    } else {
        lv_obj_add_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::UpdateSettingsValues() {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    const lv_color_t text_color = theme != nullptr ? theme->text_color() : lv_color_hex(0xE8EAF0);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr && settings_volume_value_ != nullptr) {
        lv_label_set_text_fmt(settings_volume_value_, "%d%%", codec->output_volume());
    }
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr && settings_bright_value_ != nullptr) {
        lv_label_set_text_fmt(settings_bright_value_, "%d%%", backlight->brightness());
    }
    if (codec != nullptr && settings_mic_value_ != nullptr) {
        lv_label_set_text_fmt(settings_mic_value_, "%ddB",
                              static_cast<int>(codec->input_gain()));
    }
    auto& wifi = WifiManager::GetInstance();
    const bool wifi_connected = wifi.IsConnected();
    if (settings_wifi_status_ != nullptr) {
        if (wifi_connected) {
            lv_label_set_text(settings_wifi_status_, "已连接");
        } else if (wifi.IsConfigMode()) {
            lv_label_set_text(settings_wifi_status_, "配网模式");
        } else {
            lv_label_set_text(settings_wifi_status_, "未连接");
        }
    }
    if (settings_wifi_ssid_ != nullptr) {
        if (wifi_connected) {
            lv_label_set_text(settings_wifi_ssid_, wifi.GetSsid().c_str());
            lv_obj_set_style_text_color(settings_wifi_ssid_, lv_color_hex(kDarkMuted), 0);
        } else {
            lv_label_set_text(settings_wifi_ssid_, "--");
            lv_obj_set_style_text_color(settings_wifi_ssid_, lv_color_hex(kDarkDim), 0);
        }
    }
    if (settings_wifi_dot_ != nullptr) {
        lv_obj_set_style_bg_color(settings_wifi_dot_,
                                  lv_color_hex(wifi_connected ? kDarkGood : kDarkDim), 0);
    }
#if CONFIG_FEATURE_BLE_KEYBOARD
    if (settings_bt_status_ != nullptr) {
        const bool connected = MediaBle::GetInstance().IsConnected() && bt_user_enabled_;
        const char* text = "未连接";
        uint32_t color = kDarkMuted;
        if (bt_busy_) {
            text = bt_user_enabled_ ? "正在开启…" : "正在关闭…";
        } else if (!bt_user_enabled_) {
            text = "已关闭";
            color = kDarkDim;
        } else if (connected) {
            text = "已连接";
            color = kDarkGood;
        }
        lv_label_set_text(settings_bt_status_, text);
        lv_obj_set_style_text_color(settings_bt_status_, lv_color_hex(color), 0);
    }
    if (settings_bt_icon_ != nullptr) {
        const bool bt_on = bt_user_enabled_ && !bt_busy_;
        lv_obj_set_style_text_color(settings_bt_icon_,
                                    lv_color_hex(bt_on ? kDarkPurple : kDarkDim), 0);
    }
    if (settings_bt_label_ != nullptr) {
        const bool bt_on = bt_user_enabled_ && !bt_busy_;
        lv_obj_set_style_text_color(settings_bt_label_, bt_on ? text_color : lv_color_hex(kDarkDim),
                                    0);
    }
    if (settings_bt_switch_ != nullptr) {
        if (bt_user_enabled_) {
            lv_obj_add_state(settings_bt_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(settings_bt_switch_, LV_STATE_CHECKED);
        }
        lv_obj_set_style_opa(settings_bt_switch_, bt_busy_ ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
    if (settings_bt_forget_label_ != nullptr) {
        lv_obj_t* btn = lv_obj_get_parent(settings_bt_forget_label_);
        const bool usable = bt_user_enabled_ && !bt_busy_;
        lv_obj_set_style_text_color(settings_bt_forget_label_,
                                    usable ? text_color : lv_color_hex(0x5A6070), 0);
        if (btn != nullptr) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(usable ? kDarkCardControl : 0x1B1D26), 0);
            if (usable) {
                lv_obj_remove_state(btn, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(btn, LV_STATE_DISABLED);
            }
        }
    }
#endif  // CONFIG_FEATURE_BLE_KEYBOARD
    if (settings_ip_value_ != nullptr) {
        const std::string ip = wifi_connected ? wifi.GetIpAddress() : std::string();
        if (ip.empty()) {
            lv_label_set_text(settings_ip_value_, "--");
            lv_obj_set_style_text_color(settings_ip_value_, lv_color_hex(kDarkDim), 0);
        } else {
            lv_label_set_text(settings_ip_value_, ip.c_str());
            lv_obj_set_style_text_color(settings_ip_value_, lv_color_hex(kDarkMuted), 0);
        }
    }
}

#if CONFIG_FEATURE_WEB_FILE_TRANSFER
void LcdDisplay::ShowFileServiceScreen(bool show) {
    if (file_service_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    file_service_visible_ = show;
    if (show) {
        UpdateFileServiceValues();
        if (file_activity_label_ != nullptr) {
            lv_label_set_text(file_activity_label_, "等待文件");
            lv_obj_set_style_text_color(file_activity_label_, lv_color_hex(0xFFFFFF), 0);
        }
        if (file_transfer_state_label_ != nullptr) {
            lv_label_set_text(file_transfer_state_label_, "等待上传");
            lv_obj_set_style_text_color(file_transfer_state_label_, lv_color_hex(kDarkMuted), 0);
        }
        if (file_progress_bar_ != nullptr) {
            lv_bar_set_value(file_progress_bar_, 0, LV_ANIM_OFF);
        }
        lv_obj_remove_flag(file_service_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(file_service_screen_);
    } else {
        lv_obj_add_flag(file_service_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::UpdateFileServiceValues() {
    if (file_service_screen_ == nullptr) {
        return;
    }
    auto& server = WebFileServer::GetInstance();
    auto& wifi = WifiManager::GetInstance();
    const bool running = server.IsRunning();
    const bool online = wifi.IsConnected();
    const std::string url = server.Url();
    if (file_url_label_ != nullptr) {
        lv_label_set_text(file_url_label_,
                          url.empty() ? (online ? "服务已停止" : "未联网") : url.c_str());
    }
    if (file_status_dot_ != nullptr) {
        lv_obj_set_style_bg_color(file_status_dot_,
                                  lv_color_hex(running ? kDarkGood : kDarkDim), 0);
    }
    if (file_status_label_ != nullptr) {
        lv_label_set_text(file_status_label_,
                          running ? "运行中" : (online ? "已停止" : "等待网络"));
    }
    if (file_toggle_label_ != nullptr) {
        lv_label_set_text(file_toggle_label_, running ? "停止" : "启动");
    }
    if (file_wifi_label_ != nullptr) {
        const std::string ssid = online ? wifi.GetSsid() : std::string();
        lv_label_set_text(file_wifi_label_, ssid.empty() ? "--" : ssid.c_str());
    }
    if (file_ip_label_ != nullptr) {
        const std::string ip = online ? wifi.GetIpAddress() : std::string();
        lv_label_set_text(file_ip_label_, ip.empty() ? "--" : ip.c_str());
    }
    if (file_wifi_dot_ != nullptr) {
        lv_obj_set_style_bg_color(file_wifi_dot_,
                                  lv_color_hex(online ? kDarkGood : kDarkDim), 0);
    }
    if (file_storage_label_ != nullptr) {
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        char text[64];
        if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) == ESP_OK &&
            total_bytes > 0) {
            const double gb = 1024.0 * 1024 * 1024;
            snprintf(text, sizeof(text), "%.1f / %.1f GB 可用",
                     static_cast<double>(free_bytes) / gb, static_cast<double>(total_bytes) / gb);
            lv_label_set_text(file_storage_label_, text);
            lv_obj_set_style_text_color(file_storage_label_, lv_color_hex(kDarkMuted), 0);
            if (file_storage_bar_ != nullptr) {
                const uint64_t used_bytes = total_bytes - free_bytes;
                const int percent =
                    static_cast<int>((used_bytes * 100) / total_bytes);
                lv_bar_set_value(file_storage_bar_, percent, LV_ANIM_OFF);
            }
        } else {
            lv_label_set_text(file_storage_label_, "未插入 SD 卡");
            lv_obj_set_style_text_color(file_storage_label_, lv_color_hex(kDarkDim), 0);
            if (file_storage_bar_ != nullptr) {
                lv_bar_set_value(file_storage_bar_, 0, LV_ANIM_OFF);
            }
        }
    }
}

void LcdDisplay::UpdateFileTransferActivity(const std::string& name, int percent, bool done,
                                            bool error) {
    if (file_activity_label_ == nullptr) {
        return;
    }
    lv_label_set_text(file_activity_label_, name.empty() ? "等待文件" : name.c_str());
    lv_obj_set_style_text_color(file_activity_label_, lv_color_hex(0xFFFFFF), 0);
    if (file_transfer_state_label_ != nullptr) {
        char state[32];
        uint32_t color = kDarkMuted;
        if (error) {
            snprintf(state, sizeof(state), "传输失败");
            color = kDarkDanger;
        } else if (done) {
            snprintf(state, sizeof(state), "已保存");
            color = kDarkGood;
        } else if (percent >= 0) {
            snprintf(state, sizeof(state), "上传中 %d%%", percent);
        } else {
            snprintf(state, sizeof(state), "等待上传");
        }
        lv_label_set_text(file_transfer_state_label_, state);
        lv_obj_set_style_text_color(file_transfer_state_label_, lv_color_hex(color), 0);
    }
    if (file_progress_bar_ != nullptr) {
        const int value = error ? 0 : (done ? 100 : (percent > 0 ? percent : 0));
        lv_bar_set_value(file_progress_bar_, value, LV_ANIM_OFF);
    }
}

void LcdDisplay::OnFileServiceBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowFileServiceScreen(false);
        self->CloseSubPageToDrawer();
    }
}

void LcdDisplay::OnFileServiceToggleClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    auto& server = WebFileServer::GetInstance();
    if (server.IsRunning()) {
        server.Stop();
        if (self->file_activity_label_ != nullptr) {
            lv_label_set_text(self->file_activity_label_, "等待文件");
        }
        if (self->file_transfer_state_label_ != nullptr) {
            lv_label_set_text(self->file_transfer_state_label_, "等待上传");
            lv_obj_set_style_text_color(self->file_transfer_state_label_,
                                        lv_color_hex(kDarkMuted), 0);
        }
        if (self->file_progress_bar_ != nullptr) {
            lv_bar_set_value(self->file_progress_bar_, 0, LV_ANIM_OFF);
        }
    } else {
        server.Start();
    }
    self->UpdateFileServiceValues();
}

#else
void LcdDisplay::ShowFileServiceScreen(bool show) { (void)show; }
void LcdDisplay::UpdateFileServiceValues() {}
void LcdDisplay::UpdateFileTransferActivity(const std::string&, int, bool, bool) {}
#endif  // CONFIG_FEATURE_WEB_FILE_TRANSFER

void LcdDisplay::SetIpAddress(const std::string& ip) {
    (void)ip;
    Application::GetInstance().Schedule([this]() {
        DisplayLockGuard lock(this);
        if (settings_screen_visible_) {
            UpdateSettingsValues();
        }
        if (file_service_visible_) {
            UpdateFileServiceValues();
        }
    });
}

void LcdDisplay::RebindSettingsFonts(const lv_font_t* text_font, const lv_font_t* icon_font,
                                     const lv_font_t* large_icon_font) {
    lv_obj_t* text_labels[] = {settings_title_,
                               settings_volume_label_,
                               settings_volume_value_,
                               settings_bright_label_,
                               settings_bright_value_,
                               settings_mic_label_,
                               settings_mic_value_,
                               settings_wifi_label_,
                               settings_wifi_status_,
                               settings_wifi_ssid_,
                               settings_bt_label_,
                               settings_bt_status_,
                               settings_vol_minus_icon_,
                               settings_vol_plus_icon_,
                               settings_bright_minus_icon_,
                               settings_bright_plus_icon_,
                               settings_mic_minus_icon_,
                               settings_mic_plus_icon_,
                               settings_wifi_btn_label_,
                               settings_bt_forget_label_,
                               settings_reboot_label_,
                               settings_ip_label_,
                               settings_ip_value_,
                               file_title_label_,
                               file_service_name_label_,
                               file_url_label_,
                               file_hint_label_,
                               file_status_label_,
                               file_toggle_label_,
                               file_wifi_label_,
                               file_ip_label_,
                               file_storage_title_label_,
                               file_storage_label_,
                               file_activity_label_,
                               file_transfer_state_label_,
                               file_foot_label_,
                               drawer_title_,
                               drawer_hint_};
    for (lv_obj_t* label : text_labels) {
        if (label != nullptr && text_font != nullptr) {
            lv_obj_set_style_text_font(label, text_font, 0);
        }
    }
    for (int i = 0; i < kDrawerTileCount; ++i) {
        if (drawer_tile_labels_[i] != nullptr && text_font != nullptr) {
            lv_obj_set_style_text_font(drawer_tile_labels_[i], text_font, 0);
        }
        if (drawer_tile_icons_[i] != nullptr && large_icon_font != nullptr) {
            lv_obj_set_style_text_font(drawer_tile_icons_[i], large_icon_font, 0);
        }
    }
    lv_obj_t* icon_labels[] = {settings_back_icon_, drawer_back_icon_, file_back_icon_};
    for (lv_obj_t* label : icon_labels) {
        if (label != nullptr && icon_font != nullptr) {
            lv_obj_set_style_text_font(label, icon_font, 0);
        }
    }
    for (lv_obj_t* label : settings_icon_labels_) {
        if (label != nullptr && icon_font != nullptr) {
            lv_obj_set_style_text_font(label, icon_font, 0);
        }
    }
    for (lv_obj_t* label : file_icon_labels_) {
        if (label != nullptr && icon_font != nullptr) {
            lv_obj_set_style_text_font(label, icon_font, 0);
        }
    }
}

void LcdDisplay::OnSettingsBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowSettingsScreen(false);
        self->CloseSubPageToDrawer();
    }
}

void LcdDisplay::CloseSubPageToDrawer() {
    if (!return_to_drawer_) {
        return;
    }
    return_to_drawer_ = false;
    ShowDrawerScreen(true);
}

void LcdDisplay::OnVolumeDownClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (self != nullptr && codec != nullptr) {
        int volume = codec->output_volume() - 5;
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        self->UpdateSettingsValues();
    }
}

void LcdDisplay::OnVolumeUpClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (self != nullptr && codec != nullptr) {
        int volume = codec->output_volume() + 5;
        if (volume > 100) {
            volume = 100;
        }
        codec->SetOutputVolume(volume);
        self->UpdateSettingsValues();
    }
}

void LcdDisplay::OnBrightnessDownClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    auto* backlight = Board::GetInstance().GetBacklight();
    if (self != nullptr && backlight != nullptr) {
        int brightness = static_cast<int>(backlight->brightness()) - 10;
        if (brightness < 10) {
            brightness = 10;
        }
        backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
        self->UpdateSettingsValues();
    }
}

void LcdDisplay::OnBrightnessUpClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    auto* backlight = Board::GetInstance().GetBacklight();
    if (self != nullptr && backlight != nullptr) {
        int brightness = static_cast<int>(backlight->brightness()) + 10;
        if (brightness > 100) {
            brightness = 100;
        }
        backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
        self->UpdateSettingsValues();
    }
}

void LcdDisplay::OnMicGainDownClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (self != nullptr && codec != nullptr) {
        int gain = static_cast<int>(codec->input_gain()) - 6;
        if (gain < 0) {
            gain = 0;
        }
        codec->SetInputGain(static_cast<float>(gain));
        self->UpdateSettingsValues();
    }
}

void LcdDisplay::OnMicGainUpClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (self != nullptr && codec != nullptr) {
        int gain = static_cast<int>(codec->input_gain()) + 6;
        if (gain > 36) {
            gain = 36;
        }
        codec->SetInputGain(static_cast<float>(gain));
        self->UpdateSettingsValues();
    }
}

void LcdDisplay::OnWifiConfigClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->ShowSettingsScreen(false);
    auto* wifi_board = static_cast<WifiBoard*>(&Board::GetInstance());
    wifi_board->EnterWifiConfigMode();
}

#if CONFIG_FEATURE_BLE_KEYBOARD
void LcdDisplay::OnSettingsBtToggleClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr || self->bt_busy_) {
        return;
    }
    self->bt_user_enabled_ = !self->bt_user_enabled_;
    self->bt_busy_ = true;
    Settings settings("bluetooth", true);
    settings.SetBool("enabled", self->bt_user_enabled_);
    self->UpdateSettingsValues();

    // BT init spins up code that briefly disables the flash cache, so this task
    // must run on an internal-RAM stack; a PSRAM stack trips
    // esp_task_stack_is_sane_cache_disabled().
    if (xTaskCreate(&LcdDisplay::BtToggleTask, "bt_toggle", 5120, self, 5, nullptr) != pdPASS) {
        self->bt_user_enabled_ = !self->bt_user_enabled_;
        self->bt_busy_ = false;
        settings.SetBool("enabled", self->bt_user_enabled_);
        self->UpdateSettingsValues();
        ESP_LOGE(TAG, "failed to spawn bt toggle task");
    }
}

void LcdDisplay::BtToggleTask(void* param) {
    auto* self = static_cast<LcdDisplay*>(param);
    if (self == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    const bool enable = self->bt_user_enabled_;
    ESP_LOGW(TAG, "bluetooth %s requested, internal free=%u, psram free=%u", enable ? "enable" : "disable",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    if (enable) {
        MediaBle::GetInstance().Start();
    } else {
        MediaBle::GetInstance().Stop();
    }
    ESP_LOGW(TAG, "bluetooth %s done, internal free=%u, psram free=%u", enable ? "enable" : "disable",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    self->bt_busy_ = false;
    Application::GetInstance().Schedule([self]() {
        DisplayLockGuard lock(self);
        self->UpdateSettingsValues();
        if (self->bluetooth_screen_visible_) {
            self->RefreshBluetoothList();
        }
    });
    vTaskDelete(nullptr);
}

void LcdDisplay::OnSettingsBtForgetClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    MediaBlePeer peers[8];
    const int count = MediaBle::GetInstance().GetBondedPeers(peers, 8);
    for (int i = 0; i < count; ++i) {
        MediaBle::GetInstance().Unpair(peers[i]);
    }
    if (self != nullptr) {
        self->UpdateSettingsValues();
    }
}

#endif  // CONFIG_FEATURE_BLE_KEYBOARD

void LcdDisplay::OnRebootClicked(lv_event_t* e) {
    (void)e;
    esp_restart();
}

void LcdDisplay::OnWakeWordDetected() {
    DisplayLockGuard lock(this);
    ESP_LOGI(TAG, "wake/new-turn hook: stroke=%d settings=%d", stroke_screen_visible_,
             settings_screen_visible_);
    return_to_drawer_ = false;
#if CONFIG_FEATURE_ENGLISH_DICTATION
    // In a lesson the wake word only opens a background voice-command turn;
    // keep the lesson page in front and keep the player paused (not stopped).
    if (english_screen_visible_ && !Application::GetInstance().IsDictationActive()) {
        EnglishPlayer::GetInstance().Stop();
        ShowEnglishScreen(false);
    }
    if (english_setup_visible_) {
        ShowEnglishSetup(false);
    }
#endif
    if (stroke_screen_visible_) {
        ShowStrokeScreen(false);
    }
    if (settings_screen_visible_) {
        ShowSettingsScreen(false);
    }
    if (drawer_screen_visible_) {
        ShowDrawerScreen(false);
    }
}

void LcdDisplay::ShowPressRing() {
    if (page_progress_ == nullptr) {
        return;
    }
    lv_arc_set_value(page_progress_, 0);
    lv_obj_remove_flag(page_progress_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::HidePressRing() {
    if (page_progress_ == nullptr) {
        return;
    }
    lv_obj_add_flag(page_progress_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::OnChatAreaClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr || self->chat_area_ == nullptr || self->chat_pages_.empty()) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(self->chat_area_, &area);
    const lv_coord_t middle = (area.x1 + area.x2) / 2;
    if (point.x < middle) {
        self->GoToPage(self->page_index_ - 1, true);
    } else {
        self->GoToPage(self->page_index_ + 1, true);
    }
}

void LcdDisplay::OnAvatarPressed(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowPressRing();
    }
}

void LcdDisplay::OnAvatarReleased(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->HidePressRing();
    }
}

void LcdDisplay::OnMicButtonClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        if (self->stroke_screen_visible_) {
            self->ShowStrokeScreen(false);
        }
        if (self->settings_screen_visible_) {
            self->ShowSettingsScreen(false);
        }
        if (self->drawer_screen_visible_) {
            self->ShowDrawerScreen(false);
        }
    }
    if (self != nullptr && !self->chat_pages_.empty()) {
        // Continue the conversation from the live page.
        self->GoToPage(static_cast<int>(self->chat_pages_.size()) - 1, false);
    }
    Application::GetInstance().ToggleChatState();
}

void LcdDisplay::OnAvatarLongPressed(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->HidePressRing();
    // Swallow the CLICKED event that LVGL still sends on release.
    if (lv_indev_t* indev = lv_indev_active(); indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    // Clearing the screen is the feedback: a new session has started.
    Application::GetInstance().Schedule([]() { Application::GetInstance().StartNewSession(); });
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    HidePressRing();
    chat_messages_.clear();
    chat_pages_.clear();
    page_index_ = -1;
    follow_latest_ = true;
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    ESP_LOGI(TAG, "Chat history cleared (new session)");
}
#endif

void LcdDisplay::MarkUserActivity() {
    last_activity_tick_ = lv_tick_get();
    if (avatar_sleeping_) {
        avatar_sleeping_ = false;
        if (sleep_zzz_ != nullptr) {
            lv_obj_add_flag(sleep_zzz_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::OnUserActivity(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->MarkUserActivity();
    }
}

void LcdDisplay::SetAvatarMood(AvatarMood mood) {
    DisplayLockGuard lock(this);
    if (avatar_mood_ == mood) {
        return;
    }
    const AvatarMood previous_mood = avatar_mood_;
    ESP_LOGI(TAG, "avatar mood %d -> %d (stroke=%d settings=%d)", (int)previous_mood, (int)mood,
             stroke_screen_visible_, settings_screen_visible_);
    MarkUserActivity();
    avatar_mood_ = mood;
    avatar_anim_epoch_ = lv_tick_get();
    avatar_blink_at_ = avatar_anim_epoch_ + 2600 + (esp_random() % 3500);
    avatar_blink_start_ = 0;
    gaze_next_ = 0;
    if (stroke_screen_visible_) {
        // Two moments end the demo page:
        //  - a new turn starts from Idle (wake word, button or touch), and
        //  - the current session really ends (Speaking -> Idle, e.g. goodbye).
        // The automatic Idle->Listening transition inside a turn is
        // Speaking->Listening, so the demo keeps running for normal answers.
        const bool new_turn = previous_mood == kAvatarMoodIdle &&
                              (mood == kAvatarMoodListening || mood == kAvatarMoodThinking);
        // Any active turn falling back to Idle ends the session: goodbye
        // (Speaking -> Idle) and the automatic silence timeout
        // (Listening/Thinking -> Idle) both close the stroke page.
        const bool session_end = mood == kAvatarMoodIdle && previous_mood != kAvatarMoodIdle;
        if (new_turn || session_end) {
            ShowStrokeScreen(false);
        }
    }
    if (english_setup_visible_) {
        // The setup panel is transient; the running dictation page is not.
        const bool new_turn = previous_mood == kAvatarMoodIdle &&
                              (mood == kAvatarMoodListening || mood == kAvatarMoodThinking);
        const bool session_end = mood == kAvatarMoodIdle && previous_mood != kAvatarMoodIdle;
        if (new_turn || session_end) {
            ShowEnglishSetup(false);
        }
    }
    // The dictation page deliberately survives mood changes: it is usually
    // opened by voice ("开始听写"), and the turn ends right after the answer,
    // so closing on Speaking->Idle would make it flash away. It is closed by
    // the close button, self.english.stop, or another full-screen page.
    if (mood != kAvatarMoodIdle) {
        // A conversation started: leave the media/paired/stroke/settings
        // screens so the user sees the dialog immediately.
        ShowMediaScreen(false);
        ShowBluetoothScreen(false);
        if (settings_screen_visible_) {
            ShowSettingsScreen(false);
        }
        ShowEnglishSetup(false);
        if (drawer_screen_visible_) {
            ShowDrawerScreen(false);
        }
    }
    UpdateChatViewport();
}

void LcdDisplay::AvatarAnimTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || self->emoji_box_ == nullptr || self->avatar_face_ == nullptr) {
        return;
    }
    // The preview image temporarily hides the avatar; skip while hidden.
    if (lv_obj_has_flag(self->emoji_box_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (self->media_screen_visible_ || self->bluetooth_screen_visible_ ||
        self->settings_screen_visible_ || self->stroke_screen_visible_ ||
        self->english_screen_visible_) {
        return;  // Overlay covers the AI screen; no need to animate.
    }

    constexpr float kPi = 3.14159265f;
    const uint32_t now = lv_tick_get();
    const float t = (now - self->avatar_anim_epoch_) / 1000.0f;

    AvatarExpression expression = kAvatarExpressionIdle;
    switch (self->avatar_mood_) {
        case kAvatarMoodListening:
            expression = kAvatarExpressionListening;
            break;
        case kAvatarMoodSpeaking:
            expression = kAvatarExpressionSpeaking;
            break;
        case kAvatarMoodThinking:
            expression = kAvatarExpressionThinking;
            break;
        case kAvatarMoodIdle:
        default:
            expression = kAvatarExpressionIdle;
            break;
    }
    if (self->avatar_override_ != kAvatarExpressionNone && now < self->avatar_override_until_) {
        expression = self->avatar_override_;
    }

    constexpr uint32_t kIdleBeforeSleepMs = 20 * 60 * 1000;  // 20 min idle -> doze
    constexpr uint32_t kSleepDurationMs = 5 * 60 * 1000;     // 5 min doze -> wake
    const uint32_t idle_ms = now - self->last_activity_tick_;
    bool sleeping = self->avatar_sleeping_;
    if (sleeping) {
        if (now - self->sleep_start_tick_ >= kSleepDurationMs) {
            // Pomodoro-like cue: wake up with a short happy face and restart the
            // idle timer, so the next doze starts after another 20 minutes.
            self->MarkUserActivity();
            self->avatar_sleeping_ = false;
            self->avatar_override_ = kAvatarExpressionHappy;
            self->avatar_override_until_ = now + 2500;
            sleeping = false;
            ESP_LOGI("Avatar", "sleep cycle finished, back to idle");
        }
    } else if (self->avatar_mood_ == kAvatarMoodIdle && idle_ms > kIdleBeforeSleepMs &&
               self->avatar_override_ == kAvatarExpressionNone) {
        sleeping = true;
        self->sleep_start_tick_ = now;
        ESP_LOGI("Avatar", "idle for 20 minutes, doze off for 5 minutes");
    }
    if (sleeping) {
        expression = kAvatarExpressionClosed;
    }
    self->avatar_sleeping_ = sleeping;

    const bool happy = expression == kAvatarExpressionHappy;
    const bool thinking = expression == kAvatarExpressionThinking;

    // Happy shows two arcs instead of the pill eyes; thinking adds dots.
    if (happy) {
        if (!lv_obj_has_flag(self->avatar_eye_left_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(self->avatar_eye_left_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(self->avatar_eye_right_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(self->avatar_eye_happy_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(self->avatar_eye_happy_right_, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (lv_obj_has_flag(self->avatar_eye_left_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(self->avatar_eye_left_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(self->avatar_eye_right_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(self->avatar_eye_happy_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(self->avatar_eye_happy_right_, LV_OBJ_FLAG_HIDDEN);
    }
    if (thinking) {
        lv_obj_remove_flag(self->avatar_think_dots_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(self->avatar_think_dots_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_coord_t eye_gap = 10;
    lv_coord_t eye_w = 8;
    lv_coord_t eye_h = 22;
    lv_coord_t look_x = 0;
    lv_coord_t look_y = 0;
    lv_coord_t breath = 0;
    lv_coord_t bob = 0;
    lv_coord_t face_tx = 0;
    int32_t face_rot = 0;  // 0.1 degree units
    int32_t eye_rot = 0;   // 0.1 degree units
    bool gaze_active = false;

    switch (expression) {
        case kAvatarExpressionListening:
            gaze_active = true;
            breath = static_cast<lv_coord_t>(12.0f * sinf(t * 2 * kPi / 2.6f));
            break;
        case kAvatarExpressionSpeaking:
            look_x = static_cast<lv_coord_t>(1.5f * sinf(t * 2 * kPi / 1.7f));
            breath = static_cast<lv_coord_t>(5.0f * sinf(t * 2 * kPi / 1.2f));
            bob = -static_cast<lv_coord_t>(fabsf(sinf(t * 2 * kPi / 0.6f)) * 2.0f);
            face_rot = static_cast<int32_t>(100.0f * sinf(t * 2 * kPi / 2.2f));
            break;
        case kAvatarExpressionThinking:
            look_x = 4 + static_cast<lv_coord_t>(0.8f * sinf(t * 2 * kPi / 1.1f));
            look_y = -4;
            breath = static_cast<lv_coord_t>(4.0f * sinf(t * 2 * kPi / 2.6f));
            face_rot = static_cast<int32_t>(300.0f * sinf(t * 2 * kPi / 3.2f));
            break;
        case kAvatarExpressionHappy:
            breath = static_cast<lv_coord_t>(9.0f * sinf(t * 2 * kPi / 0.9f));
            bob = -static_cast<lv_coord_t>(fabsf(sinf(t * 2 * kPi / 0.55f)) * 3.0f);
            face_rot = static_cast<int32_t>(300.0f * sinf(t * 2 * kPi / 1.1f));
            break;
        case kAvatarExpressionIdle:
        default:
            gaze_active = true;
            breath = static_cast<lv_coord_t>(10.0f * sinf(t * 2 * kPi / 3.2f));
            break;
    }

    if (sleeping) {
        gaze_active = false;
        look_x = 0;
        look_y = 0;
        // Slow breathing plus a gentle nod (head sinks periodically).
        const float nod = fmaxf(0.0f, sinf(t * 2 * kPi / 2.4f));
        bob = static_cast<lv_coord_t>(6.0f * nod);
        breath = static_cast<lv_coord_t>(5.0f * sinf(t * 2 * kPi / 6.0f));
        face_rot = static_cast<int32_t>(50.0f * sinf(t * 2 * kPi / 7.0f));
    }

    const bool closed = expression == kAvatarExpressionClosed;
    const bool wink = expression == kAvatarExpressionWink;
    if (closed || wink) {
        if (!lv_obj_has_flag(self->avatar_eye_left_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(self->avatar_eye_left_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(self->avatar_eye_right_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(self->avatar_eye_happy_left_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(self->avatar_eye_happy_right_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_coord_t eye_left_x = -eye_gap;
    lv_coord_t eye_left_y = 0;
    lv_coord_t eye_right_x = eye_gap;
    lv_coord_t eye_right_y = 0;
    lv_coord_t eye_left_w = eye_w;
    lv_coord_t eye_left_h = eye_h;
    lv_coord_t eye_right_w = eye_w;
    lv_coord_t eye_right_h = eye_h;
    int32_t eye_left_rot = eye_rot;
    int32_t eye_right_rot = eye_rot;
    bool have_eye_pos = false;

    if (closed || wink) {
        // Closed eyes are thin, slightly wider bars; wink keeps one open.
        eye_left_w = 10;
        eye_left_h = 3;
        eye_right_w = 10;
        eye_right_h = 3;
        eye_left_rot = 0;
        eye_right_rot = 0;
        if (wink) {
            eye_left_w = eye_w;
            eye_left_h = eye_h;
            eye_left_x = -eye_gap;
        }
        have_eye_pos = true;
        bob = static_cast<lv_coord_t>(fabsf(sinf(t * 2 * kPi / 2.8f)) * 1.0f);
    } else if (gaze_active) {
        if (self->gaze_next_ == 0 || now >= self->gaze_next_) {
            self->PickAvatarGaze();
            self->gaze_next_ = now + 1500 + (esp_random() % 1600);
        }
        float progress = (now - self->gaze_start_) / 240.0f;
        if (progress > 1.0f) {
            progress = 1.0f;
        }
        const float eased = progress * progress * (3.0f - 2.0f * progress);
        self->gaze_x_ = self->gaze_from_x_ + (self->gaze_to_x_ - self->gaze_from_x_) * eased;
        self->gaze_y_ = self->gaze_from_y_ + (self->gaze_to_y_ - self->gaze_from_y_) * eased;
        self->gaze_zoom_ =
            self->gaze_from_zoom_ + (self->gaze_to_zoom_ - self->gaze_from_zoom_) * eased;
        self->gaze_eye_ =
            self->gaze_from_eye_ + (self->gaze_to_eye_ - self->gaze_from_eye_) * eased;
        self->gaze_tilt_ =
            self->gaze_from_tilt_ + (self->gaze_to_tilt_ - self->gaze_from_tilt_) * eased;

        const bool listening = expression == kAvatarExpressionListening;
        const lv_coord_t base_w = listening ? 7 : 8;
        const lv_coord_t base_h = listening ? 20 : 22;
        const lv_coord_t base_gap = listening ? 9 : 10;

        // Position-driven pseudo-3D, per the user's rules:
        //  - centred eyes: almost no perspective difference;
        //  - looking sideways: only a little near/far difference, and the eye
        //    on the side the face turns to (closer to the edge) gets smaller;
        //  - overall size follows the position: down = biggest, up = smaller,
        //    towards either side = smaller, centre = big.
        const float gx = self->gaze_x_ / 7.0f;  // -1..1
        const float gy = self->gaze_y_ / 5.0f;  // -1..1, positive = looking down
        const float abs_gx = fabsf(gx);
        const float asym = 1.0f + 0.16f * gy - 0.10f * abs_gx;  // vertical bias
        const float persp = 0.02f + 0.08f * abs_gx;             // 0.02..0.10
        const float gap_scale = 1.0f - 0.10f * abs_gx;

        for (int side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            const float eye_scale = asym * (1.0f - sign * persp * gx);
            const lv_coord_t x =
                static_cast<lv_coord_t>(sign * base_gap * gap_scale + self->gaze_x_);
            const lv_coord_t y = static_cast<lv_coord_t>(self->gaze_y_);
            const lv_coord_t w = static_cast<lv_coord_t>(base_w * eye_scale * self->gaze_zoom_);
            const lv_coord_t h =
                static_cast<lv_coord_t>(base_h * eye_scale * self->gaze_zoom_ * self->gaze_eye_);
            const float rot_deg = self->gaze_tilt_ * 0.5f + sign * gx * 4.0f;

            // Only the slightly smaller (edge-side) eye is lightened a touch.
            const float far = LV_CLAMP(0.0f, -sign * gx * persp * 0.8f, 0.10f);
            lv_color_t eye_color =
                lv_color_mix(lv_color_hex(kAvatarFaceColor), lv_color_hex(kAvatarEyeColor),
                             static_cast<uint8_t>(far * 255.0f));
            lv_obj_t* eye = side == 0 ? self->avatar_eye_left_ : self->avatar_eye_right_;
            lv_obj_set_style_bg_color(eye, eye_color, 0);

            if (side == 0) {
                eye_left_x = x;
                eye_left_y = y;
                eye_left_w = LV_MAX(w, static_cast<lv_coord_t>(3));
                eye_left_h = LV_MAX(h, static_cast<lv_coord_t>(6));
                eye_left_rot = static_cast<int32_t>(rot_deg * 10.0f);
            } else {
                eye_right_x = x;
                eye_right_y = y;
                eye_right_w = LV_MAX(w, static_cast<lv_coord_t>(3));
                eye_right_h = LV_MAX(h, static_cast<lv_coord_t>(6));
                eye_right_rot = static_cast<int32_t>(rot_deg * 10.0f);
            }
        }
        have_eye_pos = true;
        bob = static_cast<lv_coord_t>(self->gaze_y_ * 0.25f);
        face_tx = static_cast<lv_coord_t>(self->gaze_x_ * 0.45f);
        face_rot = static_cast<int32_t>(self->gaze_tilt_ * 20.0f);  // 2 deg max
    }

    const float fs = self->avatar_face_scale_;
    if (fs != 1.0f) {
        eye_left_x = static_cast<lv_coord_t>(eye_left_x * fs);
        eye_left_y = static_cast<lv_coord_t>(eye_left_y * fs);
        eye_left_w = static_cast<lv_coord_t>(eye_left_w * fs);
        eye_left_h = static_cast<lv_coord_t>(eye_left_h * fs);
        eye_right_x = static_cast<lv_coord_t>(eye_right_x * fs);
        eye_right_y = static_cast<lv_coord_t>(eye_right_y * fs);
        eye_right_w = static_cast<lv_coord_t>(eye_right_w * fs);
        eye_right_h = static_cast<lv_coord_t>(eye_right_h * fs);
        bob = static_cast<lv_coord_t>(bob * fs);
        face_tx = static_cast<lv_coord_t>(face_tx * fs);
    }

    if (!happy) {
        lv_coord_t left_h_draw = eye_left_h;
        lv_coord_t right_h_draw = eye_right_h;
        bool do_blink = !closed && !wink;
        if (do_blink && self->avatar_blink_start_ == 0 && now >= self->avatar_blink_at_) {
            self->avatar_blink_start_ = now;
            // Occasionally only one eye blinks (a wink).
            self->avatar_blink_single_eye_ = (esp_random() % 100) < 12;
            self->avatar_blink_eye_ = static_cast<uint8_t>(esp_random() & 1);
        }
        if (do_blink && self->avatar_blink_start_ != 0) {
            constexpr uint32_t kBlinkDurationMs = 120;
            const uint32_t elapsed = now - self->avatar_blink_start_;
            if (elapsed >= kBlinkDurationMs) {
                self->avatar_blink_start_ = 0;
                self->avatar_blink_at_ = now + 2600 + (esp_random() % 3500);
            } else {
                const float p = static_cast<float>(elapsed) / kBlinkDurationMs;
                const float envelope = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
                const float factor = 1.0f - 0.85f * envelope;
                const bool blink_left =
                    !self->avatar_blink_single_eye_ || self->avatar_blink_eye_ == 0;
                const bool blink_right =
                    !self->avatar_blink_single_eye_ || self->avatar_blink_eye_ == 1;
                if (blink_left) {
                    left_h_draw = LV_MAX(static_cast<lv_coord_t>(3 * fs),
                                         static_cast<lv_coord_t>(eye_left_h * factor));
                }
                if (blink_right) {
                    right_h_draw = LV_MAX(static_cast<lv_coord_t>(3 * fs),
                                          static_cast<lv_coord_t>(eye_right_h * factor));
                }
            }
        }

        lv_obj_set_size(self->avatar_eye_left_, eye_left_w, left_h_draw);
        lv_obj_set_size(self->avatar_eye_right_, eye_right_w, right_h_draw);
        lv_obj_align(self->avatar_eye_left_, LV_ALIGN_CENTER, eye_left_x, eye_left_y);
        lv_obj_align(self->avatar_eye_right_, LV_ALIGN_CENTER, eye_right_x, eye_right_y);
        lv_obj_set_style_transform_rotation(self->avatar_eye_left_, eye_left_rot, 0);
        lv_obj_set_style_transform_rotation(self->avatar_eye_right_, eye_right_rot, 0);
        if (self->avatar_eye_hl_left_ != nullptr) {
            lv_obj_set_size(self->avatar_eye_hl_left_, LV_MAX(2, eye_left_w / 3),
                            LV_MAX(2, left_h_draw / 7));
            lv_obj_align(self->avatar_eye_hl_left_, LV_ALIGN_TOP_MID, 0,
                         LV_MAX(1, left_h_draw / 10));
        }
        if (self->avatar_eye_hl_right_ != nullptr) {
            lv_obj_set_size(self->avatar_eye_hl_right_, LV_MAX(2, eye_right_w / 3),
                            LV_MAX(2, right_h_draw / 7));
            lv_obj_align(self->avatar_eye_hl_right_, LV_ALIGN_TOP_MID, 0,
                         LV_MAX(1, right_h_draw / 10));
        }
    } else {
        const lv_coord_t arc_size = static_cast<lv_coord_t>(20 * fs);
        lv_obj_set_size(self->avatar_eye_happy_left_, arc_size, arc_size);
        lv_obj_set_size(self->avatar_eye_happy_right_, arc_size, arc_size);
        lv_obj_set_style_arc_width(
            self->avatar_eye_happy_left_,
            LV_MAX(static_cast<lv_coord_t>(2), static_cast<lv_coord_t>(4 * fs)), LV_PART_MAIN);
        lv_obj_set_style_arc_width(
            self->avatar_eye_happy_right_,
            LV_MAX(static_cast<lv_coord_t>(2), static_cast<lv_coord_t>(4 * fs)), LV_PART_MAIN);
        const lv_coord_t happy_gap = static_cast<lv_coord_t>(eye_gap * fs);
        lv_obj_align(self->avatar_eye_happy_left_, LV_ALIGN_CENTER, -happy_gap, 0);
        lv_obj_align(self->avatar_eye_happy_right_, LV_ALIGN_CENTER, happy_gap, 0);
    }

    lv_obj_set_style_transform_scale_x(self->emoji_box_, 256 + breath, 0);
    lv_obj_set_style_transform_scale_y(self->emoji_box_, 256 + breath, 0);
    lv_obj_set_style_translate_x(self->emoji_box_, face_tx, 0);
    lv_obj_set_style_translate_y(self->emoji_box_, bob, 0);
    lv_obj_set_style_transform_rotation(self->emoji_box_, face_rot, 0);

    if (self->sleep_zzz_ != nullptr) {
        if (sleeping) {
            const uint32_t phase = (now / 700) % 3;
            lv_label_set_text(self->sleep_zzz_, phase == 0 ? "z" : phase == 1 ? "zz" : "zzz");
            const float zt = static_cast<float>(idle_ms) / 1000.0f;
            lv_obj_set_style_translate_y(
                self->sleep_zzz_, static_cast<lv_coord_t>(-4.0f * sinf(zt * 2 * kPi / 1.8f)), 0);
            lv_obj_set_style_transform_scale(
                self->sleep_zzz_, 300 + static_cast<int32_t>(110.0f * sinf(zt * 2 * kPi / 1.3f)),
                0);
            lv_obj_remove_flag(self->sleep_zzz_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(self->sleep_zzz_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::PickAvatarGaze() {
    static constexpr int8_t kDirX[] = {0, 1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int8_t kDirY[] = {0, 0, 0, 1, -1, 1, -1, 1, -1};
    const uint32_t dir = esp_random() % 9;
    const float mag_x = 2.5f + static_cast<float>(esp_random() % 45) / 10.0f;  // 2.5..6.9
    const float mag_y = 1.5f + static_cast<float>(esp_random() % 35) / 10.0f;  // 1.5..4.9

    gaze_from_x_ = gaze_x_;
    gaze_from_y_ = gaze_y_;
    gaze_from_zoom_ = gaze_zoom_;
    gaze_from_eye_ = gaze_eye_;
    gaze_from_tilt_ = gaze_tilt_;

    gaze_to_x_ = kDirX[dir] * mag_x;
    gaze_to_y_ = kDirY[dir] * mag_y;
    gaze_to_zoom_ = 0.98f + static_cast<float>(esp_random() % 9) / 100.0f;  // 0.98..1.06

    const uint32_t eye_roll = esp_random() % 100;
    if (eye_roll < 18) {
        gaze_to_eye_ = 0.72f;  // squint
    } else if (eye_roll < 30) {
        gaze_to_eye_ = 1.10f;  // wide eyes
    } else {
        gaze_to_eye_ = 0.92f + static_cast<float>(esp_random() % 14) / 100.0f;
    }

    gaze_to_tilt_ = kDirX[dir] * (3.0f + static_cast<float>(esp_random() % 9));
    gaze_to_tilt_ += static_cast<float>(esp_random() % 5) - 2.0f;
    gaze_start_ = lv_tick_get();
}

void LcdDisplay::SetAvatarEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    const std::string name = emotion;
    if (name == "neutral" || name.rfind("robot", 0) == 0) {
        avatar_override_ = kAvatarExpressionNone;
        return;
    }
    AvatarExpression expression = kAvatarExpressionNone;
    if (name == "happy" || name == "laughing" || name == "funny" || name == "delicious" ||
        name == "cool" || name == "confident" || name == "relaxed" || name == "kissy" ||
        name == "loving" || name == "winking") {
        expression = kAvatarExpressionHappy;
    } else if (name == "thinking" || name == "confused") {
        expression = kAvatarExpressionThinking;
    } else if (name == "sleepy") {
        expression = kAvatarExpressionClosed;
    } else if (name == "winking" || name == "wink") {
        expression = kAvatarExpressionWink;
    }
    if (expression == kAvatarExpressionNone) {
        return;  // Not part of the core set yet: keep the current mood face.
    }
    avatar_override_ = expression;
    avatar_override_until_ = lv_tick_get() + 2500;
}

void LcdDisplay::UpdateChatViewport() {
    const int lines = avatar_mood_ == kAvatarMoodIdle ? 5 : 6;
    if (lines == chat_lines_per_page_) {
        UpdateMediaBarVisibility();
        return;
    }
    chat_lines_per_page_ = lines;
    follow_latest_ = true;
    ApplyChatLayout();
}

void LcdDisplay::UpdateMediaBarVisibility() {
    if (media_bar_ != nullptr) {
        const bool show = chat_lines_per_page_ == 5 && !media_screen_visible_ &&
                          !bluetooth_screen_visible_ && !stroke_screen_visible_ &&
                          !file_service_visible_ && avatar_mood_ == kAvatarMoodIdle;
        if (show) {
            lv_obj_remove_flag(media_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(media_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // 抽屉只在首页空闲态可用；离开空闲或任意二级页时自动收起。
    const bool drawer_available =
        chat_lines_per_page_ == 5 && !media_screen_visible_ && !bluetooth_screen_visible_ &&
        !stroke_screen_visible_ && !english_screen_visible_ && !settings_screen_visible_ &&
        !english_setup_visible_ && !file_service_visible_ && avatar_mood_ == kAvatarMoodIdle;
    if (drawer_screen_visible_ && !drawer_available) {
        ShowDrawerScreen(false);
    }
}

#if CONFIG_FEATURE_BLE_KEYBOARD
void LcdDisplay::ShowMediaScreen(bool show) {
    if (media_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    media_screen_visible_ = show;
    if (show) {
        ShowBluetoothScreen(false);
        // 媒体页与英语页互斥
#if CONFIG_FEATURE_ENGLISH_DICTATION
        if (english_screen_visible_) {
            EnglishPlayer::GetInstance().Stop();
            ShowEnglishScreen(false);
        }
#endif
        ShowEnglishSetup(false);
        SetMediaPanel(0);
        lv_obj_remove_flag(media_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(media_screen_);
    } else {
        lv_obj_add_flag(media_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::ShowBluetoothScreen(bool show) {
    if (bluetooth_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    bluetooth_screen_visible_ = show;
    if (show) {
        RefreshBluetoothList();
#if CONFIG_FEATURE_ENGLISH_DICTATION
        if (english_screen_visible_) {
            EnglishPlayer::GetInstance().Stop();
            ShowEnglishScreen(false);
        }
#endif
        ShowEnglishSetup(false);
        lv_obj_remove_flag(bluetooth_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(bluetooth_screen_);
    } else {
        lv_obj_add_flag(bluetooth_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}
#else
void LcdDisplay::ShowMediaScreen(bool show) { (void)show; }
void LcdDisplay::ShowBluetoothScreen(bool show) { (void)show; }
#endif  // CONFIG_FEATURE_BLE_KEYBOARD

#if CONFIG_FEATURE_STROKE_ORDER

/* Stroke order learner */

static const char* const kStrokeGradeCn[] = {"",       "",       "一年级", "二年级",
                                             "三年级", "四年级", "五年级", "六年级"};

static const struct {
    uint8_t b0, b1;
    char base;
} kStrokeToneMap[] = {
    {0xC4, 0x81, 'a'}, {0xC3, 0xA1, 'a'}, {0xC7, 0x8E, 'a'}, {0xC3, 0xA0, 'a'}, {0xC4, 0x93, 'e'},
    {0xC3, 0xA9, 'e'}, {0xC4, 0x9B, 'e'}, {0xC3, 0xA8, 'e'}, {0xC4, 0xAB, 'i'}, {0xC3, 0xAD, 'i'},
    {0xC7, 0x90, 'i'}, {0xC3, 0xAC, 'i'}, {0xC5, 0x8D, 'o'}, {0xC3, 0xB3, 'o'}, {0xC7, 0x92, 'o'},
    {0xC3, 0xB2, 'o'}, {0xC5, 0xAB, 'u'}, {0xC3, 0xBA, 'u'}, {0xC7, 0x94, 'u'}, {0xC3, 0xB9, 'u'},
    {0xC7, 0x96, 'u'}, {0xC7, 0x98, 'u'}, {0xC7, 0x9A, 'u'}, {0xC7, 0x9C, 'u'}, {0xC3, 0xBC, 'u'},
};

static std::string StrokeAsciiPinyin(const std::string& py) {
    std::string out;
    out.reserve(py.size());
    for (size_t i = 0; i < py.size(); ++i) {
        uint8_t c = static_cast<uint8_t>(py[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            continue;
        }
        bool hit = false;
        if (i + 1 < py.size()) {
            uint8_t c1 = static_cast<uint8_t>(py[i + 1]);
            for (const auto& t : kStrokeToneMap) {
                if (t.b0 == c && t.b1 == c1) {
                    out += t.base;
                    i++;
                    hit = true;
                    break;
                }
            }
        }
        if (!hit && i + 1 < py.size()) {
            i++;
        }
    }
    return out;
}

float LcdDisplay::StrokeMapFpx(uint8_t v) {
    return kStrokeCanvasMargin + v * kStrokeGlyphSpan / static_cast<float>(StrokeStore::kGridMax);
}

void LcdDisplay::StrokeStampDot(int cx, int cy, float r, lv_color_t color) {
    if (stroke_canvas_ == nullptr) {
        return;
    }
    const int ri = static_cast<int>(ceilf(r));
    const float r2 = r * r;
    for (int dy = -ri; dy <= ri; ++dy) {
        int py = cy + dy;
        if (py < 0 || py >= kStrokeCanvasSide) {
            continue;
        }
        for (int dx = -ri; dx <= ri; ++dx) {
            int px = cx + dx;
            if (px < 0 || px >= kStrokeCanvasSide) {
                continue;
            }
            if (dx * dx + dy * dy <= r2) {
                lv_canvas_set_px(stroke_canvas_, px, py, color, LV_OPA_COVER);
            }
        }
    }
}

void LcdDisplay::StrokeStampLine(float x0, float y0, float x1, float y1, float r,
                                 lv_color_t color) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.5f) {
        StrokeStampDot(static_cast<int>(lroundf(x0)), static_cast<int>(lroundf(y0)), r, color);
        return;
    }
    int steps = static_cast<int>(dist / (r * 0.45f));
    if (steps < 1) {
        steps = 1;
    } else if (steps > 96) {
        steps = 96;
    }
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / steps;
        StrokeStampDot(static_cast<int>(lroundf(x0 + dx * t)),
                       static_cast<int>(lroundf(y0 + dy * t)), r, color);
    }
}

void LcdDisplay::StrokeFillCanvas(lv_color_t color) {
    if (stroke_canvas_ == nullptr || stroke_canvas_buf_ == nullptr) {
        return;
    }
    auto* raw = reinterpret_cast<uint16_t*>(stroke_canvas_buf_);
    const uint16_t v = lv_color_to_u16(color);
    const int count = kStrokeCanvasSide * kStrokeCanvasSide;
    for (int i = 0; i < count; ++i) {
        raw[i] = v;
    }
    const uint16_t grid = lv_color_to_u16(lv_color_hex(kPaperGrid));
    const int mid = kStrokeCanvasSide / 2;
    for (int i = kStrokeCanvasMargin / 2; i < kStrokeCanvasSide - kStrokeCanvasMargin / 2; ++i) {
        if ((i / 6) % 2 == 0) {
            raw[mid * kStrokeCanvasSide + i] = grid;
            raw[i * kStrokeCanvasSide + mid] = grid;
        }
    }
    lv_obj_invalidate(stroke_canvas_);
}

void LcdDisplay::StrokeBlitGlyph(lv_color_t color) {
    if (stroke_canvas_ == nullptr || stroke_char_idx_ < 0) {
        return;
    }
    memset(stroke_bits_, 0, sizeof(stroke_bits_));
    if (!StrokeStore::GetInstance().DecodeGlyph(stroke_char_idx_, stroke_bits_)) {
        return;
    }
    const int span = StrokeStore::kGlyphSide;
    auto* raw = reinterpret_cast<uint16_t*>(stroke_canvas_buf_);
    const uint16_t v = lv_color_to_u16(color);
    for (int p = 0; p < span * span; ++p) {
        if ((stroke_bits_[p >> 3] >> (7 - (p & 7))) & 1) {
            const int x = kStrokeCanvasMargin + p % span;
            const int y = kStrokeCanvasMargin + p / span;
            raw[y * kStrokeCanvasSide + x] = v;
        }
    }
    lv_obj_invalidate(stroke_canvas_);
}

void LcdDisplay::StrokeStartStroke(int s) {
    if (stroke_canvas_ == nullptr) {
        return;
    }
    if (s < 0 || s >= static_cast<int>(stroke_metas_.size())) {
        if (stroke_metas_.empty()) {
            stroke_done_tick_ = lv_tick_get() + 2000;
        }
        return;
    }
    stroke_stroke_idx_ = s;
    const StrokeMeta& m = stroke_metas_[s];
    stroke_pos_x_ = StrokeMapFpx(m.pts[0].x);
    stroke_pos_y_ = StrokeMapFpx(m.pts[0].y);
    stroke_pt_idx_ = 1;
    // Pen width: the raw medians are quite bold; draw at half thickness.
    stroke_radius_ = m.width * kStrokeGlyphSpan / (2.0f * StrokeStore::kGridMax) * 0.5f;
    if (stroke_radius_ < 1.5f) {
        stroke_radius_ = 1.5f;
    }
    float len = 0.0f;
    for (int i = 1; i < m.npts; ++i) {
        float dx = StrokeMapFpx(m.pts[i].x) - StrokeMapFpx(m.pts[i - 1].x);
        float dy = StrokeMapFpx(m.pts[i].y) - StrokeMapFpx(m.pts[i - 1].y);
        len += sqrtf(dx * dx + dy * dy);
    }
    float seconds = 0.09f + len * 0.00125f;
    if (seconds < 0.09f) {
        seconds = 0.09f;
    } else if (seconds > 0.3f) {
        seconds = 0.3f;
    }
    stroke_speed_ = len > 0.5f ? len / (25.0f * seconds) : 2.0f;

    StrokeStampDot(static_cast<int>(lroundf(stroke_pos_x_)),
                   static_cast<int>(lroundf(stroke_pos_y_)), stroke_radius_, stroke_color_done_);
    if (s < kStrokeMaxStrokes && stroke_badges_[s] != nullptr) {
        lv_obj_t* b = stroke_badges_[s];
        lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, static_cast<int>(lroundf(stroke_pos_x_)) - 10,
                     static_cast<int>(lroundf(stroke_pos_y_)) - 10);
    }
    lv_obj_invalidate(stroke_canvas_);
}

void LcdDisplay::StrokeDrawStroke(int s, lv_color_t color) {
    if (s < 0 || s >= static_cast<int>(stroke_metas_.size())) {
        return;
    }
    const StrokeMeta& m = stroke_metas_[s];
    for (int i = 1; i < m.npts; ++i) {
        StrokeStampLine(StrokeMapFpx(m.pts[i - 1].x), StrokeMapFpx(m.pts[i - 1].y),
                        StrokeMapFpx(m.pts[i].x), StrokeMapFpx(m.pts[i].y), stroke_radius_, color);
    }
    if (m.npts <= 1) {
        StrokeStampDot(static_cast<int>(lroundf(StrokeMapFpx(m.pts[0].x))),
                       static_cast<int>(lroundf(StrokeMapFpx(m.pts[0].y))), stroke_radius_, color);
    }
}

void LcdDisplay::StrokeFinishStroke(int s) { StrokeDrawStroke(s, stroke_color_done_); }

void LcdDisplay::StrokeLoadChar(int idx) {
    auto& store = StrokeStore::GetInstance();
    if (stroke_screen_ == nullptr || stroke_canvas_ == nullptr || !store.Ready() || idx < 0 ||
        idx >= store.CharCount()) {
        return;
    }
    stroke_char_idx_ = idx;
    stroke_metas_.clear();
    store.LoadMedians(idx, stroke_metas_);
    for (int i = 0; i < kStrokeMaxStrokes; ++i) {
        if (stroke_badges_[i] != nullptr) {
            lv_obj_add_flag(stroke_badges_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    stroke_color_bg_ = lv_color_hex(kPaperSheet);
    stroke_color_dim_ = lv_color_hex(kPaperGuide);
    stroke_color_done_ = lv_color_hex(kPaperInk);

    lv_label_set_text(stroke_title_label_, store.PinyinOf(idx).c_str());

    StrokeFillCanvas(stroke_color_bg_);
    StrokeBlitGlyph(stroke_color_dim_);
    for (int i = 0; i < kStrokeMaxStrokes; ++i) {
        if (stroke_lines_[i] == nullptr) {
            continue;
        }
        if (i < static_cast<int>(stroke_metas_.size())) {
            const StrokeMeta& m = stroke_metas_[i];
            auto& pts = stroke_line_pts_[i];
            pts.clear();
            pts.reserve(m.npts);
            for (int k = 0; k < m.npts; ++k) {
                pts.push_back(lv_point_precise_t{
                    static_cast<lv_value_precise_t>(lroundf(StrokeMapFpx(m.pts[k].x))),
                    static_cast<lv_value_precise_t>(lroundf(StrokeMapFpx(m.pts[k].y)))});
            }
            float radius = m.width * kStrokeGlyphSpan / (2.0f * StrokeStore::kGridMax) * 0.5f;
            if (radius < 1.5f) {
                radius = 1.5f;
            }
            lv_line_set_points(stroke_lines_[i], pts.data(), static_cast<uint32_t>(pts.size()));
            lv_obj_set_style_line_width(stroke_lines_[i],
                                        LV_MAX(2, static_cast<int>(lroundf(radius * 2))), 0);
            lv_obj_set_style_line_color(stroke_lines_[i], stroke_color_done_, 0);
            lv_obj_remove_flag(stroke_lines_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(stroke_lines_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(stroke_lines_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    stroke_done_tick_ = 0;
    stroke_revealed_ = 0;
    stroke_reveal_tick_ = lv_tick_get() + 400;
}

void LcdDisplay::StrokeAnimTick() {
    auto& store = StrokeStore::GetInstance();
    if (stroke_canvas_ == nullptr || stroke_char_idx_ < 0 || !store.Ready()) {
        return;
    }
    const uint32_t now = lv_tick_get();
    // Do not leave a demo looping on screen forever: after five minutes without
    // any touch/voice activity return to the normal screen.
    if (stroke_screen_visible_ && now - last_activity_tick_ > 300000) {
        ShowStrokeScreen(false);
        return;
    }
    if (stroke_done_tick_ != 0) {
        if (static_cast<int32_t>(now - stroke_done_tick_) >= 0) {
            // Replay the same character without redrawing the background: just
            // hide the stroke lines again.
            StrokeHideAllLines();
            stroke_revealed_ = 0;
            stroke_reveal_tick_ = now + 400;
            stroke_done_tick_ = 0;
        }
        return;
    }
    int total = static_cast<int>(stroke_metas_.size());
    if (total > kStrokeMaxStrokes) {
        total = kStrokeMaxStrokes;
    }
    if (stroke_revealed_ >= total) {
        stroke_done_tick_ = now + 2500;
        return;
    }
    if (static_cast<int32_t>(now - stroke_reveal_tick_) < 0) {
        return;
    }
    // Showing a pre-built line object only repaints that stroke's area; the
    // canvas with the dim glyph is not touched, so TTS audio stays smooth.
    if (stroke_revealed_ >= 0 && stroke_revealed_ < kStrokeMaxStrokes &&
        stroke_lines_[stroke_revealed_] != nullptr) {
        lv_obj_remove_flag(stroke_lines_[stroke_revealed_], LV_OBJ_FLAG_HIDDEN);
    }
    stroke_revealed_++;
    stroke_reveal_tick_ = now + 260;
}

void LcdDisplay::StrokeHideAllLines() {
    for (int i = 0; i < kStrokeMaxStrokes; ++i) {
        if (stroke_lines_[i] != nullptr) {
            lv_obj_add_flag(stroke_lines_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::StrokeAnimTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->StrokeAnimTick();
    }
}

void LcdDisplay::ShowStrokeScreen(int char_idx) {
    if (stroke_screen_ == nullptr) {
        return;
    }
    auto& store = StrokeStore::GetInstance();
    if (!store.Ready() || char_idx < 0 || char_idx >= store.CharCount()) {
        return;
    }
    DisplayLockGuard lock(this);
    stroke_screen_visible_ = true;
    media_screen_visible_ = false;
    bluetooth_screen_visible_ = false;
    if (media_screen_ != nullptr) {
        lv_obj_add_flag(media_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bluetooth_screen_ != nullptr) {
        lv_obj_add_flag(bluetooth_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(stroke_screen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(stroke_screen_);
    StrokeLoadChar(char_idx);
    if (stroke_anim_timer_ != nullptr) {
        lv_timer_resume(stroke_anim_timer_);
    }
    UpdateMediaBarVisibility();
}

#endif

#if CONFIG_FEATURE_ENGLISH_DICTATION
// ---- 英语听写页 ----

void LcdDisplay::ShowEnglishScreen(bool show) {
    if (english_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    english_screen_visible_ = show;
    if (show) {
        lv_obj_clear_flag(english_screen_, LV_OBJ_FLAG_HIDDEN);
        UpdateEnglishScreen();
    } else {
        lv_obj_add_flag(english_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::RefreshEnglishScreen() { UpdateEnglishScreen(); }

void LcdDisplay::RefreshEnglishScreenStatic() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->RefreshEnglishScreen();
    }
}

void LcdDisplay::UpdateEnglishScreen() {
    if (english_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    auto& player = EnglishPlayer::GetInstance();
    auto& store = EnglishStore::GetInstance();
    if (!player.Active()) {
        lv_label_set_text(english_word_label_, "");
        lv_label_set_text(english_phonetic_label_, "");
        lv_label_set_text(english_meaning_label_, "");
        lv_label_set_text(english_progress_label_, "听写");
        if (english_title_label_ != nullptr) {
            lv_label_set_text(english_title_label_, "");
        }
        if (english_score_label_ != nullptr) {
            lv_label_set_text(english_score_label_, "");
        }
        return;
    }
    const int idx = player.CurrentIndex();
    const int total = player.Total();
    const int pos = player.Position();

    // 进度：第几个/共几个 + 剩余倒计时（单元名由下一行标题展示，避免换行）
    char buf[96];
    const int unit = store.UnitAt(idx);
    const int cd = player.Countdown();
    if (player.Manual()) {
        snprintf(buf, sizeof(buf), "%d/%d · 手动", pos < 0 ? 0 : pos, total);
    } else if (cd >= 0) {
        snprintf(buf, sizeof(buf), "%d/%d · %ds", pos < 0 ? 0 : pos, total, cd);
    } else {
        snprintf(buf, sizeof(buf), "%d/%d · %ds", pos < 0 ? 0 : pos, total, player.Interval());
    }
    lv_label_set_text(english_progress_label_, buf);

    // 标题：按分类练习时显示分类名，否则显示课本单元标题
    if (english_title_label_ != nullptr) {
        const int topic = player.CurrentTopic();
        if (topic > 0) {
            lv_label_set_text(english_title_label_, EnglishStore::TopicName(topic));
        } else {
            const auto title = store.UnitTitle(unit);
            if (title.empty()) {
                const std::string unit_name = store.UnitName(unit);
                lv_label_set_text(english_title_label_, unit_name.c_str());
            } else {
                lv_label_set_text(english_title_label_, title.data());
            }
        }
    }

    // 听写模式：大字显示中文释义，英文拼写由"看词听写"开关控制；
    // 单词卡模式：大字显示英文，下面是音标和中文。
    if (player.ShowWord() || player.CurrentMode() == EnglishPlayer::kCards) {
        lv_label_set_text(english_word_label_, store.WordAt(idx).data());
        lv_label_set_text(english_phonetic_label_, store.PhoneticAt(idx).data());
        lv_label_set_text(english_meaning_label_, store.MeaningAt(idx).data());
    } else {
        // 真听写：只给中文提示，英文让孩子自己写
        lv_label_set_text(english_word_label_, store.MeaningAt(idx).data());
        lv_label_set_text(english_phonetic_label_, "");
        lv_label_set_text(english_meaning_label_, "");
    }

    // 听写成绩（有作答记录时显示）
    if (english_score_label_ != nullptr) {
        const int answered = player.AnsweredCount();
        if (answered > 0) {
            snprintf(buf, sizeof(buf), "对 %d / %d", player.CorrectCount(), answered);
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(english_score_label_, buf);
    }

    // 底部两个小按钮的文案
    if (english_mode_label_ != nullptr) {
        const char* mode_text = player.CurrentMode() == EnglishPlayer::kCards
                                    ? "单词卡"
                                    : (player.ShowWord() ? "看词听写" : "真听写");
        lv_label_set_text(english_mode_label_, mode_text);
    }
    if (english_interval_label_ != nullptr) {
        if (player.Manual()) {
            lv_label_set_text(english_interval_label_, "间隔 手动");
        } else {
            snprintf(buf, sizeof(buf), "间隔 %ds", player.Interval());
            lv_label_set_text(english_interval_label_, buf);
        }
    }
    if (english_speak_label_ != nullptr) {
        lv_label_set_text(english_speak_label_, EnglishPlayer::SpeakName(player.CurrentSpeak()));
    }
}

void LcdDisplay::RebindEnglishFonts(const lv_font_t* text_font, const lv_font_t* icon_font) {
    lv_obj_t* text_labels[] = {english_word_label_,        english_meaning_label_,
                               english_progress_label_,    english_score_label_,
                               english_title_label_,       english_setup_title_label_,
                               english_setup_next_label_,  english_setup_tab_text_,
                               english_setup_tab_topic_};
    for (lv_obj_t* label : text_labels) {
        if (label != nullptr && text_font != nullptr) {
            lv_obj_set_style_text_font(label, text_font, 0);
        }
    }
    if (english_phonetic_label_ != nullptr) {
        lv_obj_set_style_text_font(english_phonetic_label_, &font_english_20, 0);
    }
    // 启动面板的次级标签 / 底部按钮：统一 16px
    for (lv_obj_t* label : english_setup_labels_) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, &ui_font_16_4, 0);
        }
    }
    for (int i = 0; i < english_setup_row_count_; ++i) {
        if (english_setup_row_names_[i] != nullptr) {
            lv_obj_set_style_text_font(english_setup_row_names_[i], &ui_font_16_4, 0);
        }
        if (english_setup_row_counts_[i] != nullptr) {
            lv_obj_set_style_text_font(english_setup_row_counts_[i], &ui_font_16_4, 0);
        }
    }
    for (int group = 0; group < 4; ++group) {
        for (int i = 0; i < english_setup_chip_count_[group]; ++i) {
            if (english_setup_chip_labels_[group][i] != nullptr) {
                lv_obj_set_style_text_font(english_setup_chip_labels_[group][i], &ui_font_16_4,
                                           0);
            }
        }
    }
    if (english_setup_close_icon_ != nullptr && icon_font != nullptr) {
        lv_obj_set_style_text_font(english_setup_close_icon_, icon_font, 0);
    }
    lv_obj_t* icon_labels[] = {english_back_icon_, english_reset_icon_,
                               english_setup_page_left_icon_, english_setup_page_right_icon_};
    for (lv_obj_t* label : icon_labels) {
        if (label != nullptr && icon_font != nullptr) {
            lv_obj_set_style_text_font(label, icon_font, 0);
        }
    }
    lv_obj_t* small_labels[] = {english_mode_label_, english_interval_label_,
                                english_speak_label_};
    for (lv_obj_t* label : small_labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, &ui_font_16_4, 0);
        }
    }
}

void LcdDisplay::ShowEnglishSetup(bool show) {
    if (english_setup_panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    english_setup_visible_ = show;
    if (show) {
        EnglishSetupRebuildScopeList();
        UpdateEnglishSetup();
        lv_obj_clear_flag(english_setup_panel_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(english_setup_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::EnglishSetupShowStep(int step) {
    english_setup_step_ = step;
    if (english_setup_step1_ != nullptr) {
        if (step == 0) {
            lv_obj_clear_flag(english_setup_step1_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(english_setup_step1_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (english_setup_step2_ != nullptr) {
        if (step == 1) {
            lv_obj_clear_flag(english_setup_step2_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(english_setup_step2_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (english_setup_title_label_ != nullptr) {
        if (step == 0 && english_setup_page_count_ > 1) {
            char buf[48];
            snprintf(buf, sizeof(buf), "选择内容 %d/%d", english_setup_page_ + 1,
                     english_setup_page_count_);
            lv_label_set_text(english_setup_title_label_, buf);
        } else {
            lv_label_set_text(english_setup_title_label_, step == 0 ? "选择内容" : "播报设置");
        }
    }
    if (english_setup_next_label_ != nullptr) {
        lv_label_set_text(english_setup_next_label_, step == 0 ? "下一步" : "开始听写");
    }
}

void LcdDisplay::EnglishSetupRebuildScopeList() {
    if (english_setup_list_ == nullptr) {
        return;
    }
    lv_obj_clean(english_setup_list_);
    english_setup_row_count_ = 0;
    auto& store = EnglishStore::GetInstance();

    struct ScopeItem {
        int value;
        std::string name;
        int words;
    };
    std::vector<ScopeItem> items;
    const bool by_unit = english_setup_source_ == 0;
    int total = 0;
    if (by_unit) {
        for (int unit = 1; unit < 35; ++unit) {
            total += store.UnitCount(unit);
        }
    } else {
        for (int topic = 1; topic < 28; ++topic) {
            total += store.TopicCount(topic);
        }
    }
    items.push_back({0, "全部", total});
    if (by_unit) {
        for (int unit = 1; unit < 35; ++unit) {
            const int words = store.UnitCount(unit);
            if (words > 0) {
                items.push_back({unit, store.UnitName(unit), words});
            }
        }
    } else {
        for (int topic = 1; topic < 28; ++topic) {
            const int words = store.TopicCount(topic);
            if (words > 0) {
                items.push_back({topic, EnglishStore::TopicName(topic), words});
            }
        }
    }

    const int item_count = static_cast<int>(items.size());
    english_setup_page_count_ = std::max(1, (item_count + kEnglishSetupRowsPerPage - 1) /
                                                kEnglishSetupRowsPerPage);
    english_setup_page_ =
        std::max(0, std::min(english_setup_page_, english_setup_page_count_ - 1));
    const int begin = english_setup_page_ * kEnglishSetupRowsPerPage;
    const int end = std::min(begin + kEnglishSetupRowsPerPage, item_count);
    for (int i = begin; i < end; ++i) {
        const ScopeItem& item = items[i];
        const int slot = english_setup_row_count_;
        lv_obj_t* row = lv_button_create(english_setup_list_);
        lv_obj_set_size(row, LV_PCT(100), 28);
        lv_obj_set_style_radius(row, 9, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_ext_click_area(row, 2);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(item.value)));
        lv_obj_add_event_cb(row, &LcdDisplay::OnEnglishSetupScopeRowClicked, LV_EVENT_PRESSED,
                            this);
        lv_obj_t* left = lv_label_create(row);
        lv_obj_set_style_text_font(left, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(left, lv_color_hex(kSkyMuted), 0);
        lv_obj_set_width(left, 112);
        lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
        lv_label_set_text(left, item.name.c_str());
        lv_obj_align(left, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_t* right = lv_label_create(row);
        lv_obj_set_style_text_font(right, &ui_font_16_4, 0);
        lv_obj_set_style_text_color(right, lv_color_hex(kSkyMuted), 0);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d", item.words);
        lv_label_set_text(right, buf);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, -8, 0);
        english_setup_rows_[slot] = row;
        english_setup_row_names_[slot] = left;
        english_setup_row_counts_[slot] = right;
        english_setup_row_values_[slot] = item.value;
        english_setup_row_count_++;
    }
    lv_obj_update_layout(english_setup_list_);
}

void LcdDisplay::UpdateEnglishSetup() {
    EnglishSetupShowStep(english_setup_step_);

    const bool text_source = english_setup_source_ == 0;
    lv_obj_t* tabs[2] = {nullptr, nullptr};
    if (english_setup_tab_text_ != nullptr) {
        tabs[0] = lv_obj_get_parent(english_setup_tab_text_);
    }
    if (english_setup_tab_topic_ != nullptr) {
        tabs[1] = lv_obj_get_parent(english_setup_tab_topic_);
    }
    for (int i = 0; i < 2; ++i) {
        if (tabs[i] == nullptr) {
            continue;
        }
        const bool active = (i == 0) == text_source;
        lv_obj_set_style_bg_color(tabs[i],
                                  lv_color_hex(active ? kSkyAccent : kSkyCard), 0);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tabs[i],
                                      lv_color_hex(active ? kSkyAccent : kSkyBorder), 0);
        lv_obj_t* label = lv_obj_get_child(tabs[i], 0);
        if (label != nullptr) {
            lv_obj_set_style_text_color(label, lv_color_hex(active ? 0xFFFFFF : kSkyMuted), 0);
        }
    }

    const int selected = text_source ? english_setup_unit_ : english_setup_topic_;
    for (int i = 0; i < english_setup_row_count_; ++i) {
        lv_obj_t* row = english_setup_rows_[i];
        if (row == nullptr) {
            continue;
        }
        const bool active = english_setup_row_values_[i] == selected;
        lv_obj_set_style_bg_color(row, lv_color_hex(active ? kSkySelect : kSkyCard), 0);
        lv_obj_set_style_bg_opa(row, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, active ? 1 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(kSkySelectEdge), 0);
        const uint32_t color = active ? 0x11496F : kSkyMuted;
        if (english_setup_row_names_[i] != nullptr) {
            lv_obj_set_style_text_color(english_setup_row_names_[i], lv_color_hex(color), 0);
        }
        if (english_setup_row_counts_[i] != nullptr) {
            lv_obj_set_style_text_color(english_setup_row_counts_[i], lv_color_hex(color), 0);
        }
    }

    for (int group = 0; group < 4; ++group) {
        for (int i = 0; i < english_setup_chip_count_[group]; ++i) {
            lv_obj_t* label = english_setup_chip_labels_[group][i];
            if (label == nullptr) {
                continue;
            }
            lv_obj_t* chip = lv_obj_get_parent(label);
            int current = 0;
            if (group == 0) {
                current = english_setup_count_;
            } else if (group == 1) {
                current = english_setup_interval_;
            } else if (group == 2) {
                current = english_setup_speak_;
            } else {
                current = english_setup_mode_;
            }
            const bool active = english_setup_chip_values_[group][i] == current;
            lv_obj_set_style_bg_color(chip, lv_color_hex(active ? kSkySelect : kSkyCard), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(
                chip, lv_color_hex(active ? kSkySelectEdge : kSkyBorder), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(active ? 0x11496F : kSkyMuted), 0);
        }
    }

    const bool has_prev = english_setup_page_ > 0;
    const bool has_next = english_setup_page_ + 1 < english_setup_page_count_;
    if (english_setup_page_left_ != nullptr) {
        lv_obj_set_style_opa(english_setup_page_left_, has_prev ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (english_setup_page_right_ != nullptr) {
        lv_obj_set_style_opa(english_setup_page_right_, has_next ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

void LcdDisplay::OnEnglishSetupBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    if (self->english_setup_step_ == 1) {
        self->EnglishSetupShowStep(0);
        self->UpdateEnglishSetup();
        return;
    }
    self->ShowEnglishSetup(false);
    self->CloseSubPageToDrawer();
}

void LcdDisplay::OnEnglishSetupSourceClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    const int source = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_obj_get_user_data(lv_event_get_target_obj(e))));
    if (source == self->english_setup_source_) {
        return;
    }
    self->english_setup_source_ = source;
    self->english_setup_page_ = 0;
    self->EnglishSetupRebuildScopeList();
    self->UpdateEnglishSetup();
    lv_obj_t* list = self->english_setup_list_;
    if (list != nullptr) {
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
}

void LcdDisplay::OnEnglishSetupPageClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_obj_get_user_data(lv_event_get_target_obj(e))));
    const int page = self->english_setup_page_ + delta;
    if (page < 0 || page >= self->english_setup_page_count_) {
        return;
    }
    self->english_setup_page_ = page;
    self->EnglishSetupRebuildScopeList();
    self->UpdateEnglishSetup();
}

void LcdDisplay::OnEnglishSetupScopeRowClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_obj_t* row = lv_event_get_target_obj(e);
    const int value = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
    if (self->english_setup_source_ == 0) {
        self->english_setup_unit_ = value;
    } else {
        self->english_setup_topic_ = value;
    }
    self->UpdateEnglishSetup();
}

void LcdDisplay::OnEnglishSetupChipClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    const int packed = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_obj_get_user_data(lv_event_get_target_obj(e))));
    const int group = packed >> 8;
    const int value = packed & 0xFF;
    switch (group) {
        case 0:
            self->english_setup_count_ = value;
            break;
        case 1:
            self->english_setup_interval_ = value;
            break;
        case 2:
            self->english_setup_speak_ = value;
            break;
        case 3:
            self->english_setup_mode_ = value;
            break;
        default:
            return;
    }
    self->UpdateEnglishSetup();
}

void LcdDisplay::OnEnglishSetupNextClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    if (self->english_setup_step_ == 0) {
        self->EnglishSetupShowStep(1);
        self->UpdateEnglishSetup();
        return;
    }
    if (!EnglishPlayer::Available()) {
        ESP_LOGW(TAG, "英语词库未就绪");
        return;
    }
    const int unit = self->english_setup_source_ == 0 ? self->english_setup_unit_ : 0;
    const int topic = self->english_setup_source_ == 1 ? self->english_setup_topic_ : 0;
    const int count = self->english_setup_count_;
    const int interval = self->english_setup_interval_;
    const auto mode = static_cast<EnglishPlayer::Mode>(self->english_setup_mode_);
    const auto speak = static_cast<EnglishPlayer::Speak>(self->english_setup_speak_);
    self->ShowEnglishSetup(false);
    self->ShowEnglishScreen(true);
    // 播放会在音频队列里阻塞，放到主任务执行，避免卡住 LVGL
    Application::GetInstance().Schedule([self, unit, topic, count, interval, mode, speak]() {
        auto& player = EnglishPlayer::GetInstance();
        // 只设置语言，不在这里重播当前词；否则"开始"会先把旧词再读一遍。
        player.SetSpeak(speak);
        player.Start(unit, topic, count, interval, mode == EnglishPlayer::kCards, mode);
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    EnglishPlayer::GetInstance().Stop();
    self->ShowEnglishScreen(false);
    self->CloseSubPageToDrawer();
}

void LcdDisplay::OnEnglishResetClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().Restart();
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishPrevClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().Prev();
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishNextClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    // LVGL 长按结束后还会补发一次 CLICKED，这里屏蔽掉，避免"答对"后又跳过一个词
    if (lv_tick_get() - self->english_long_guard_ < 600) {
        return;
    }
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().Next();
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishNextLongPressed(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    // 长按"下一个"表示这个单词听写正确
    self->english_long_guard_ = lv_tick_get();
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().MarkCorrect();
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishModeClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().ToggleMode();
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishSpeakClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().CycleSpeak();
        self->RefreshEnglishScreen();
    });
}

void LcdDisplay::OnEnglishIntervalClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([self]() {
        EnglishPlayer::GetInstance().CycleInterval();
        self->RefreshEnglishScreen();
    });
}

#else

void LcdDisplay::ShowEnglishScreen(bool show) { (void)show; }
void LcdDisplay::UpdateEnglishScreen() {}
void LcdDisplay::RefreshEnglishScreen() {}
void LcdDisplay::RefreshEnglishScreenStatic() {}
void LcdDisplay::ShowEnglishSetup(bool show) { (void)show; }
void LcdDisplay::RebindEnglishFonts(const lv_font_t* text_font, const lv_font_t* icon_font) {
    (void)text_font;
    (void)icon_font;
}

#endif

#if CONFIG_FEATURE_STROKE_ORDER

void LcdDisplay::ShowStrokeScreen(bool show) {
    if (stroke_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    ESP_LOGI(TAG, "ShowStrokeScreen(%d)", show);
    stroke_screen_visible_ = show;
    if (show) {
        auto& store = StrokeStore::GetInstance();
        if (store.Ready()) {
            int idx = stroke_char_idx_ >= 0 ? stroke_char_idx_ : store.GradeStart(1);
            if (idx >= 0) {
                StrokeLoadChar(idx);
            }
        }
        lv_obj_remove_flag(stroke_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(stroke_screen_);
        if (stroke_anim_timer_ != nullptr) {
            lv_timer_resume(stroke_anim_timer_);
        }
    } else {
        lv_obj_add_flag(stroke_screen_, LV_OBJ_FLAG_HIDDEN);
        if (stroke_anim_timer_ != nullptr) {
            lv_timer_pause(stroke_anim_timer_);
        }
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::StrokeChangeChar(int dir) {
    if (stroke_screen_ == nullptr) {
        return;
    }
    auto& store = StrokeStore::GetInstance();
    if (!store.Ready()) {
        return;
    }
    int target = -1;
    {
        DisplayLockGuard lock(this);
        if (stroke_char_idx_ < 0) {
            target = store.GradeStart(1);
        } else {
            int grade = store.GradeAt(stroke_char_idx_);
            target = stroke_char_idx_ + dir;
            if (target < store.GradeStart(grade) || target >= store.GradeEnd(grade)) {
                target = dir > 0 ? store.GradeStart(grade) : store.GradeEnd(grade) - 1;
            }
            if (target == stroke_char_idx_) {
                return;
            }
        }
    }
    if (target >= 0) {
        ShowStrokeScreen(target);
    }
}

void LcdDisplay::OnStrokeCloseClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowStrokeScreen(false);
        self->CloseSubPageToDrawer();
    }
}

void LcdDisplay::OnStrokeReplayClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr && self->stroke_char_idx_ >= 0) {
        self->ShowStrokeScreen(self->stroke_char_idx_);
    }
}

void LcdDisplay::OnStrokePrevClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->StrokeChangeChar(-1);
    }
}

void LcdDisplay::OnStrokeNextClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->StrokeChangeChar(1);
    }
}

#else

void LcdDisplay::ShowStrokeScreen(int char_idx) { (void)char_idx; }

void LcdDisplay::ShowStrokeScreen(bool show) { (void)show; }

void LcdDisplay::StrokeChangeChar(int dir) { (void)dir; }

#endif

bool LcdDisplay::DrawerEnglishEnabled() const {
#if CONFIG_FEATURE_ENGLISH_DICTATION
    return EnglishPlayer::Available();
#else
    return false;
#endif
}

bool LcdDisplay::DrawerStrokeEnabled() const {
#if CONFIG_FEATURE_STROKE_ORDER
    return StrokeStore::GetInstance().Ready();
#else
    return false;
#endif
}

bool LcdDisplay::DrawerBleKeyboardEnabled() const {
#if CONFIG_FEATURE_BLE_KEYBOARD
    return true;
#else
    return false;
#endif
}

void LcdDisplay::RefreshDrawerTiles() {
    for (int i = 0; i < kDrawerTileCount; ++i) {
        if (drawer_tiles_[i] == nullptr) {
            continue;
        }
        const DrawerTileSpec& spec = kDrawerSpecs[i];
        const bool enabled = (this->*spec.enabled)();
        // 未实现（pending）或依赖不满足时整体降透明度，点击给出原因。
        const lv_opa_t opa = (spec.pending || !enabled) ? LV_OPA_40 : LV_OPA_COVER;
        lv_obj_set_style_opa(drawer_tiles_[i], opa, 0);
        lv_obj_set_style_opa(drawer_tile_boxes_[i], opa, 0);
        lv_obj_set_style_opa(drawer_tile_labels_[i], opa, 0);
        lv_obj_set_style_opa(drawer_tile_icons_[i], opa, 0);
    }
}

void LcdDisplay::ShowDrawerHint(const char* text) {
    if (drawer_hint_ == nullptr) {
        return;
    }
    lv_label_set_text(drawer_hint_, text != nullptr ? text : "");
    lv_obj_remove_flag(drawer_hint_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(drawer_hint_);
    if (drawer_hint_timer_ == nullptr) {
        drawer_hint_timer_ = lv_timer_create(&LcdDisplay::OnDrawerHintTimer, 1600, this);
    } else {
        lv_timer_reset(drawer_hint_timer_);
    }
}

void LcdDisplay::OnDrawerHintTimer(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    lv_timer_pause(timer);
    if (self->drawer_hint_ != nullptr) {
        lv_obj_add_flag(self->drawer_hint_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ShowDrawerScreen(bool show) {
    if (drawer_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    drawer_screen_visible_ = show;
    if (show) {
        RefreshDrawerTiles();
        lv_obj_remove_flag(drawer_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(drawer_screen_);
    } else {
        lv_obj_add_flag(drawer_screen_, LV_OBJ_FLAG_HIDDEN);
        if (drawer_hint_ != nullptr) {
            lv_obj_add_flag(drawer_hint_, LV_OBJ_FLAG_HIDDEN);
        }
        if (drawer_hint_timer_ != nullptr) {
            lv_timer_pause(drawer_hint_timer_);
        }
    }
}

void LcdDisplay::OpenSettingsPage() { ShowSettingsScreen(true); }

void LcdDisplay::OpenEnglishPage() {
#if CONFIG_FEATURE_ENGLISH_DICTATION
    ShowEnglishSetup(true);
#endif
}

void LcdDisplay::OpenStrokePage() {
#if CONFIG_FEATURE_STROKE_ORDER
    auto& store = StrokeStore::GetInstance();
    const int idx = stroke_char_idx_ >= 0 ? stroke_char_idx_ : store.GradeStart(1);
    if (store.Ready() && idx >= 0) {
        ShowStrokeScreen(idx);
    }
#endif
}

#if CONFIG_FEATURE_BLE_KEYBOARD
void LcdDisplay::OpenMediaPage() { ShowMediaScreen(true); }
#else
void LcdDisplay::OpenMediaPage() {}
#endif

#if CONFIG_FEATURE_WEB_FILE_TRANSFER
void LcdDisplay::OpenFileServicePage() { ShowFileServiceScreen(true); }
#else
void LcdDisplay::OpenFileServicePage() {}
#endif

void LcdDisplay::OnDrawerHandleClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->MarkUserActivity();
    self->ShowDrawerScreen(!self->drawer_screen_visible_);
}

void LcdDisplay::OnDrawerBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowDrawerScreen(false);
    }
}

void LcdDisplay::OnDrawerTileClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    const auto idx = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
    if (idx < 0 || idx >= kDrawerTileCount) {
        return;
    }
    const DrawerTileSpec& spec = kDrawerSpecs[idx];
    self->MarkUserActivity();
    if (spec.pending || !(self->*spec.enabled)()) {
        if (spec.hint != nullptr && spec.hint[0] != '\0') {
            self->ShowDrawerHint(spec.hint);
        }
        return;
    }
    self->return_to_drawer_ = true;
    self->ShowDrawerScreen(false);
    (self->*spec.action)();
}

#if CONFIG_FEATURE_BLE_KEYBOARD
void LcdDisplay::OnMediaBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowMediaScreen(false);
        self->CloseSubPageToDrawer();
    }
}

static const MediaBleKey kMediaKeys[6] = {MediaBleKey::kPrevious, MediaBleKey::kPlayPause,
                                          MediaBleKey::kNext,     MediaBleKey::kVolumeDown,
                                          MediaBleKey::kMute,     MediaBleKey::kVolumeUp};
static const bool kMediaRepeat[6] = {false, false, false, true, false, true};
static const char* kMediaHints[6] = {"上一首", "播放/暂停", "下一首", "音量-", "静音", "音量+"};

struct MediaShortcut {
    const char* label;
    uint8_t modifiers;
    uint8_t keycode;
};
static const MediaShortcut kShortcuts[10] = {
    {"调度", 0x01, 0x52},  // Ctrl+Up: Mission Control
    {"切换", 0x08, 0x2B},  // Cmd+Tab: app switcher
    {"锁屏", 0x09, 0x14},  // Ctrl+Cmd+Q
    {"截图", 0x0A, 0x20},  // Cmd+Shift+3
    {"聚焦", 0x08, 0x2C},  // Cmd+Space: Spotlight
    {"全屏", 0x09, 0x09},  // Ctrl+Cmd+F
    {"关闭", 0x08, 0x1A},  // Cmd+W
    {"桌面", 0x00, 0x44},  // F11: show desktop
    {"复制", 0x08, 0x06},  // Cmd+C
    {"粘贴", 0x08, 0x19},  // Cmd+V
};

void LcdDisplay::UpdateMediaPanelTitle() {
    if (media_title_ == nullptr) {
        return;
    }
    const char* title = media_panel_ == 0   ? "媒 体"
                        : media_panel_ == 1 ? "全键盘"
                                            : "数字键";
    lv_label_set_text(media_title_, title);
}

void LcdDisplay::SetMediaPanel(int panel) {
    media_panel_ = ((panel % 3) + 3) % 3;
    for (int i = 0; i < 16; ++i) {
        if (media_keys_[i] == nullptr) {
            continue;
        }
        if (media_panel_ == 0) {
            lv_obj_remove_flag(media_keys_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(media_keys_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (keyboard_panel_ != nullptr) {
        if (media_panel_ == 1) {
            lv_obj_remove_flag(keyboard_panel_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(keyboard_panel_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (numpad_panel_ != nullptr) {
        if (media_panel_ == 2) {
            lv_obj_remove_flag(numpad_panel_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(numpad_panel_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (media_feedback_timer_ != nullptr) {
        lv_timer_pause(media_feedback_timer_);
    }
    UpdateMediaPanelTitle();
}

void LcdDisplay::OnMediaPanelCycle(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->SetMediaPanel(self->media_panel_ + 1);
    }
}

void LcdDisplay::OnKeyboardKeyClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(e);
    if (target == nullptr) {
        return;
    }
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));
    const uint8_t code = static_cast<uint8_t>(encoded & 0xFF);
    const uint8_t base_mods = static_cast<uint8_t>((encoded >> 8) & 0xFF);
    if (code == 0xFD) {  // 123: switch to the numeric panel
        self->SetMediaPanel(2);
        return;
    }
    if (code == 0xFE) {  // Ctrl+Space: switch input language
        const bool sent = MediaBle::GetInstance().SendShortcut(0x01, 0x2C);
        self->ShowMediaFeedback(sent ? "中英切换" : "蓝牙未连接");
        return;
    }
    if (code == 0xFF) {  // shift / caps toggle
        self->shift_active_ = !self->shift_active_;
        if (self->kb_shift_btn_ != nullptr) {
            lv_obj_set_style_bg_opa(self->kb_shift_btn_,
                                    self->shift_active_ ? LV_OPA_30 : LV_OPA_TRANSP, 0);
            if (self->media_title_ != nullptr) {
                lv_obj_set_style_bg_color(
                    self->kb_shift_btn_, lv_obj_get_style_text_color(self->media_title_, LV_PART_MAIN), 0);
            }
        }
        self->ShowMediaFeedback(self->shift_active_ ? "大写" : "小写");
        return;
    }
    const bool shiftable =
        self->media_panel_ == 1 &&
        ((code >= 0x04 && code <= 0x1D) || (code >= 0x2D && code <= 0x38));
    const uint8_t modifiers =
        static_cast<uint8_t>(base_mods | ((self->shift_active_ && shiftable) ? 0x02 : 0));
    const bool sent = MediaBle::GetInstance().SendShortcut(modifiers, code);
    const char* hint = nullptr;
    switch (code) {
        case 0x50: hint = "左"; break;
        case 0x52: hint = "上"; break;
        case 0x51: hint = "下"; break;
        case 0x4F: hint = "右"; break;
        default: break;
    }
    if (hint == nullptr) {
        lv_obj_t* label = lv_obj_get_child(target, 0);
        if (label != nullptr) {
            hint = lv_label_get_text(label);
        }
    }
    self->ShowMediaFeedback(sent ? (hint != nullptr ? hint : "已发送") : "蓝牙未连接");
}

void LcdDisplay::ShowMediaFeedback(const char* text) {
    if (media_title_ == nullptr) {
        return;
    }
    lv_label_set_text(media_title_, text != nullptr ? text : "媒 体");
    if (media_feedback_timer_ != nullptr) {
        lv_timer_reset(media_feedback_timer_);
        lv_timer_resume(media_feedback_timer_);
    }
}

void LcdDisplay::MediaFeedbackTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->UpdateMediaPanelTitle();
    }
    lv_timer_pause(timer);
}

void LcdDisplay::HandleMediaKeyDown(int index) {
    if (index < 0 || index >= 16) {
        return;
    }
    if (index < 6) {
        const bool sent = MediaBle::GetInstance().SendKey(kMediaKeys[index]);
        ShowMediaFeedback(sent ? kMediaHints[index] : "蓝牙未连接");
        if (index == 1) {  // play/pause icon toggle
            media_playing_ = !media_playing_;
            if (media_key_icons_[1] != nullptr) {
                lv_label_set_text(media_key_icons_[1], media_playing_
                                                           ? MATERIAL_SYMBOLS_PAUSE
                                                           : MATERIAL_SYMBOLS_PLAY_ARROW);
            }
        }
        if (kMediaRepeat[index]) {
            media_repeat_key_ = index;
            media_repeat_active_ = true;
            media_repeat_next_ = lv_tick_get() + 400;
            if (media_repeat_timer_ != nullptr) {
                lv_timer_resume(media_repeat_timer_);
            }
        }
        return;
    }
    const MediaShortcut& shortcut = kShortcuts[index - 6];
    const bool sent = MediaBle::GetInstance().SendShortcut(shortcut.modifiers, shortcut.keycode);
    ShowMediaFeedback(sent ? shortcut.label : "蓝牙未连接");
}

void LcdDisplay::HandleMediaKeyUp() {
    media_repeat_active_ = false;
    media_repeat_key_ = -1;
    if (media_repeat_timer_ != nullptr) {
        lv_timer_pause(media_repeat_timer_);
    }
}

void LcdDisplay::OnMediaKeyClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        self->HandleMediaKeyUp();
        return;
    }
    if (code != LV_EVENT_PRESSED) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (int i = 0; i < 16; ++i) {
        if (target == self->media_keys_[i]) {
            self->HandleMediaKeyDown(i);
            return;
        }
    }
}

void LcdDisplay::MediaRepeatTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || !self->media_repeat_active_ || self->media_repeat_key_ < 0) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if (now < self->media_repeat_next_) {
        return;
    }
    self->media_repeat_next_ = now + 120;
    MediaBle::GetInstance().SendKey(kMediaKeys[self->media_repeat_key_]);
}

void LcdDisplay::OnBluetoothEntryClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowBluetoothScreen(true);
    }
}

static std::string FormatBtAddress(const MediaBlePeer& peer) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", peer.addr[0], peer.addr[1],
             peer.addr[2], peer.addr[3], peer.addr[4], peer.addr[5]);
    return std::string(buffer);
}

static bool SameBtPeer(const MediaBlePeer& a, const MediaBlePeer& b) {
    return a.type == b.type && memcmp(a.addr, b.addr, 6) == 0;
}

void LcdDisplay::RefreshBluetoothList() {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    if (bt_list_ == nullptr || theme == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    auto& ble = MediaBle::GetInstance();
    if (!bt_user_enabled_ || !ble.IsRunning()) {
        lv_obj_clean(bt_list_);
        bt_peer_count_ = 0;
        for (auto& button : bt_forget_buttons_) {
            button = nullptr;
        }
        if (bt_status_ != nullptr) {
            lv_label_set_text(bt_status_, bt_busy_ ? "正在关闭…" : "蓝牙已关闭");
        }
        if (bt_empty_ != nullptr) {
            lv_obj_add_flag(bt_empty_, LV_OBJ_FLAG_HIDDEN);
        }
        if (bt_disconnect_btn_ != nullptr) {
            lv_obj_add_flag(bt_disconnect_btn_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    bt_peer_count_ = ble.GetBondedPeers(bt_peers_, 8);
    MediaBlePeer connected = {};
    const bool has_conn = ble.GetConnectedPeer(&connected);

    lv_obj_clean(bt_list_);
    for (auto& button : bt_forget_buttons_) {
        button = nullptr;
    }
    if (bt_status_ != nullptr) {
        if (has_conn) {
            const std::string text = "已连接 " + FormatBtAddress(connected);
            lv_label_set_text(bt_status_, text.c_str());
        } else {
            lv_label_set_text(bt_status_, "广播中 · 等待配对");
        }
    }
    if (bt_disconnect_btn_ != nullptr) {
        if (has_conn) {
            lv_obj_remove_flag(bt_disconnect_btn_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bt_disconnect_btn_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (bt_peer_count_ == 0) {
        if (bt_empty_ != nullptr) {
            lv_obj_remove_flag(bt_empty_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (bt_empty_ != nullptr) {
        lv_obj_add_flag(bt_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < bt_peer_count_; ++i) {
        lv_obj_t* row = lv_obj_create(bt_list_);
        lv_obj_set_size(row, 224, 40);
        lv_obj_set_style_bg_color(row, theme->text_color(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_10, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t* label = lv_label_create(row);
        lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(label, theme->text_color(), 0);
        const bool is_connected = has_conn && SameBtPeer(bt_peers_[i], connected);
        const std::string text = (is_connected ? "● " : "") + FormatBtAddress(bt_peers_[i]);
        lv_label_set_text(label, text.c_str());
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t* forget = lv_button_create(row);
        bt_forget_buttons_[i] = forget;
        lv_obj_set_size(forget, 34, 34);
        lv_obj_align(forget, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_opa(forget, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(forget, 0, 0);
        lv_obj_set_style_shadow_width(forget, 0, 0);
        lv_obj_set_style_pad_all(forget, 0, 0);
        lv_obj_set_ext_click_area(forget, 8);
        lv_obj_add_event_cb(forget, &LcdDisplay::OnBtForgetClicked, LV_EVENT_PRESSED, this);
        lv_obj_t* icon = lv_label_create(forget);
        lv_obj_set_style_text_font(icon, theme->icon_font()->font(), 0);
        lv_obj_set_style_text_color(icon, theme->text_color(), 0);
        lv_label_set_text(icon, MATERIAL_SYMBOLS_CLOSE);
        lv_obj_center(icon);
    }
}

void LcdDisplay::OnBtDisconnectClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    MediaBle::GetInstance().Disconnect();
    self->RefreshBluetoothList();
}

void LcdDisplay::OnBtForgetClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (int i = 0; i < self->bt_peer_count_; ++i) {
        if (target == self->bt_forget_buttons_[i]) {
            MediaBle::GetInstance().Unpair(self->bt_peers_[i]);
            self->RefreshBluetoothList();
            return;
        }
    }
}

void LcdDisplay::OnBluetoothBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowBluetoothScreen(false);
    }
}

#endif  // CONFIG_FEATURE_BLE_KEYBOARD

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (avatar_face_ != nullptr) {
        SetAvatarEmotion(emotion);
        return;
    }
#endif
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    auto fit_avatar = [this](const lv_image_dsc_t* dsc) {
        if (dsc != nullptr && dsc->header.w > 0 && dsc->header.h > 0) {
            lv_coord_t max_dim = LV_MAX(dsc->header.w, dsc->header.h);
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
            lv_img_set_zoom(emoji_image_, (56 * 256) / max_dim);
#else
            lv_img_set_zoom(emoji_image_, (64 * 256) / max_dim);
#endif
        }
    };
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this, fit_avatar]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
                fit_avatar(gif_controller_->image_dsc());
            });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            fit_avatar(gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        fit_avatar(image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font.  The theme's own font may already have been released when the
    // assets were replaced, so use the resident chat font where possible.
    auto text_font = SafeTextFont();
    if (text_font == nullptr) {
        text_font = lvgl_theme->text_font()->font();
    }
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    // Rebind the stroke, settings and English pages immediately: their labels
    // are not part of the chat layout and would still point at freed previous
    // fonts.  This must happen before anything triggers a label relayout.
    RebindStrokeFonts(text_font, icon_font, large_icon_font);
    RebindSettingsFonts(text_font, icon_font, large_icon_font);
    RebindEnglishFonts(text_font, icon_font);

    // Labels that use the icon font must follow the new theme font as well:
    // the old font is freed when the assets are replaced.
    // Anything still pointing at a freed font (from the previous theme) is
    // repaired here, before any layout runs.
    RepairDanglingFonts(text_font, icon_font, large_icon_font);

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    }
    if (sleep_zzz_ != nullptr) {
        lv_obj_set_style_text_font(sleep_zzz_, large_icon_font, 0);
    }
    if (page_progress_ != nullptr) {
        lv_obj_set_style_text_font(page_progress_, large_icon_font, 0);
    }
    if (avatar_think_dots_ != nullptr) {
        lv_obj_set_style_text_font(avatar_think_dots_, large_icon_font, 0);
    }

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_set_style_bg_color(emoji_box_, lvgl_theme->chat_background_color(), 0);
        lv_obj_set_style_border_color(emoji_box_, lvgl_theme->border_color(), 0);
    }
    if (page_progress_ != nullptr) {
        lv_obj_set_style_arc_color(page_progress_, lvgl_theme->border_color(), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(page_progress_, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(page_progress_, LV_OPA_TRANSP, LV_PART_INDICATOR);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
    // Rebind fonts/sizes: SetTextFont() replaces the theme font when the assets
    // partition loads, so the five-line viewport must be rebuilt with it.
    ApplyChatLayout();
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (chat_area_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(chat_area_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(chat_area_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}
