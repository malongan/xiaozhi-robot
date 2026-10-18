#include "lcd_display.h"
#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "media_ble.h"

#include <cstdio>
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

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
        .buffer_size = static_cast<uint32_t>(width_ * 20),
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
        .buffer_size = static_cast<uint32_t>(width_ * 20),
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

    avatar_think_dots_ = lv_label_create(screen);
    lv_obj_set_style_text_font(avatar_think_dots_, text_font, 0);
    lv_obj_set_style_text_color(avatar_think_dots_, lvgl_theme->text_color(), 0);
    lv_label_set_text(avatar_think_dots_, "···");
    lv_obj_add_flag(avatar_think_dots_, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_add_event_cb(media_bar_, &LcdDisplay::OnMediaBarClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* keyboard = lv_obj_create(media_bar_);
    lv_obj_set_size(keyboard, 32, 20);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(keyboard, 5, 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyboard, 2, 0);
    lv_obj_set_style_border_color(keyboard, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(keyboard, 0, 0);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(keyboard, LV_SCROLLBAR_MODE_OFF);
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 4; ++col) {
            lv_obj_t* key = lv_obj_create(keyboard);
            lv_obj_set_size(key, 3, 2);
            lv_obj_set_pos(key, 4 + col * 6, 4 + row * 5);
            lv_obj_set_style_radius(key, 1, 0);
            lv_obj_set_style_bg_color(key, lvgl_theme->text_color(), 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(key, 0, 0);
            lv_obj_set_style_pad_all(key, 0, 0);
            lv_obj_remove_flag(key, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(key, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    lv_obj_t* spacebar = lv_obj_create(keyboard);
    lv_obj_set_size(spacebar, 10, 2);
    lv_obj_set_pos(spacebar, 11, 15);
    lv_obj_set_style_radius(spacebar, 1, 0);
    lv_obj_set_style_bg_color(spacebar, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_opa(spacebar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(spacebar, 0, 0);
    lv_obj_remove_flag(spacebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spacebar, LV_OBJ_FLAG_CLICKABLE);

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
    lv_obj_add_event_cb(media_back, &LcdDisplay::OnMediaBackClicked, LV_EVENT_CLICKED, this);
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
    lv_obj_add_event_cb(bt_entry, &LcdDisplay::OnBluetoothEntryClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* ai_entry = lv_button_create(media_screen_);
    lv_obj_set_size(ai_entry, 44, 36);
    lv_obj_align(ai_entry, LV_ALIGN_TOP_RIGHT, -52, 2);
    lv_obj_set_style_bg_opa(ai_entry, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ai_entry, 0, 0);
    lv_obj_set_style_shadow_width(ai_entry, 0, 0);
    lv_obj_set_style_pad_all(ai_entry, 0, 0);
    lv_obj_add_event_cb(ai_entry, &LcdDisplay::OnAiTalkClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* ai_icon = lv_label_create(ai_entry);
    lv_obj_set_style_text_font(ai_icon, icon_font, 0);
    lv_obj_set_style_text_color(ai_icon, lvgl_theme->text_color(), 0);
    lv_label_set_text(ai_icon, MATERIAL_SYMBOLS_MIC);
    lv_obj_center(ai_icon);

    media_bt_icon_ = lv_label_create(bt_entry);
    lv_obj_set_style_text_font(media_bt_icon_, icon_font, 0);
    lv_obj_set_style_text_color(media_bt_icon_, lv_color_hex(0x777777), 0);
    lv_label_set_text(media_bt_icon_, MATERIAL_SYMBOLS_BLUETOOTH);
    lv_obj_center(media_bt_icon_);

    const char* media_icons[6] = {
        MATERIAL_SYMBOLS_SKIP_PREVIOUS, MATERIAL_SYMBOLS_PLAY_ARROW,  MATERIAL_SYMBOLS_SKIP_NEXT,
        MATERIAL_SYMBOLS_VOLUME_OFF,    MATERIAL_SYMBOLS_VOLUME_DOWN, MATERIAL_SYMBOLS_VOLUME_UP,
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
    lv_obj_add_event_cb(bt_back, &LcdDisplay::OnBluetoothBackClicked, LV_EVENT_CLICKED, this);
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
    lv_obj_add_event_cb(bt_disconnect_btn_, &LcdDisplay::OnBtDisconnectClicked, LV_EVENT_CLICKED,
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

    chat_lines_per_page_ = 5;  // idle layout: five lines + media entry icon
    ApplyChatLayout();

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
    MediaBle::GetInstance().Start();

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

const lv_font_t* LcdDisplay::GetChatFont() {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* theme_font = lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr
                                      ? lvgl_theme->text_font()->font()
                                      : nullptr;
    if (chat_font_ == nullptr) {
        void* data = nullptr;
        size_t size = 0;
        if (Assets::GetInstance().GetAssetData("font_chat_common_18_4.bin", data, size)) {
            chat_font_ = new LvglCBinFont(data);
            ESP_LOGI(TAG, "Chat font loaded: font_chat_common_18_4.bin (%u bytes)", (unsigned)size);
        }
    }
    if (chat_font_ != nullptr && chat_font_->font() != nullptr) {
        if (theme_font != nullptr) {
            // Rare glyphs the 18px Common subset does not cover fall back to
            // the 20px UI font.
            chat_font_->SetFallback(theme_font);
        }
        return chat_font_->font();
    }
    return theme_font;
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
    auto text_font = lvgl_theme->text_font();
    if (text_font == nullptr || text_font->font() == nullptr) {
        return;
    }
    const lv_font_t* font = text_font->font();

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

    lv_obj_set_size(emoji_box_, kAvatarSize, kAvatarSize);
    lv_obj_align(emoji_box_, LV_ALIGN_TOP_MID, 0, avatar_y);

    if (page_progress_ != nullptr) {
        lv_obj_set_size(page_progress_, kAvatarSize + 14, kAvatarSize + 14);
        lv_obj_align_to(page_progress_, emoji_box_, LV_ALIGN_CENTER, 0, 0);
    }
    if (avatar_think_dots_ != nullptr) {
        lv_obj_align_to(avatar_think_dots_, emoji_box_, LV_ALIGN_TOP_RIGHT, 6, -4);
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
    lv_obj_t* themed_labels[] = {media_title_, media_hint_, bt_title_,
                                 bt_empty_,    bt_status_,  bt_disconnect_label_};
    for (lv_obj_t* label : themed_labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
        }
    }
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

void LcdDisplay::SetAvatarMood(AvatarMood mood) {
    DisplayLockGuard lock(this);
    if (avatar_mood_ == mood) {
        return;
    }
    avatar_mood_ = mood;
    avatar_anim_epoch_ = lv_tick_get();
    avatar_blink_at_ = avatar_anim_epoch_ + 2600 + (esp_random() % 3500);
    avatar_blink_start_ = 0;
    gaze_next_ = 0;
    if (mood != kAvatarMoodIdle) {
        // A conversation started: leave the media/paired screens so the user
        // sees the dialog immediately.
        ShowMediaScreen(false);
        ShowBluetoothScreen(false);
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
    if (self->media_screen_visible_ || self->bluetooth_screen_visible_) {
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
                    left_h_draw = LV_MAX(static_cast<lv_coord_t>(3),
                                         static_cast<lv_coord_t>(eye_left_h * factor));
                }
                if (blink_right) {
                    right_h_draw = LV_MAX(static_cast<lv_coord_t>(3),
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
        lv_obj_align(self->avatar_eye_happy_left_, LV_ALIGN_CENTER, -eye_gap, 0);
        lv_obj_align(self->avatar_eye_happy_right_, LV_ALIGN_CENTER, eye_gap, 0);
    }

    lv_obj_set_style_transform_scale_x(self->emoji_box_, 256 + breath, 0);
    lv_obj_set_style_transform_scale_y(self->emoji_box_, 256 + breath, 0);
    lv_obj_set_style_translate_x(self->emoji_box_, face_tx, 0);
    lv_obj_set_style_translate_y(self->emoji_box_, bob, 0);
    lv_obj_set_style_transform_rotation(self->emoji_box_, face_rot, 0);
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
    if (media_bar_ == nullptr) {
        return;
    }
    const bool show = chat_lines_per_page_ == 5 && !media_screen_visible_ &&
                      !bluetooth_screen_visible_ && avatar_mood_ == kAvatarMoodIdle;
    if (show) {
        lv_obj_remove_flag(media_bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(media_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ShowMediaScreen(bool show) {
    if (media_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    media_screen_visible_ = show;
    if (show) {
        ShowBluetoothScreen(false);
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
        lv_obj_remove_flag(bluetooth_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(bluetooth_screen_);
    } else {
        lv_obj_add_flag(bluetooth_screen_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateMediaBarVisibility();
}

void LcdDisplay::OnMediaBarClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowMediaScreen(true);
    }
}

void LcdDisplay::OnMediaBackClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->ShowMediaScreen(false);
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
    if (self != nullptr && self->media_title_ != nullptr) {
        lv_label_set_text(self->media_title_, "媒 体");
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

void LcdDisplay::OnAiTalkClicked(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->ShowMediaFeedback("AI 对话");
    Application::GetInstance().ToggleChatState();
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
        lv_obj_add_event_cb(forget, &LcdDisplay::OnBtForgetClicked, LV_EVENT_CLICKED, this);
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

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

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
