/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// Original anime-style avatar skin (male look): sharp horizontal eyes with a
// blue gradient iris, strong expressive eyebrows and a calm mouth.
// Drop-in replacement for DefaultAvatar.
#pragma once
#include "../../avatar/avatar.h"
#include "../../avatar/elements/feature.h"
#include "../default/default.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <memory>

namespace stackchan::avatar {

namespace anime_palette {
inline lv_color_t background()
{
    return lv_color_hex(0x111827);  // Dark slate blue
}
inline lv_color_t line()
{
    return lv_color_hex(0xE8EEF7);  // Eye lines, happy eyes, mouth line
}
inline lv_color_t brow()
{
    return lv_color_hex(0xD6DEEA);
}
inline lv_color_t irisTop()
{
    return lv_color_hex(0x1E3A8A);  // Deep blue
}
inline lv_color_t irisBottom()
{
    return lv_color_hex(0x38BDF8);  // Sky blue
}
inline lv_color_t pupil()
{
    return lv_color_hex(0x0A0F1C);
}
inline lv_color_t blush()
{
    return lv_color_hex(0xF28B82);
}
inline lv_color_t mouthInside()
{
    return lv_color_hex(0x4A1D2A);
}
inline lv_color_t tongue()
{
    return lv_color_hex(0xE57A86);
}
}  // namespace anime_palette

class AnimeAvatar : public Avatar {
public:
    ~AnimeAvatar() override;

    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    uitk::lvgl_cpp::Container* getPanel() const;

    void setEmotion(const Emotion& emotion) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _pannel;
    lv_obj_t* _blush_left  = nullptr;
    lv_obj_t* _blush_right = nullptr;
};

class AnimeEyes : public Feature {
public:
    AnimeEyes(lv_obj_t* parent, bool isLeftEye);
    ~AnimeEyes();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const Emotion& emotion) override;
    void setVisible(bool visible) override;
    void setSize(int size) override;

private:
    bool _is_left_eye = false;
    bool _happy       = false;
    int _brow_tilt    = 0;  // 0.1 deg, positive lowers the inner end
    int _brow_raise   = 0;  // px, positive moves the brow up

    lv_obj_t* _container = nullptr;
    lv_obj_t* _eye       = nullptr;
    lv_obj_t* _pupil     = nullptr;
    lv_obj_t* _shine_big = nullptr;
    lv_obj_t* _shine_dot = nullptr;
    lv_obj_t* _eyelid    = nullptr;
    lv_obj_t* _lash      = nullptr;  // Upper eye line
    lv_obj_t* _lash_wing = nullptr;  // Subtle lower line at the outer corner
    lv_obj_t* _brow      = nullptr;
    lv_obj_t* _happy_arc = nullptr;

    void layout();
};

class AnimeMouth : public Feature {
public:
    AnimeMouth(lv_obj_t* parent);
    ~AnimeMouth();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const Emotion& emotion) override;
    void setVisible(bool visible) override;

private:
    Emotion _emotion = Emotion::Neutral;

    lv_obj_t* _arc    = nullptr;  // Closed mouth: smile / frown curve
    lv_obj_t* _open   = nullptr;  // Open mouth while talking
    lv_obj_t* _tongue = nullptr;

    void layout();
};

}  // namespace stackchan::avatar
