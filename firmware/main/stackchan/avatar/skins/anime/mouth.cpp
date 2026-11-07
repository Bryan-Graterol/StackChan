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

static constexpr int32_t MOUTH_Y     = 44;
static constexpr int32_t GAZE_X      = 8;
static constexpr int32_t GAZE_Y      = 6;
static constexpr int OPEN_THRESHOLD  = 12;  // Weight above this shows the open mouth

AnimeMouth::AnimeMouth(lv_obj_t* parent)
{
    _arc = draw::stroke_arc(parent, anime_palette::line(), 3);
    lv_obj_set_align(_arc, LV_ALIGN_CENTER);

    _open = draw::shape(parent, anime_palette::mouthInside(), LV_RADIUS_CIRCLE);
    lv_obj_set_align(_open, LV_ALIGN_CENTER);
    lv_obj_set_style_border_width(_open, 3, 0);
    lv_obj_set_style_border_color(_open, anime_palette::line(), 0);
    lv_obj_set_style_border_opa(_open, LV_OPA_COVER, 0);

    _tongue = draw::shape(_open, anime_palette::tongue(), LV_RADIUS_CIRCLE);

    _weight = 0;
    layout();
}

AnimeMouth::~AnimeMouth()
{
    if (_open) {
        lv_obj_delete(_open);
        _open = nullptr;
    }
    if (_arc) {
        lv_obj_delete(_arc);
        _arc = nullptr;
    }
}

void AnimeMouth::layout()
{
    int32_t pos_x = map_range(_position.x, -100, 100, -GAZE_X, GAZE_X);
    int32_t pos_y = MOUTH_Y + map_range(_position.y, -100, 100, -GAZE_Y, GAZE_Y);

    // Closed mouth curve per emotion: size, start/end angle, vertical shift
    // Calm, short shapes for a male look; Doubt is a one-sided smirk.
    int32_t size = 56, start = 72, end = 108, shift = -26;
    switch (_emotion) {
        case Emotion::Happy:
            size = 40, start = 30, end = 150, shift = -14;
            break;
        case Emotion::Sad:
            size = 44, start = 245, end = 295, shift = 20;
            break;
        case Emotion::Angry:
            size = 40, start = 240, end = 300, shift = 18;
            break;
        case Emotion::Doubt:
            size = 36, start = 40, end = 95, shift = -14;
            break;
        case Emotion::Sleepy:
            size = 40, start = 75, end = 105, shift = -18;
            break;
        default:
            break;
    }
    draw::set_arc(_arc, size, start, end);
    lv_obj_align(_arc, LV_ALIGN_CENTER, pos_x, pos_y + shift);

    // Open mouth grows with the voice level
    const int32_t w = 22 + _weight * 14 / 100;
    const int32_t h = 6 + _weight * 22 / 100;
    lv_obj_set_size(_open, w, h);
    lv_obj_set_style_radius(_open, h / 2 < 10 ? h / 2 : 10, 0);
    lv_obj_set_style_transform_pivot_x(_open, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(_open, h / 2, 0);
    lv_obj_align(_open, LV_ALIGN_CENTER, pos_x, pos_y);

    lv_obj_set_size(_tongue, w * 7 / 10, h / 2);
    lv_obj_align(_tongue, LV_ALIGN_BOTTOM_MID, 0, h / 6);

    const bool open = _weight > OPEN_THRESHOLD;
    draw::set_hidden(_arc, !_visible || open);
    draw::set_hidden(_open, !_visible || !open);
}

void AnimeMouth::setPosition(const Vector2i& position)
{
    Element::setPosition(position);
    layout();
}

void AnimeMouth::setWeight(int weight)
{
    Feature::setWeight(weight);
    layout();
}

void AnimeMouth::setRotation(int rotation)
{
    Element::setRotation(rotation);
    const int32_t r = draw::signed_rotation(_rotation);
    lv_obj_set_style_transform_rotation(_arc, r, 0);
    lv_obj_set_style_transform_rotation(_open, r, 0);
}

void AnimeMouth::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }
    _emotion = emotion;
    layout();
}

void AnimeMouth::setVisible(bool visible)
{
    Element::setVisible(visible);
    layout();
}
