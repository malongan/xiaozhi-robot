#include "math_cannon.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_random.h>

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr int kBlockSize = 64;
constexpr float kCannonX = 120.0f;
constexpr float kCannonY = 286.0f;
constexpr float kBarrelLength = 30.0f;
constexpr float kBulletSpeed = 9.0f;

constexpr uint32_t kBgColor = 0x14141C;
constexpr uint32_t kBlockColor = 0xC0392B;
constexpr uint32_t kBlockBorder = 0x2A0A0A;
constexpr uint32_t kCannonColor = 0x4A4A55;

const int kBlockBaseX[3] = {12, 88, 164};
const int kBlockBaseY[3] = {104, 150, 104};

struct EnglishWord {
    const char* en;
    const char* zh;
    const char* wrong1;
    const char* wrong2;
};

const EnglishWord kWords[] = {
    {"apple", "苹果", "香蕉", "橙子"}, {"cat", "猫", "狗", "兔子"},
    {"dog", "狗", "猫", "猪"},         {"red", "红色", "蓝色", "绿色"},
    {"blue", "蓝色", "红色", "黄色"},  {"one", "一", "二", "三"},
    {"two", "二", "一", "三"},         {"three", "三", "一", "二"},
    {"book", "书", "笔", "尺子"},      {"sun", "太阳", "月亮", "星星"},
    {"fish", "鱼", "鸟", "虫"},        {"milk", "牛奶", "水", "果汁"},
};

int Rand(int n) { return n <= 0 ? 0 : static_cast<int>(esp_random() % static_cast<uint32_t>(n)); }

}  // namespace

void MathCannonGame::Create() {
    if (created_) {
        return;
    }
    created_ = true;

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root_, kWidth, kHeight);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBgColor), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(root_, &MathCannonGame::RootEventCb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(root_, &MathCannonGame::RootEventCb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(root_, &MathCannonGame::RootEventCb, LV_EVENT_RELEASED, this);

    back_btn_ = lv_button_create(root_);
    lv_obj_set_size(back_btn_, 44, 36);
    lv_obj_align(back_btn_, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_style_bg_opa(back_btn_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back_btn_, 0, 0);
    lv_obj_set_style_shadow_width(back_btn_, 0, 0);
    lv_obj_set_style_pad_all(back_btn_, 0, 0);
    lv_obj_add_event_cb(back_btn_, &MathCannonGame::BackEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back_btn_);
    lv_obj_set_style_text_font(back_label, font_, 0);
    lv_obj_set_style_text_color(back_label, text_color_, 0);
    lv_label_set_text(back_label, "<");
    lv_obj_center(back_label);

    hud_ = lv_label_create(root_);
    lv_obj_set_style_text_font(hud_, font_, 0);
    lv_obj_set_style_text_color(hud_, text_color_, 0);
    lv_obj_set_pos(hud_, 4, 4);

    question_ = lv_label_create(root_);
    lv_obj_set_width(question_, 236);
    lv_obj_set_style_text_align(question_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(question_, font_, 0);
    lv_obj_set_style_text_color(question_, text_color_, 0);
    lv_obj_set_pos(question_, 2, 30);

    for (int i = 0; i < 3; ++i) {
        blocks_[i] = lv_obj_create(root_);
        lv_obj_set_size(blocks_[i], kBlockSize, kBlockSize);
        lv_obj_set_style_radius(blocks_[i], 10, 0);
        lv_obj_set_style_bg_color(blocks_[i], lv_color_hex(kBlockColor), 0);
        lv_obj_set_style_bg_opa(blocks_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(blocks_[i], 2, 0);
        lv_obj_set_style_border_color(blocks_[i], lv_color_hex(kBlockBorder), 0);
        lv_obj_set_style_pad_all(blocks_[i], 0, 0);
        lv_obj_remove_flag(blocks_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(blocks_[i], LV_OBJ_FLAG_CLICKABLE);
        block_labels_[i] = lv_label_create(blocks_[i]);
        lv_obj_set_style_text_font(block_labels_[i], font_, 0);
        lv_obj_set_style_text_color(block_labels_[i], lv_color_white(), 0);
        lv_obj_center(block_labels_[i]);
    }

    aim_line_ = lv_line_create(root_);
    lv_line_set_points(aim_line_, aim_points_, 2);
    lv_obj_set_style_line_width(aim_line_, 2, 0);
    lv_obj_set_style_line_color(aim_line_, lv_color_white(), 0);
    lv_obj_set_style_line_opa(aim_line_, LV_OPA_40, 0);
    lv_obj_set_style_line_rounded(aim_line_, true, 0);
    lv_obj_remove_flag(aim_line_, LV_OBJ_FLAG_CLICKABLE);

    barrel_line_ = lv_line_create(root_);
    lv_line_set_points(barrel_line_, barrel_points_, 2);
    lv_obj_set_style_line_width(barrel_line_, 12, 0);
    lv_obj_set_style_line_color(barrel_line_, lv_color_hex(kCannonColor), 0);
    lv_obj_set_style_line_rounded(barrel_line_, true, 0);
    lv_obj_remove_flag(barrel_line_, LV_OBJ_FLAG_CLICKABLE);

    cannon_body_ = lv_obj_create(root_);
    lv_obj_set_size(cannon_body_, 30, 30);
    lv_obj_set_pos(cannon_body_, static_cast<lv_coord_t>(kCannonX - 15),
                   static_cast<lv_coord_t>(kCannonY - 15));
    lv_obj_set_style_radius(cannon_body_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cannon_body_, lv_color_hex(kCannonColor), 0);
    lv_obj_set_style_bg_opa(cannon_body_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cannon_body_, 2, 0);
    lv_obj_set_style_border_color(cannon_body_, lv_color_hex(0x1E1E26), 0);
    lv_obj_remove_flag(cannon_body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cannon_body_, LV_OBJ_FLAG_CLICKABLE);

    bullet_ = lv_obj_create(root_);
    lv_obj_set_size(bullet_, 10, 10);
    lv_obj_set_style_radius(bullet_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bullet_, lv_color_hex(0xFFD54F), 0);
    lv_obj_set_style_bg_opa(bullet_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bullet_, 1, 0);
    lv_obj_set_style_border_color(bullet_, lv_color_white(), 0);
    lv_obj_add_flag(bullet_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bullet_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bullet_, LV_OBJ_FLAG_CLICKABLE);

    result_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(result_label_, font_, 0);
    lv_obj_set_style_text_color(result_label_, lv_color_white(), 0);
    lv_obj_set_style_transform_pivot_x(result_label_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(result_label_, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(result_label_, 400, 0);
    lv_obj_align(result_label_, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(result_label_, LV_OBJ_FLAG_HIDDEN);

    gameover_box_ = lv_obj_create(root_);
    lv_obj_set_size(gameover_box_, kWidth, kHeight);
    lv_obj_set_pos(gameover_box_, 0, 0);
    lv_obj_set_style_radius(gameover_box_, 0, 0);
    lv_obj_set_style_bg_color(gameover_box_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gameover_box_, LV_OPA_70, 0);
    lv_obj_set_style_border_width(gameover_box_, 0, 0);
    lv_obj_set_style_pad_all(gameover_box_, 0, 0);
    lv_obj_remove_flag(gameover_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(gameover_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gameover_box_, LV_OBJ_FLAG_HIDDEN);

    gameover_title_ = lv_label_create(gameover_box_);
    lv_obj_set_style_text_font(gameover_title_, font_, 0);
    lv_obj_set_style_text_color(gameover_title_, lv_color_white(), 0);
    lv_label_set_text(gameover_title_, "游戏结束");
    lv_obj_align(gameover_title_, LV_ALIGN_CENTER, 0, -40);

    gameover_score_ = lv_label_create(gameover_box_);
    lv_obj_set_style_text_font(gameover_score_, font_, 0);
    lv_obj_set_style_text_color(gameover_score_, lv_color_white(), 0);
    lv_obj_align(gameover_score_, LV_ALIGN_CENTER, 0, 0);

    gameover_hint_ = lv_label_create(gameover_box_);
    lv_obj_set_style_text_font(gameover_hint_, font_, 0);
    lv_obj_set_style_text_color(gameover_hint_, lv_color_white(), 0);
    lv_label_set_text(gameover_hint_, "点击屏幕重新开始");
    lv_obj_align(gameover_hint_, LV_ALIGN_CENTER, 0, 44);

    timer_ = lv_timer_create(&MathCannonGame::TimerCb, 33, this);
    lv_timer_pause(timer_);

    Reset();
}

void MathCannonGame::Show() {
    Create();
    if (root_ == nullptr) {
        return;
    }
    visible_ = true;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    if (timer_ != nullptr) {
        lv_timer_resume(timer_);
    }
    LayoutAim();
    UpdateHud();
}

void MathCannonGame::Hide() {
    visible_ = false;
    if (root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
}

void MathCannonGame::Reset() {
    score_ = 0;
    lives_ = 3;
    combo_ = 0;
    state_ = State::kPlaying;
    bullet_active_ = false;
    result_frames_ = 0;
    if (bullet_ != nullptr) {
        lv_obj_add_flag(bullet_, LV_OBJ_FLAG_HIDDEN);
    }
    if (result_label_ != nullptr) {
        lv_obj_add_flag(result_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (gameover_box_ != nullptr) {
        lv_obj_add_flag(gameover_box_, LV_OBJ_FLAG_HIDDEN);
    }
    NextQuestion();
    UpdateHud();
}

void MathCannonGame::NextQuestion() {
    if (state_ == State::kResult) {
        lv_obj_add_flag(result_label_, LV_OBJ_FLAG_HIDDEN);
    }
    state_ = State::kPlaying;
    bullet_active_ = false;
    if (bullet_ != nullptr) {
        lv_obj_add_flag(bullet_, LV_OBJ_FLAG_HIDDEN);
    }

    char options[3][16] = {};
    const bool english = Rand(100) < 30;
    if (english) {
        const EnglishWord& word = kWords[Rand(sizeof(kWords) / sizeof(kWords[0]))];
        snprintf(question_buf_, sizeof(question_buf_), "%s = ?", word.en);
        snprintf(options[0], sizeof(options[0]), "%s", word.zh);
        snprintf(options[1], sizeof(options[1]), "%s", word.wrong1);
        snprintf(options[2], sizeof(options[2]), "%s", word.wrong2);
        correct_index_ = 0;
    } else {
        const int max_num = 10 + (combo_ / 3) * 3;
        const int op = Rand(4);
        int a = 0;
        int b = 0;
        int answer = 0;
        if (op == 0) {
            a = Rand(max_num) + 1;
            b = Rand(max_num) + 1;
            answer = a + b;
            snprintf(question_buf_, sizeof(question_buf_), "%d + %d = ?", a, b);
        } else if (op == 1) {
            a = Rand(max_num) + 1;
            b = Rand(a) + 1;
            answer = a - b;
            snprintf(question_buf_, sizeof(question_buf_), "%d - %d = ?", a, b);
        } else if (op == 2) {
            a = Rand(max_num > 12 ? 12 : max_num) + 1;
            b = Rand(max_num > 12 ? 12 : max_num) + 1;
            answer = a * b;
            snprintf(question_buf_, sizeof(question_buf_), "%d x %d = ?", a, b);
        } else {
            b = Rand(max_num > 12 ? 12 : max_num) + 1;
            answer = Rand(max_num > 12 ? 12 : max_num) + 1;
            a = b * answer;
            snprintf(question_buf_, sizeof(question_buf_), "%d / %d = ?", a, b);
        }
        snprintf(options[0], sizeof(options[0]), "%d", answer);
        int used[3] = {answer, -1, -1};
        for (int i = 1; i < 3; ++i) {
            int wrong = 0;
            do {
                wrong = Rand(max_num * 2) + 1;
            } while (wrong == used[0] || wrong == used[1] || wrong == used[2]);
            used[i] = wrong;
            snprintf(options[i], sizeof(options[i]), "%d", wrong);
        }
        correct_index_ = 0;
    }

    int order[3] = {0, 1, 2};
    for (int i = 2; i > 0; --i) {
        const int j = Rand(i + 1);
        const int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (int i = 0; i < 3; ++i) {
        snprintf(block_buf_[i], sizeof(block_buf_[i]), "%s", options[order[i]]);
        block_text_[i] = block_buf_[i];
        if (order[i] == correct_index_) {
            correct_index_ = i;
        }
        if (block_labels_[i] != nullptr) {
            lv_label_set_text(block_labels_[i], block_text_[i]);
        }
    }
    if (question_ != nullptr) {
        lv_label_set_text(question_, question_buf_);
    }
    if (result_label_ != nullptr) {
        lv_obj_add_flag(result_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (gameover_box_ != nullptr) {
        lv_obj_add_flag(gameover_box_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateHud();
    LayoutAim();
}

void MathCannonGame::UpdateHud() {
    if (hud_ == nullptr) {
        return;
    }
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "分%d  金%d  命%d  连%d", score_, gold_, lives_, combo_);
    lv_label_set_text(hud_, buffer);
}

void MathCannonGame::LayoutAim() {
    if (root_ == nullptr) {
        return;
    }
    float dx = aim_x_ - kCannonX;
    float dy = aim_y_ - kCannonY;
    if (dy > -10.0f) {
        dy = -10.0f;
    }
    const float length = sqrtf(dx * dx + dy * dy);
    if (length < 1.0f) {
        dx = 0.0f;
        dy = -1.0f;
    } else {
        dx /= length;
        dy /= length;
    }
    const float muzzle_x = kCannonX + dx * kBarrelLength;
    const float muzzle_y = kCannonY + dy * kBarrelLength;
    barrel_points_[0].x = kCannonX;
    barrel_points_[0].y = kCannonY;
    barrel_points_[1].x = muzzle_x;
    barrel_points_[1].y = muzzle_y;
    lv_line_set_points(barrel_line_, barrel_points_, 2);

    aim_points_[0].x = muzzle_x;
    aim_points_[0].y = muzzle_y;
    aim_points_[1].x = muzzle_x + dx * 260.0f;
    aim_points_[1].y = muzzle_y + dy * 260.0f;
    lv_line_set_points(aim_line_, aim_points_, 2);
    lv_obj_move_foreground(barrel_line_);
    lv_obj_move_foreground(cannon_body_);
    lv_obj_move_foreground(bullet_);
}

void MathCannonGame::Shoot() {
    if (!visible_ || state_ != State::kPlaying || bullet_active_) {
        return;
    }
    float dx = aim_x_ - kCannonX;
    float dy = aim_y_ - kCannonY;
    if (dy > -10.0f) {
        dy = -10.0f;
    }
    const float length = sqrtf(dx * dx + dy * dy);
    if (length < 1.0f) {
        return;
    }
    bullet_x_ = kCannonX + (dx / length) * kBarrelLength;
    bullet_y_ = kCannonY + (dy / length) * kBarrelLength;
    bullet_vx_ = (dx / length) * kBulletSpeed;
    bullet_vy_ = (dy / length) * kBulletSpeed;
    bullet_active_ = true;
    if (bullet_ != nullptr) {
        lv_obj_remove_flag(bullet_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(bullet_, static_cast<lv_coord_t>(bullet_x_ - 5),
                       static_cast<lv_coord_t>(bullet_y_ - 5));
        lv_obj_move_foreground(bullet_);
    }
}

void MathCannonGame::ShowResult(bool correct) {
    state_ = State::kResult;
    result_correct_ = correct;
    result_frames_ = 20;
    if (result_label_ != nullptr) {
        lv_label_set_text(result_label_, correct ? "√" : "×");
        lv_obj_set_style_text_color(result_label_, lv_color_hex(correct ? 0x4CAF50 : 0xF44336), 0);
        lv_obj_remove_flag(result_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(result_label_);
    }
    if (correct) {
        score_ += 100;
        gold_ += 5 + (combo_ / 3) * 5;
        combo_++;
    } else {
        combo_ = 0;
        lives_--;
    }
    UpdateHud();
}

void MathCannonGame::EnterGameOver() {
    state_ = State::kGameOver;
    if (gameover_box_ != nullptr) {
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "得分 %d    金币 %d", score_, gold_);
        if (gameover_score_ != nullptr) {
            lv_label_set_text(gameover_score_, buffer);
        }
        lv_obj_remove_flag(gameover_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(gameover_box_);
    }
}

void MathCannonGame::Update() {
    if (!visible_ || root_ == nullptr) {
        return;
    }
    block_float_ += 0.06f;
    for (int i = 0; i < 3; ++i) {
        block_y_offset_[i] = sinf(block_float_ + block_phase_[i]) * 6.0f;
        if (blocks_[i] != nullptr) {
            lv_obj_set_pos(blocks_[i], kBlockBaseX[i],
                           static_cast<lv_coord_t>(kBlockBaseY[i] + block_y_offset_[i]));
        }
    }
    LayoutAim();

    if (bullet_active_) {
        bullet_x_ += bullet_vx_;
        bullet_y_ += bullet_vy_;
        if (bullet_x_ < -10 || bullet_x_ > kWidth + 10 || bullet_y_ < -10 ||
            bullet_y_ > kHeight + 10) {
            bullet_active_ = false;
            if (bullet_ != nullptr) {
                lv_obj_add_flag(bullet_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (bullet_ != nullptr) {
                lv_obj_set_pos(bullet_, static_cast<lv_coord_t>(bullet_x_ - 5),
                               static_cast<lv_coord_t>(bullet_y_ - 5));
            }
            for (int i = 0; i < 3; ++i) {
                const float by = kBlockBaseY[i] + block_y_offset_[i];
                if (bullet_x_ > kBlockBaseX[i] && bullet_x_ < kBlockBaseX[i] + kBlockSize &&
                    bullet_y_ > by && bullet_y_ < by + kBlockSize) {
                    bullet_active_ = false;
                    if (bullet_ != nullptr) {
                        lv_obj_add_flag(bullet_, LV_OBJ_FLAG_HIDDEN);
                    }
                    ShowResult(i == correct_index_);
                    break;
                }
            }
        }
    }

    if (state_ == State::kResult) {
        if (result_frames_ > 0) {
            result_frames_--;
        } else {
            if (result_label_ != nullptr) {
                lv_obj_add_flag(result_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (!result_correct_ && lives_ <= 0) {
                EnterGameOver();
            } else {
                NextQuestion();
            }
        }
    }
}

void MathCannonGame::HandleTouch(lv_coord_t x, lv_coord_t y, bool released) {
    if (state_ == State::kGameOver) {
        if (released) {
            Reset();
        }
        return;
    }
    aim_x_ = static_cast<float>(x);
    aim_y_ = static_cast<float>(y);
    LayoutAim();
    if (released) {
        Shoot();
    }
}

void MathCannonGame::TimerCb(lv_timer_t* timer) {
    auto* self = static_cast<MathCannonGame*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Update();
    }
}

void MathCannonGame::RootEventCb(lv_event_t* e) {
    auto* self = static_cast<MathCannonGame*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    if (lv_event_get_target(e) != self->root_) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const lv_event_code_t code = lv_event_get_code(e);
    self->HandleTouch(point.x, point.y, code == LV_EVENT_RELEASED);
}

void MathCannonGame::BackEventCb(lv_event_t* e) {
    auto* self = static_cast<MathCannonGame*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->Hide();
    }
}

void MathCannonGame::RebindFonts(const lv_font_t* font, lv_color_t text_color) {
    if (font == nullptr) {
        return;
    }
    font_ = font;
    text_color_ = text_color;
    if (!created_) {
        return;
    }
    lv_obj_t* labels[] = {hud_,           question_,    gameover_title_, gameover_score_,
                          gameover_hint_, result_label_};
    for (lv_obj_t* label : labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (block_labels_[i] != nullptr) {
            lv_obj_set_style_text_font(block_labels_[i], font, 0);
        }
    }
}
