/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// Small raw-LVGL helpers shared by the anime skin parts.
#pragma once
#include <lvgl.h>

namespace stackchan::avatar::anime_draw {

// Plain filled shape without theme styles. Not clickable, so taps reach the panel.
inline lv_obj_t* shape(lv_obj_t* parent, lv_color_t color, int32_t radius, lv_opa_t opa = LV_OPA_COVER)
{
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// Transparent group used to move / rotate several shapes together.
inline lv_obj_t* group(lv_obj_t* parent, int32_t w, int32_t h)
{
    lv_obj_t* obj = shape(parent, lv_color_black(), 0, LV_OPA_TRANSP);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_transform_pivot_x(obj, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, h / 2, 0);
    return obj;
}

// Rounded stroke drawn as an arc (LVGL angles: 0 = right, clockwise; 90 = bottom).
inline lv_obj_t* stroke_arc(lv_obj_t* parent, lv_color_t color, int32_t width)
{
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_bg_angles(arc, 0, 360);
    return arc;
}

inline void set_arc(lv_obj_t* arc, int32_t size, int32_t start, int32_t end)
{
    lv_obj_set_size(arc, size, size);
    lv_obj_set_style_transform_pivot_x(arc, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(arc, size / 2, 0);
    lv_arc_set_angles(arc, start, end);
}

inline void set_hidden(lv_obj_t* obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// Element rotation is 0..3600 (0.1 deg); map it to a signed LVGL rotation.
inline int32_t signed_rotation(int rotation)
{
    rotation %= 3600;
    return rotation > 1800 ? rotation - 3600 : rotation;
}

}  // namespace stackchan::avatar::anime_draw
