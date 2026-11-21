/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "apple_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "gemini_client.h"
#include "logger.h"
#include "paths.h"
#include "prompt_presets.h"
#include "settings.h"
#include "theme.h"

#include <filesystem>
#include <string_view>

namespace screen {
namespace {

constexpr std::size_t kMaxPromptLength = 512;

bool is_printable_key(uint32_t key) {
    return key >= 32 && key <= 126;
}

} // namespace

AppleScreen::AppleScreen(viewmodel::BaseViewModel& view_model,
                         app::AssetManager& assets,
                         app::Settings& settings,
                         const app::Paths& paths)
    : BaseScreen(view_model, assets),
      settings_(settings),
      paths_(paths),
      prompt_buffer_() {
    init();
}

void AppleScreen::build_content(lv_obj_t* content) {
    auto* panel = lv_obj_create(content);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(panel, 12, 0);
    lv_obj_set_style_pad_right(panel, 12, 0);
    lv_obj_set_style_pad_top(panel, 8, 0);
    lv_obj_set_style_pad_bottom(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    title_label_ = lv_label_create(panel);
    lv_label_bind_text(title_label_, view_model().greeting_subject(), nullptr);
    auto* title_font = assets().load_standard_font(18, app::StandardFontWeight::Bold);
    lv_obj_set_style_text_font(title_label_, title_font ? title_font : &lv_font_montserrat_20, 0);
    reactive::bind_theme(title_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    auto* subtitle = lv_label_create(panel);
    lv_label_set_text(subtitle, "Type a prompt and press Enter to generate an image.");
    lv_obj_set_width(subtitle, 296);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    auto* subtitle_font = assets().load_standard_font(11, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(subtitle,
                               subtitle_font ? subtitle_font : &lv_font_montserrat_12,
                               0);
    reactive::bind_theme(subtitle, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    prompt_card_ = lv_obj_create(panel);
    lv_obj_remove_style_all(prompt_card_);
    lv_obj_set_width(prompt_card_, LV_PCT(100));
    lv_obj_set_style_radius(prompt_card_, 8, 0);
    lv_obj_set_style_pad_all(prompt_card_, 8, 0);
    lv_obj_set_style_pad_row(prompt_card_, 3, 0);
    lv_obj_set_flex_flow(prompt_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(prompt_card_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(prompt_card_, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
    lv_obj_set_style_border_width(prompt_card_, 1, 0);
    lv_obj_set_style_border_opa(prompt_card_, LV_OPA_COVER, 0);

    prompt_label_ = lv_label_create(prompt_card_);
    lv_label_set_text(prompt_label_, "Prompt");
    auto* field_font = assets().load_standard_font(11, app::StandardFontWeight::Bold);
    lv_obj_set_style_text_font(prompt_label_, field_font ? field_font : &lv_font_montserrat_12, 0);
    reactive::bind_theme(prompt_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    prompt_textarea_ = lv_textarea_create(prompt_card_);
    lv_obj_set_width(prompt_textarea_, LV_PCT(100));
    lv_obj_set_height(prompt_textarea_, 42);
    lv_textarea_set_one_line(prompt_textarea_, true);
    lv_textarea_set_max_length(prompt_textarea_, kMaxPromptLength);
    lv_textarea_set_placeholder_text(prompt_textarea_, "Describe the image to generate");
    lv_textarea_set_text(prompt_textarea_, prompt_buffer_.c_str());
    lv_textarea_set_cursor_pos(prompt_textarea_, LV_TEXTAREA_CURSOR_LAST);
    reactive::bind_theme(prompt_textarea_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    preview_card_ = lv_obj_create(panel);
    lv_obj_remove_style_all(preview_card_);
    lv_obj_set_width(preview_card_, LV_PCT(100));
    lv_obj_set_style_radius(preview_card_, 8, 0);
    lv_obj_set_style_pad_all(preview_card_, 8, 0);
    lv_obj_set_style_pad_row(preview_card_, 3, 0);
    lv_obj_set_flex_flow(preview_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(preview_card_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(preview_card_, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
    lv_obj_set_style_border_width(preview_card_, 1, 0);
    lv_obj_set_style_border_opa(preview_card_, LV_OPA_COVER, 0);

    auto* preview_title = lv_label_create(preview_card_);
    lv_label_set_text(preview_title, "Preview");
    lv_obj_set_style_text_font(preview_title, field_font ? field_font : &lv_font_montserrat_12, 0);
    reactive::bind_theme(preview_title, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    preview_hint_label_ = lv_label_create(preview_card_);
    lv_label_set_text(preview_hint_label_, "Generate an image to see it here.");
    lv_obj_set_width(preview_hint_label_, LV_PCT(100));
    lv_label_set_long_mode(preview_hint_label_, LV_LABEL_LONG_WRAP);
    auto* hint_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(preview_hint_label_, hint_font ? hint_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(preview_hint_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    preview_image_ = lv_image_create(preview_card_);
    lv_obj_set_width(preview_image_, LV_PCT(100));
    lv_obj_set_height(preview_image_, 96);
    lv_image_set_inner_align(preview_image_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(panel);
    lv_label_set_text(status_label_, "Ready");
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    auto* status_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(status_label_, status_font ? status_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(status_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    refresh_prompt_display();
    refresh_focus_display();
    refresh_status_display();
}

bool AppleScreen::handle_key(uint32_t key, bool long_pressed) {
    if (long_pressed) {
        return false;
    }

    if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
        return generate_image();
    }

    if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL || key == 8u) {
        if (!prompt_buffer_.empty()) {
            prompt_buffer_.pop_back();
            refresh_prompt_display();
            action_status_.clear();
            refresh_status_display();
        }
        return true;
    }

    if (!is_printable_key(key)) {
        return false;
    }

    if (prompt_buffer_.size() < kMaxPromptLength) {
        prompt_buffer_.push_back(static_cast<char>(key));
        refresh_prompt_display();
        action_status_.clear();
        refresh_status_display();
    }
    return true;
}

void AppleScreen::refresh_prompt_display() {
    if (!prompt_textarea_) {
        return;
    }

    lv_textarea_set_text(prompt_textarea_, prompt_buffer_.c_str());
    lv_textarea_set_cursor_pos(prompt_textarea_, LV_TEXTAREA_CURSOR_LAST);
}

void AppleScreen::refresh_status_display() {
    if (!status_label_) {
        return;
    }

    std::string status = action_status_.empty() ? "Ready" : action_status_;
    status.append(" • Model: ");
    status.append(settings_.gemini_model);
    status.append(" • Personality: ");
    status.append(settings_.default_personality);
    lv_label_set_text(status_label_, status.c_str());
}

void AppleScreen::refresh_focus_display() {
    if (!prompt_textarea_) {
        return;
    }

    lv_obj_add_state(prompt_textarea_, LV_STATE_FOCUSED);
}

bool AppleScreen::generate_image() {
    if (settings_.gemini_api_key.empty()) {
        action_status_ = "Set the API key in settings first";
        refresh_status_display();
        return false;
    }

    const auto* preset = app::find_prompt_preset(settings_.default_personality);
    const auto& selected_preset = preset ? *preset : app::default_prompt_preset();
    const std::string prompt = app::build_prompt(selected_preset, prompt_buffer_);

    action_status_ = "Generating image...";
    refresh_status_display();

    const auto output_path = paths_.build_output_path("gemini-image", ".png");
    const auto result = app::generate_gemini_image_request(settings_.gemini_api_key,
                                                           settings_.gemini_model,
                                                           prompt,
                                                           output_path,
                                                           paths_.temp_dir());
    if (!result.ok) {
        action_status_ = result.message.empty() ? "Gemini image request failed" : result.message;
        refresh_status_display();
        LOG_WARN("Gemini image generation failed for {} / {}: {}",
                 settings_.gemini_model,
                 settings_.default_personality,
                 result.message);
        return false;
    }

    set_preview_image(result.image_path);
    action_status_ = "Saved image";
    refresh_status_display();
    LOG_INFO("Gemini image saved to {} using {} / {}",
             result.image_path.string(),
             settings_.gemini_model,
             settings_.default_personality);
    return true;
}

void AppleScreen::set_preview_image(const std::filesystem::path& image_path) {
    preview_path_ = image_path.string();
    preview_lvgl_path_ = to_lvgl_path(image_path);
    if (preview_image_) {
        lv_image_set_src(preview_image_, preview_lvgl_path_.c_str());
        lv_obj_clear_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_hint_label_) {
        lv_obj_add_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_card_) {
        lv_obj_scroll_to_view_recursive(preview_card_, LV_ANIM_OFF);
    }
}

std::string AppleScreen::to_lvgl_path(const std::filesystem::path& path) {
    return std::string("A:") + path.generic_string();
}

bool AppleScreen::is_printable_key(uint32_t key) {
    return key >= 32 && key <= 126;
}

} // namespace screen
