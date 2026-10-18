#ifndef MATH_CANNON_H
#define MATH_CANNON_H

#include <lvgl.h>
#include <cstdint>

// Native LVGL port of the "数字大炮" touch game: aim the cannon and shoot
// the answer block that matches the question. Practice for math + English.
class MathCannonGame {
public:
    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }
    void RebindFonts(const lv_font_t* font, lv_color_t text_color);

private:
    enum class State : uint8_t {
        kPlaying,
        kResult,
        kGameOver,
    };

    void Create();
    void Reset();
    void NextQuestion();
    void Update();
    void UpdateHud();
    void LayoutAim();
    void Shoot();
    void HandleTouch(lv_coord_t x, lv_coord_t y, bool released);
    void ShowResult(bool correct);
    void EnterGameOver();

    static void TimerCb(lv_timer_t* timer);
    static void RootEventCb(lv_event_t* e);
    static void BackEventCb(lv_event_t* e);

    bool created_ = false;
    bool visible_ = false;
    State state_ = State::kPlaying;
    int result_frames_ = 0;
    bool result_correct_ = false;

    int score_ = 0;
    int gold_ = 0;
    int lives_ = 3;
    int combo_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* hud_ = nullptr;
    lv_obj_t* question_ = nullptr;
    lv_obj_t* blocks_[3] = {};
    lv_obj_t* block_labels_[3] = {};
    lv_obj_t* bullet_ = nullptr;
    lv_obj_t* aim_line_ = nullptr;
    lv_obj_t* barrel_line_ = nullptr;
    lv_obj_t* cannon_body_ = nullptr;
    lv_obj_t* result_label_ = nullptr;
    lv_obj_t* gameover_box_ = nullptr;
    lv_obj_t* gameover_title_ = nullptr;
    lv_obj_t* gameover_score_ = nullptr;
    lv_obj_t* gameover_hint_ = nullptr;
    lv_obj_t* back_btn_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    lv_point_precise_t aim_points_[2] = {};
    lv_point_precise_t barrel_points_[2] = {};

    float aim_x_ = 120.0f;
    float aim_y_ = 60.0f;
    float bullet_x_ = 0.0f;
    float bullet_y_ = 0.0f;
    float bullet_vx_ = 0.0f;
    float bullet_vy_ = 0.0f;
    bool bullet_active_ = false;

    float block_float_ = 0.0f;
    float block_y_offset_[3] = {};
    float block_phase_[3] = {0.0f, 2.0f, 4.0f};
    const char* block_text_[3] = {};
    char question_buf_[32] = {};
    char block_buf_[3][16] = {};
    int correct_index_ = 0;

    const lv_font_t* font_ = nullptr;
    lv_color_t text_color_ = lv_color_white();
};

#endif  // MATH_CANNON_H
