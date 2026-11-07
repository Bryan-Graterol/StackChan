/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "anime.h"
#include "anime_draw.h"

using namespace uitk;
using namespace stackchan::avatar;
namespace draw = stackchan::avatar::anime_draw;

static constexpr int32_t BOX_W        = 104;  // Group box, big enough for scale 1.25 + brows
static constexpr int32_t BOX_H        = 116;
static constexpr int32_t EYE_X        = 68;   // Distance from screen center
static constexpr int32_t EYE_Y        = -16;
static constexpr int32_t GAZE_X       = 12;   // Whole-eye travel
static constexpr int32_t GAZE_Y       = 9;
static constexpr int32_t PUPIL_GAZE_X = 6;    // Extra pupil travel inside the eye
static constexpr int32_t PUPIL_GAZE_Y = 3;

AnimeEyes::AnimeEyes(lv_obj_t* parent, bool isLeftEye) : _is_left_eye(isLeftEye)
{
    _container = draw::group(parent, BOX_W, BOX_H);
    lv_obj_set_align(_container, LV_ALIGN_CENTER);

    // Iris: deep blue -> sky blue, sharp horizontal shape
    _eye = draw::shape(_container, anime_palette::irisTop(), 0);
    lv_obj_set_style_bg_grad_color(_eye, anime_palette::irisBottom(), 0);
    lv_obj_set_style_bg_grad_dir(_eye, LV_GRAD_DIR_VER, 0);

    _pupil     = draw::shape(_eye, anime_palette::pupil(), LV_RADIUS_CIRCLE);
    _shine_big = draw::shape(_eye, lv_color_white(), LV_RADIUS_CIRCLE);
    _shine_dot = draw::shape(_eye, lv_color_white(), LV_RADIUS_CIRCLE, 200);

    // Eyelid slides down over the iris (same color as the background)
    _eyelid = draw::shape(_eye, anime_palette::background(), 0);

    // Firm upper line + subtle lower line at the outer corner
    _lash      = draw::shape(_container, anime_palette::line(), LV_RADIUS_CIRCLE);
    _lash_wing = draw::shape(_container, anime_palette::line(), LV_RADIUS_CIRCLE, 150);

    // Strong eyebrow: main carrier of the expression
    _brow = draw::shape(_container, anime_palette::brow(), 4);

    // Flat "^" eye for happy expressions
    _happy_arc = draw::stroke_arc(_container, anime_palette::line(), 6);
    draw::set_hidden(_happy_arc, true);

    _weight    = 100;
    _brow_tilt = 40;
    layout();
}

AnimeEyes::~AnimeEyes()
{
    if (_container) {
        lv_obj_delete(_container);
        _container = nullptr;
    }
}

void AnimeEyes::layout()
{
    // Size: -100..100 -> scale 0.75..1.25 (in 1/1000)
    const int32_t s = 1000 + _size * 250 / 100;
    auto sc         = [s](int32_t v) { return v * s / 1000; };
    const int side  = _is_left_eye ? -1 : 1;  // -1 = outer corner on the left

    const int32_t eye_w = sc(56);
    const int32_t eye_h = sc(36);
    const int32_t eye_y = sc(10);

    // Whole eye follows the gaze
    int32_t pos_x = side * EYE_X + map_range(_position.x, -100, 100, -GAZE_X, GAZE_X);
    int32_t pos_y = EYE_Y + map_range(_position.y, -100, 100, -GAZE_Y, GAZE_Y);
    lv_obj_align(_container, LV_ALIGN_CENTER, pos_x, pos_y);

    lv_obj_set_size(_eye, eye_w, eye_h);
    lv_obj_set_style_radius(_eye, sc(14), 0);
    lv_obj_align(_eye, LV_ALIGN_CENTER, 0, eye_y);

    // Pupil and highlights drift a little more for depth
    int32_t gx = map_range(_position.x, -100, 100, -PUPIL_GAZE_X, PUPIL_GAZE_X);
    int32_t gy = map_range(_position.y, -100, 100, -PUPIL_GAZE_Y, PUPIL_GAZE_Y);

    lv_obj_set_size(_pupil, sc(14), sc(24));
    lv_obj_align(_pupil, LV_ALIGN_CENTER, gx, sc(2) + gy);

    lv_obj_set_size(_shine_big, sc(9), sc(9));
    lv_obj_align(_shine_big, LV_ALIGN_CENTER, sc(-9) + gx / 2, sc(-7) + gy / 2);

    lv_obj_set_size(_shine_dot, sc(4), sc(4));
    lv_obj_align(_shine_dot, LV_ALIGN_CENTER, sc(9) + gx / 2, sc(7) + gy / 2);

    // Eyelid: weight 100 = open, 0 = closed
    const int32_t closed = (100 - _weight) * eye_h / 100;
    lv_obj_set_size(_eyelid, eye_w, eye_h);
    lv_obj_align(_eyelid, LV_ALIGN_TOP_MID, 0, -eye_h + closed);

    // Upper line rides on the eyelid edge so a blink ends in a closed-eye line.
    // Outer end slightly lowered for a sharper look.
    const int32_t lash_y = eye_y - eye_h / 2 + closed - sc(1);
    lv_obj_set_size(_lash, sc(64), sc(5));
    lv_obj_set_style_transform_pivot_x(_lash, sc(64) / 2, 0);
    lv_obj_set_style_transform_pivot_y(_lash, sc(5) / 2, 0);
    lv_obj_set_style_transform_rotation(_lash, side * 50, 0);
    lv_obj_align(_lash, LV_ALIGN_CENTER, 0, lash_y);

    lv_obj_set_size(_lash_wing, sc(24), sc(3));
    lv_obj_align(_lash_wing, LV_ALIGN_CENTER, side * sc(17), eye_y + eye_h / 2 + sc(4));

    // Eyebrow: tilt (positive lowers the inner end) and raise per emotion.
    // Drops a little when the eyes close.
    const int32_t brow_w = sc(58);
    const int32_t brow_h = sc(8);
    const int32_t brow_y = eye_y - eye_h / 2 - sc(16) - _brow_raise + closed / 4;
    lv_obj_set_size(_brow, brow_w, brow_h);
    lv_obj_set_style_transform_pivot_x(_brow, brow_w / 2, 0);
    lv_obj_set_style_transform_pivot_y(_brow, brow_h / 2, 0);
    lv_obj_set_style_transform_rotation(_brow, -side * _brow_tilt, 0);
    lv_obj_align(_brow, LV_ALIGN_CENTER, side * sc(2), brow_y);

    draw::set_arc(_happy_arc, sc(64), 222, 318);
    lv_obj_align(_happy_arc, LV_ALIGN_CENTER, 0, sc(26));

    // Happy: show "^" instead of the open eye; brows stay visible
    const bool show_eye = _visible && !_happy;
    draw::set_hidden(_eye, !show_eye);
    draw::set_hidden(_lash, !show_eye);
    draw::set_hidden(_lash_wing, !show_eye);
    draw::set_hidden(_brow, !_visible);
    draw::set_hidden(_happy_arc, !(_visible && _happy));
}

void AnimeEyes::setPosition(const Vector2i& position)
{
    Element::setPosition(position);
    layout();
}

void AnimeEyes::setWeight(int weight)
{
    Feature::setWeight(weight);
    layout();
}

void AnimeEyes::setRotation(int rotation)
{
    Element::setRotation(rotation);
    lv_obj_set_style_transform_rotation(_container, draw::signed_rotation(_rotation), 0);
}

void AnimeEyes::setSize(int size)
{
    Feature::setSize(size);
    layout();
}

void AnimeEyes::setVisible(bool visible)
{
    Element::setVisible(visible);
    layout();
}

void AnimeEyes::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }

    // eye_tilt: positive lowers the inner corners. brow_tilt/raise drive the eyebrows.
    auto apply = [this](int weight, int eye_tilt, int brow_tilt, int brow_raise, bool happy) {
        _happy      = happy;
        _brow_tilt  = brow_tilt;
        _brow_raise = brow_raise;
        const int r = _is_left_eye ? eye_tilt : -eye_tilt;
        Element::setRotation((r + 3600) % 3600);
        lv_obj_set_style_transform_rotation(_container, r, 0);
        setWeight(weight);
    };

    switch (emotion) {
        case Emotion::Neutral:
            apply(100, 0, 40, 0, false);
            break;
        case Emotion::Happy:
            apply(100, 0, -60, 4, true);
            break;
        case Emotion::Angry:
            apply(72, 60, 230, -4, false);
            break;
        case Emotion::Sad:
            apply(80, -60, -210, 2, false);
            break;
        case Emotion::Doubt:
            // One brow raised, the other lowered
            if (_is_left_eye) {
                apply(95, 0, -120, 9, false);
            } else {
                apply(70, 0, 90, -2, false);
            }
            break;
        case Emotion::Sleepy:
            apply(28, 0, -30, -2, false);
            break;
        default:
            break;
    }
}
