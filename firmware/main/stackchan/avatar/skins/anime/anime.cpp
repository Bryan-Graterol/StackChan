/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "anime.h"
#include "anime_draw.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;
namespace draw = stackchan::avatar::anime_draw;

static constexpr int32_t BLUSH_X = 72;
static constexpr int32_t BLUSH_Y = 28;

AnimeAvatar::~AnimeAvatar()
{
    // Features own LVGL children of the panel: destroy them before the panel.
    _decorator_pool.clear();
    _key_elements.speechBubble.reset();
    _key_elements.mouth.reset();
    _key_elements.rightEye.reset();
    _key_elements.leftEye.reset();
    _pannel.reset();
}

void AnimeAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(320, 240);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(anime_palette::background());
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Cheeks (behind eyes and mouth)
    // Cheeks: hidden by default, a faint tint only when happy or angry
    _blush_left  = draw::shape(_pannel->get(), anime_palette::blush(), LV_RADIUS_CIRCLE, LV_OPA_TRANSP);
    _blush_right = draw::shape(_pannel->get(), anime_palette::blush(), LV_RADIUS_CIRCLE, LV_OPA_TRANSP);
    lv_obj_set_size(_blush_left, 30, 8);
    lv_obj_set_size(_blush_right, 30, 8);
    lv_obj_align(_blush_left, LV_ALIGN_CENTER, -BLUSH_X, BLUSH_Y);
    lv_obj_align(_blush_right, LV_ALIGN_CENTER, BLUSH_X, BLUSH_Y);

    _key_elements.leftEye  = std::make_unique<AnimeEyes>(_pannel->get(), true);
    _key_elements.rightEye = std::make_unique<AnimeEyes>(_pannel->get(), false);
    _key_elements.mouth    = std::make_unique<AnimeMouth>(_pannel->get());
    _key_elements.speechBubble = std::make_unique<DefaultSpeechBubble>(_pannel->get(), anime_palette::line(),
                                                                       anime_palette::background(), font);
}

Container* AnimeAvatar::getPanel() const
{
    return _pannel ? _pannel.get() : nullptr;
}

void AnimeAvatar::setEmotion(const Emotion& emotion)
{
    lv_opa_t opa = LV_OPA_TRANSP;
    switch (emotion) {
        case Emotion::Happy:
            opa = 90;
            break;
        case Emotion::Angry:
            opa = 60;
            break;
        default:
            break;
    }
    if (_blush_left && _blush_right) {
        lv_obj_set_style_bg_opa(_blush_left, opa, 0);
        lv_obj_set_style_bg_opa(_blush_right, opa, 0);
    }

    Avatar::setEmotion(emotion);
}
