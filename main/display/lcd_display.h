#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"
#include "media_ble.h"
#include "stroke_tool.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
    int chat_lines_per_page_ = 6;  // 5 lines + media entry when idle, 6 while talking
    lv_obj_t* chat_area_ = nullptr;
    lv_obj_t* media_bar_ = nullptr;         // bottom keyboard icon, idle only
    lv_obj_t* media_screen_ = nullptr;      // full-screen media keys page
    lv_obj_t* bluetooth_screen_ = nullptr;  // paired-device list
    bool media_screen_visible_ = false;
    bool bluetooth_screen_visible_ = false;
    lv_obj_t* media_keys_[16] = {};
    lv_obj_t* shortcut_labels_[10] = {};
    int shortcut_label_count_ = 0;
    lv_obj_t* keyboard_panel_ = nullptr;
    lv_obj_t* numpad_panel_ = nullptr;
    lv_obj_t* keyboard_labels_[42] = {};
    int keyboard_label_count_ = 0;
    lv_obj_t* numpad_labels_[20] = {};
    int numpad_label_count_ = 0;
    lv_obj_t* numpad_icon_labels_[4] = {};
    int numpad_icon_label_count_ = 0;
    lv_obj_t* kb_shift_btn_ = nullptr;
    int media_panel_ = 0;
    bool shift_active_ = false;
    lv_timer_t* media_feedback_timer_ = nullptr;
    lv_obj_t* media_key_icons_[9] = {};
    lv_timer_t* media_repeat_timer_ = nullptr;
    int media_repeat_key_ = -1;
    bool media_repeat_active_ = false;
    uint32_t media_repeat_next_ = 0;
    bool media_playing_ = false;

    // 笔顺学习页：TF 卡数据（StrokeStore），入口按钮常驻右下角；仅默认聊天样式分支创建。
    static constexpr int kStrokeCanvasSide = 176;  // 画布边长
    static constexpr int kStrokeCanvasMargin = 8;  // 160px 字模在画布内的留白
    static constexpr int kStrokeGlyphSpan = 160;   // 字模边长
    static constexpr int kStrokeMaxStrokes = 48;   // 单字最大笔画数（字库最大 33，留余量）
    lv_obj_t* english_screen_ = nullptr;
    lv_obj_t* english_word_label_ = nullptr;
    lv_obj_t* english_phonetic_label_ = nullptr;
    lv_obj_t* english_meaning_label_ = nullptr;
    lv_obj_t* english_progress_label_ = nullptr;

    lv_obj_t* english_mode_label_ = nullptr;
    lv_obj_t* english_speak_label_ = nullptr;
    lv_obj_t* english_score_label_ = nullptr;
    lv_obj_t* english_title_label_ = nullptr;
    lv_obj_t* english_interval_label_ = nullptr;
    // 启动选择面板
    lv_obj_t* english_setup_panel_ = nullptr;
    lv_obj_t* english_setup_title_label_ = nullptr;
    lv_obj_t* english_setup_step1_ = nullptr;
    lv_obj_t* english_setup_step2_ = nullptr;
    lv_obj_t* english_setup_tab_text_ = nullptr;
    lv_obj_t* english_setup_tab_topic_ = nullptr;
    lv_obj_t* english_setup_list_ = nullptr;
    // 左右翻页按钮：内容列表不再滚动，避免滑动与点选互相干扰
    lv_obj_t* english_setup_page_left_ = nullptr;
    lv_obj_t* english_setup_page_right_ = nullptr;
    lv_obj_t* english_setup_page_left_icon_ = nullptr;
    lv_obj_t* english_setup_page_right_icon_ = nullptr;
    int english_setup_page_ = 0;
    int english_setup_page_count_ = 1;
    lv_obj_t* english_setup_rows_[35] = {};
    lv_obj_t* english_setup_row_names_[35] = {};
    lv_obj_t* english_setup_row_counts_[35] = {};
    int english_setup_row_values_[35] = {};
    int english_setup_row_count_ = 0;
    lv_obj_t* english_setup_next_label_ = nullptr;
    lv_obj_t* english_setup_chip_labels_[4][6] = {};
    int english_setup_chip_count_[4] = {0, 0, 0, 0};
    int english_setup_chip_values_[4][6] = {};
    int english_setup_step_ = 0;
    // 启动面板里所有用主题字体的标签（主题刷新时必须一起重绑）
    std::vector<lv_obj_t*> english_setup_labels_;
    lv_obj_t* english_setup_close_icon_ = nullptr;
    int english_setup_unit_ = 1;
    int english_setup_topic_ = 0;
    int english_setup_source_ = 0;  // 0=课文（按单元） 1=分类（按主题）
    int english_setup_count_ = 10;
    int english_setup_interval_ = 10;
    int english_setup_mode_ = 0;   // 0=报听写 1=单词卡
    int english_setup_speak_ = 0;  // 0=报英文 1=报中文 2=中英
    lv_obj_t* english_back_icon_ = nullptr;
    lv_obj_t* english_reset_icon_ = nullptr;
    lv_obj_t* stroke_screen_ = nullptr;
    lv_obj_t* stroke_canvas_ = nullptr;
    lv_obj_t* stroke_title_label_ = nullptr;
    lv_obj_t* stroke_pinyin_label_ = nullptr;
    lv_obj_t* stroke_info_label_ = nullptr;
    lv_obj_t* stroke_badges_[kStrokeMaxStrokes] = {};
    lv_obj_t* stroke_badge_labels_[kStrokeMaxStrokes] = {};
    lv_obj_t* stroke_close_icon_ = nullptr;
    void RebindStrokeFonts(const lv_font_t* text_font, const lv_font_t* icon_font,
                           const lv_font_t* large_icon_font);
    uint8_t* stroke_canvas_buf_ = nullptr;
    lv_color_t stroke_color_bg_;
    lv_color_t stroke_color_dim_;
    lv_color_t stroke_color_done_;
    std::vector<StrokeMeta> stroke_metas_;
    uint8_t stroke_bits_[StrokeStore::kGlyphBytes] = {};
    int stroke_char_idx_ = -1;
    int stroke_stroke_idx_ = 0;
    int stroke_pt_idx_ = 0;
    float stroke_pos_x_ = 0.0f;
    float stroke_pos_y_ = 0.0f;
    float stroke_radius_ = 3.0f;
    float stroke_speed_ = 2.0f;
    uint32_t stroke_reveal_tick_ = 0;
    int stroke_revealed_ = 0;
    lv_obj_t* stroke_lines_[kStrokeMaxStrokes] = {};
    std::vector<std::vector<lv_point_precise_t>> stroke_line_pts_ =
        std::vector<std::vector<lv_point_precise_t>>(kStrokeMaxStrokes);
    void StrokeHideAllLines();
    uint32_t stroke_done_tick_ = 0;
    bool stroke_screen_visible_ = false;
    bool english_screen_visible_ = false;
    bool english_setup_visible_ = false;
    uint32_t english_long_guard_ = 0;  // 长按"下一个"后抑制同一次松手产生的 CLICKED
    lv_timer_t* stroke_anim_timer_ = nullptr;

    float avatar_face_scale_ = 1.0f;  // 1.0 = 80px, 1.875 = 150px idle face
    uint32_t last_activity_tick_ = 0;
    bool avatar_sleeping_ = false;
    uint32_t sleep_start_tick_ = 0;
    lv_obj_t* sleep_zzz_ = nullptr;
    lv_obj_t* settings_screen_ = nullptr;
    lv_obj_t* settings_title_ = nullptr;
    lv_obj_t* settings_volume_label_ = nullptr;
    lv_obj_t* settings_volume_value_ = nullptr;
    lv_obj_t* settings_bright_label_ = nullptr;
    lv_obj_t* settings_bright_value_ = nullptr;
    lv_obj_t* settings_mic_label_ = nullptr;
    lv_obj_t* settings_mic_value_ = nullptr;
    lv_obj_t* settings_mic_minus_icon_ = nullptr;
    lv_obj_t* settings_mic_plus_icon_ = nullptr;
    lv_obj_t* settings_wifi_label_ = nullptr;
    lv_obj_t* settings_wifi_status_ = nullptr;
    lv_obj_t* settings_wifi_ssid_ = nullptr;
    lv_obj_t* settings_wifi_dot_ = nullptr;
    lv_obj_t* settings_ip_label_ = nullptr;
    lv_obj_t* settings_ip_value_ = nullptr;
    lv_obj_t* settings_bt_label_ = nullptr;
    lv_obj_t* settings_bt_status_ = nullptr;
    lv_obj_t* settings_back_icon_ = nullptr;
    lv_obj_t* settings_vol_minus_icon_ = nullptr;
    lv_obj_t* settings_vol_plus_icon_ = nullptr;
    lv_obj_t* settings_bright_minus_icon_ = nullptr;
    lv_obj_t* settings_bright_plus_icon_ = nullptr;
    lv_obj_t* settings_wifi_btn_label_ = nullptr;
    lv_obj_t* settings_bt_icon_ = nullptr;
    lv_obj_t* settings_bt_switch_ = nullptr;
    lv_obj_t* settings_bt_forget_label_ = nullptr;
    lv_obj_t* settings_reboot_label_ = nullptr;
    std::vector<lv_obj_t*> settings_icon_labels_;
    bool settings_screen_visible_ = false;
    bool bt_user_enabled_ = true;
    bool bt_busy_ = false;

    // 网络页：文件服务、WiFi、SD 容量与上传进度
    lv_obj_t* file_service_screen_ = nullptr;
    lv_obj_t* file_back_icon_ = nullptr;
    lv_obj_t* file_title_label_ = nullptr;
    lv_obj_t* file_service_name_label_ = nullptr;
    lv_obj_t* file_url_label_ = nullptr;
    lv_obj_t* file_hint_label_ = nullptr;
    lv_obj_t* file_status_dot_ = nullptr;
    lv_obj_t* file_status_label_ = nullptr;
    lv_obj_t* file_toggle_label_ = nullptr;
    lv_obj_t* file_wifi_dot_ = nullptr;
    lv_obj_t* file_wifi_label_ = nullptr;
    lv_obj_t* file_ip_label_ = nullptr;
    lv_obj_t* file_storage_title_label_ = nullptr;
    lv_obj_t* file_storage_label_ = nullptr;
    lv_obj_t* file_storage_bar_ = nullptr;
    lv_obj_t* file_activity_label_ = nullptr;
    lv_obj_t* file_transfer_state_label_ = nullptr;
    lv_obj_t* file_progress_bar_ = nullptr;
    lv_obj_t* file_foot_label_ = nullptr;
    std::vector<lv_obj_t*> file_icon_labels_;
    bool file_service_visible_ = false;
    // 从功能抽屉进入二级页时置位；返回时据此回到抽屉而不是待机画面。
    bool return_to_drawer_ = false;
    void CloseSubPageToDrawer();
    void ShowFileServiceScreen(bool show);
    void UpdateFileServiceValues();
    void UpdateFileTransferActivity(const std::string& name, int percent, bool done, bool error);
    static void OnFileServiceBackClicked(lv_event_t* e);
    static void OnFileServiceToggleClicked(lv_event_t* e);

    // 功能抽屉：底部点状把手展开的彩色图标宫格页（表驱动，新增功能只加一行）
    struct DrawerTileSpec {
        const char* icon;
        const char* label;
        void (LcdDisplay::*action)();
        bool (LcdDisplay::*enabled)() const;
        bool pending;
        const char* hint;
        uint32_t color_top;
        uint32_t color_bottom;
    };
    static constexpr int kDrawerTileCount = 5;
    static const DrawerTileSpec kDrawerSpecs[kDrawerTileCount];
    lv_obj_t* drawer_screen_ = nullptr;
    lv_obj_t* drawer_back_icon_ = nullptr;
    lv_obj_t* drawer_title_ = nullptr;
    lv_obj_t* drawer_hint_ = nullptr;
    lv_obj_t* drawer_tiles_[kDrawerTileCount] = {};
    lv_obj_t* drawer_tile_boxes_[kDrawerTileCount] = {};
    lv_obj_t* drawer_tile_icons_[kDrawerTileCount] = {};
    lv_obj_t* drawer_tile_labels_[kDrawerTileCount] = {};
    lv_timer_t* drawer_hint_timer_ = nullptr;
    bool drawer_screen_visible_ = false;
    void ShowDrawerScreen(bool show);
    void RefreshDrawerTiles();
    void ShowDrawerHint(const char* text);
    void OpenSettingsPage();
    void OpenEnglishPage();
    void OpenStrokePage();
    void OpenMediaPage();
    void OpenFileServicePage();
    bool DrawerAlwaysEnabled() const { return true; }
    bool DrawerEnglishEnabled() const;
    bool DrawerStrokeEnabled() const;
    static void OnDrawerHandleClicked(lv_event_t* e);
    static void OnDrawerBackClicked(lv_event_t* e);
    static void OnDrawerTileClicked(lv_event_t* e);
    static void OnDrawerHintTimer(lv_timer_t* timer);
    lv_obj_t* media_hint_ = nullptr;
    lv_obj_t* media_bt_icon_ = nullptr;
    lv_obj_t* media_title_ = nullptr;
    lv_obj_t* bt_title_ = nullptr;
    lv_obj_t* bt_empty_ = nullptr;
    lv_obj_t* bt_status_ = nullptr;
    lv_obj_t* bt_list_ = nullptr;
    MediaBlePeer bt_peers_[8] = {};
    int bt_peer_count_ = 0;
    lv_obj_t* bt_forget_buttons_[8] = {};
    lv_obj_t* bt_disconnect_btn_ = nullptr;
    lv_obj_t* bt_disconnect_label_ = nullptr;
    lv_obj_t* page_progress_ = nullptr;  // faint press ring around the avatar

    // Procedural round-face avatar (white circle + two pill eyes).
    lv_obj_t* avatar_face_ = nullptr;
    lv_obj_t* avatar_eye_left_ = nullptr;
    lv_obj_t* avatar_eye_right_ = nullptr;
    lv_obj_t* avatar_eye_hl_left_ = nullptr;  // highlight: fake 3D gloss
    lv_obj_t* avatar_eye_hl_right_ = nullptr;
    lv_obj_t* avatar_eye_happy_left_ = nullptr;
    lv_obj_t* avatar_eye_happy_right_ = nullptr;
    lv_obj_t* avatar_think_dots_ = nullptr;
    AvatarExpression avatar_override_ = kAvatarExpressionNone;
    uint32_t avatar_override_until_ = 0;

    // Idle/listening gaze: eyes wander in eight directions with zoom, tilt
    // and squint, while the whole face sways like a head looking around.
    float gaze_x_ = 0.0f;
    float gaze_y_ = 0.0f;
    float gaze_zoom_ = 1.0f;
    float gaze_eye_ = 1.0f;
    float gaze_tilt_ = 0.0f;
    float gaze_from_x_ = 0.0f;
    float gaze_from_y_ = 0.0f;
    float gaze_from_zoom_ = 1.0f;
    float gaze_from_eye_ = 1.0f;
    float gaze_from_tilt_ = 0.0f;
    float gaze_to_x_ = 0.0f;
    float gaze_to_y_ = 0.0f;
    float gaze_to_zoom_ = 1.0f;
    float gaze_to_eye_ = 1.0f;
    float gaze_to_tilt_ = 0.0f;
    uint32_t gaze_start_ = 0;
    uint32_t gaze_next_ = 0;

    // Procedural avatar animation (breathing, blink, state motion).
    lv_timer_t* avatar_anim_timer_ = nullptr;
    AvatarMood avatar_mood_ = kAvatarMoodIdle;
    uint32_t avatar_anim_epoch_ = 0;
    uint32_t avatar_blink_at_ = 0;
    uint32_t avatar_blink_start_ = 0;
    bool avatar_blink_single_eye_ = false;
    uint8_t avatar_blink_eye_ = 0;

    void ApplyChatLayout();
    const lv_font_t* GetChatFont();
    static void AvatarAnimTimerCb(lv_timer_t* timer);
    static void OnSettingsBackClicked(lv_event_t* e);
    static void OnVolumeDownClicked(lv_event_t* e);
    static void OnVolumeUpClicked(lv_event_t* e);
    static void OnBrightnessDownClicked(lv_event_t* e);
    static void OnBrightnessUpClicked(lv_event_t* e);
    static void OnMicGainDownClicked(lv_event_t* e);
    static void OnMicGainUpClicked(lv_event_t* e);
    static void OnWifiConfigClicked(lv_event_t* e);
    static void OnSettingsBtToggleClicked(lv_event_t* e);
    static void BtToggleTask(void* param);
    static void OnSettingsBtForgetClicked(lv_event_t* e);
    static void OnRebootClicked(lv_event_t* e);
    void ShowSettingsScreen(bool show);
    void UpdateSettingsValues();
    void RebindSettingsFonts(const lv_font_t* text_font, const lv_font_t* icon_font,
                             const lv_font_t* large_icon_font);
    void SetAvatarEmotion(const char* emotion);
    void PickAvatarGaze();
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
    static void OnMediaBackClicked(lv_event_t* e);
    static void OnMediaKeyClicked(lv_event_t* e);
    static void OnMediaPanelCycle(lv_event_t* e);
    static void OnKeyboardKeyClicked(lv_event_t* e);
    static void MediaFeedbackTimerCb(lv_timer_t* timer);
    void ShowMediaFeedback(const char* text);
    void SetMediaPanel(int panel);
    void UpdateMediaPanelTitle();
    static void MediaRepeatTimerCb(lv_timer_t* timer);
    void HandleMediaKeyDown(int index);
    void HandleMediaKeyUp();
    static void OnBluetoothEntryClicked(lv_event_t* e);
    static void OnBluetoothBackClicked(lv_event_t* e);
    static void OnBtForgetClicked(lv_event_t* e);
    static void OnBtDisconnectClicked(lv_event_t* e);
    void RefreshBluetoothList();
    void ShowMediaScreen(bool show);
    void ShowBluetoothScreen(bool show);
    void UpdateMediaBarVisibility();
    void UpdateChatViewport();
    // 笔顺页内部（调用方需已持 LVGL 锁；控件为空/数据未就绪时静默返回）
    static float StrokeMapFpx(uint8_t v);  // medians 0..255 → 画布坐标
    void StrokeLoadChar(int idx);
    void StrokeStartStroke(int s);
    void StrokeFinishStroke(int s);
    void StrokeDrawStroke(int s, lv_color_t color);
    void StrokeFillCanvas(lv_color_t color);
    void StrokeBlitGlyph(lv_color_t color);
    void StrokeStampDot(int cx, int cy, float r, lv_color_t color);
    void StrokeStampLine(float x0, float y0, float x1, float y1, float r, lv_color_t color);
    void StrokeAnimTick();
    static void StrokeAnimTimerCb(lv_timer_t* timer);
    static void OnStrokeCloseClicked(lv_event_t* e);
    static void OnStrokeReplayClicked(lv_event_t* e);
    static void OnStrokePrevClicked(lv_event_t* e);
    static void OnStrokeNextClicked(lv_event_t* e);
    static void OnEnglishBackClicked(lv_event_t* e);
    static void OnEnglishResetClicked(lv_event_t* e);
    static void OnEnglishPrevClicked(lv_event_t* e);
    static void OnEnglishNextClicked(lv_event_t* e);
    static void OnEnglishNextLongPressed(lv_event_t* e);
    static void OnEnglishModeClicked(lv_event_t* e);
    static void OnEnglishIntervalClicked(lv_event_t* e);
    static void OnEnglishSpeakClicked(lv_event_t* e);
    static void OnEnglishSetupBackClicked(lv_event_t* e);
    static void OnEnglishSetupSourceClicked(lv_event_t* e);
    static void OnEnglishSetupScopeRowClicked(lv_event_t* e);
    static void OnEnglishSetupChipClicked(lv_event_t* e);
    static void OnEnglishSetupNextClicked(lv_event_t* e);
    static void OnEnglishSetupPageClicked(lv_event_t* e);
    void EnglishSetupRebuildScopeList();
    void EnglishSetupShowStep(int step);
    void ShowEnglishSetup(bool show);
    void UpdateEnglishSetup();
    static void OnAvatarPressed(lv_event_t* e);
    static void OnAvatarReleased(lv_event_t* e);
    static void OnAvatarLongPressed(lv_event_t* e);
    static void OnUserActivity(lv_event_t* e);
    void MarkUserActivity();
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
    virtual void SetAvatarMood(AvatarMood mood) override;
    virtual void OnWakeWordDetected() override;
    virtual void SetIpAddress(const std::string& ip) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual bool KeepsChatHistory() const override { return true; }
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // 笔顺学习页：char_idx 显示指定汉字并从头播放；bool 控制显隐（MCP 语音入口）
    void ShowStrokeScreen(int char_idx);
    void ShowStrokeScreen(bool show);
    // 英语听写页
    void ShowEnglishScreen(bool show);
    void UpdateEnglishScreen();
    // 播放器的界面刷新回调（静态，因为播放器不持有 display）
    static void RefreshEnglishScreenStatic();
    // 覆盖基类：刷新英语页
    virtual void RefreshEnglishScreen() override;
    void RebindEnglishFonts(const lv_font_t* text_font, const lv_font_t* icon_font);
    // 当前有效的主题字体。主题刷新时 lvgl_theme 里的旧字体已被释放，
    // 统一返回主题字体，避免标签指向野指针。
    const lv_font_t* SafeTextFont() const;
    // 遍历整棵对象树，把所有指向已释放字体的标签换成安全字体。
    // 逐页维护标签清单总会遗漏，这是一劳永逸的兜底。
    void RepairDanglingFonts(const lv_font_t* text_font, const lv_font_t* icon_font,
                             const lv_font_t* large_icon_font);
    void StrokeChangeChar(int dir);  // dir = -1/+1，年级内循环
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
