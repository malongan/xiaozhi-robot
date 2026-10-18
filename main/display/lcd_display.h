#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LvglCBinFont;

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // Default message style: paged conversation history. One page shows up to
    // five wrapped lines; long messages span several pages. Pages live in RAM
    // only and are dropped when a new session starts.
    struct ChatMessage {
        std::string role;
        std::string text;                   // display text, includes the role prefix
        std::vector<uint32_t> line_starts;  // byte offsets of wrapped line starts
    };
    struct ChatPage {
        uint16_t message = 0;  // index into chat_messages_
        uint16_t line = 0;     // first wrapped line shown on the page
    };
    std::deque<ChatMessage> chat_messages_;
    std::vector<ChatPage> chat_pages_;
    int page_index_ = -1;
    bool follow_latest_ = true;
    lv_obj_t* chat_area_ = nullptr;
    lv_obj_t* page_progress_ = nullptr;  // faint press ring around the avatar
    LvglCBinFont* chat_font_ = nullptr;  // 18px Common font loaded from the assets partition

    void ApplyChatLayout();
    const lv_font_t* GetChatFont();
    void RewrapMessage(size_t index);
    void RewrapAllMessages();
    void RebuildChatPages();
    void RenderCurrentPage();
    void GoToPage(int index, bool show_indicator);
    void ShowPageIndicator();
    void ShowPressRing();
    void HidePressRing();
    std::string BuildPageText(const ChatPage& page) const;

    static void OnChatAreaClicked(lv_event_t* e);
    static void OnAvatarPressed(lv_event_t* e);
    static void OnAvatarReleased(lv_event_t* e);
    static void OnAvatarLongPressed(lv_event_t* e);
    static void OnMicButtonClicked(lv_event_t* e);

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual bool KeepsChatHistory() const override { return true; }
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy, bool double_buffer = false);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
