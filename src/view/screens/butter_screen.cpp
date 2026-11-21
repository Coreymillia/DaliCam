/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "butter_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "paths.h"
#include "settings.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace screen {
namespace {

constexpr std::size_t kMaxApiKeyLength = 128;
constexpr std::string_view kModelDropdownOptions =
    "Nano Banana 2.1\n"
    "3.1 Flash Lite\n"
    "3 Pro Image";

bool is_printable_key(uint32_t key) {
    return key >= 32 && key <= 126;
}

} // namespace

ButterScreen::ButterScreen(viewmodel::BaseViewModel& view_model,
                           app::AssetManager& assets,
                           app::Settings& settings,
                           const app::Paths& paths)
    : BaseScreen(view_model, assets),
      settings_(settings),
      paths_(paths),
      api_key_buffer_(settings.gemini_api_key),
      selected_model_index_(model_index_for(settings.gemini_model)) {
    settings_.gemini_model = std::string(selected_model());
    init();
}

void ButterScreen::build_content(lv_obj_t* content) {
    auto* panel = lv_obj_create(content);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(panel, 12, 0);
    lv_obj_set_style_pad_right(panel, 12, 0);
    lv_obj_set_style_pad_top(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    title_label_ = lv_label_create(panel);
    lv_label_set_text(title_label_, "Gemini settings");
    auto* title_font = assets().load_standard_font(17, app::StandardFontWeight::Bold);
    lv_obj_set_style_text_font(title_label_, title_font ? title_font : &lv_font_montserrat_18, 0);
    reactive::bind_theme(title_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    subtitle_label_ = lv_label_create(panel);
    lv_label_set_text(subtitle_label_, "Enter your API key and choose the image model.");
    lv_obj_set_width(subtitle_label_, 296);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_CLIP);
    auto* subtitle_font = assets().load_standard_font(11, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(subtitle_label_,
                               subtitle_font ? subtitle_font : &lv_font_montserrat_12,
                               0);
    reactive::bind_theme(subtitle_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    auto* controls_row = lv_obj_create(panel);
    lv_obj_remove_style_all(controls_row);
    lv_obj_set_size(controls_row, LV_PCT(100), 56);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(controls_row, 6, 0);
    lv_obj_clear_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);

    auto* api_card = lv_obj_create(controls_row);
    lv_obj_remove_style_all(api_card);
    lv_obj_set_size(api_card, 148, 56);
    lv_obj_set_style_radius(api_card, 8, 0);
    lv_obj_set_style_pad_all(api_card, 8, 0);
    lv_obj_set_style_pad_row(api_card, 3, 0);
    lv_obj_set_flex_flow(api_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(api_card, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(api_card, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);

    auto* api_label = lv_label_create(api_card);
    lv_label_set_text(api_label, "API key");
    auto* field_font = assets().load_standard_font(11, app::StandardFontWeight::Bold);
    lv_obj_set_style_text_font(api_label, field_font ? field_font : &lv_font_montserrat_12, 0);
    reactive::bind_theme(api_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    api_key_textarea_ = lv_textarea_create(api_card);
    lv_obj_set_width(api_key_textarea_, 132);
    lv_obj_set_height(api_key_textarea_, 24);
    lv_textarea_set_one_line(api_key_textarea_, true);
    lv_textarea_set_max_length(api_key_textarea_, kMaxApiKeyLength);
    lv_textarea_set_placeholder_text(api_key_textarea_, "Paste Gemini key");
    lv_textarea_set_text(api_key_textarea_, api_key_buffer_.c_str());
    lv_textarea_set_cursor_pos(api_key_textarea_, LV_TEXTAREA_CURSOR_LAST);
    reactive::bind_theme(api_key_textarea_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    auto* model_card = lv_obj_create(controls_row);
    lv_obj_remove_style_all(model_card);
    lv_obj_set_size(model_card, 148, 56);
    lv_obj_set_style_radius(model_card, 8, 0);
    lv_obj_set_style_pad_all(model_card, 8, 0);
    lv_obj_set_style_pad_row(model_card, 3, 0);
    lv_obj_set_flex_flow(model_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(model_card, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(model_card, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);

    auto* model_label = lv_label_create(model_card);
    lv_label_set_text(model_label, "Image model");
    lv_obj_set_style_text_font(model_label, field_font ? field_font : &lv_font_montserrat_12, 0);
    reactive::bind_theme(model_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    model_dropdown_ = lv_dropdown_create(model_card);
    lv_obj_set_width(model_dropdown_, 132);
    lv_dropdown_set_options(model_dropdown_, kModelDropdownOptions.data());
    lv_dropdown_set_selected(model_dropdown_, static_cast<uint16_t>(selected_model_index_));
    lv_obj_add_event_cb(model_dropdown_, model_dropdown_changed_cb, LV_EVENT_VALUE_CHANGED, this);
    reactive::bind_theme(model_dropdown_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    status_label_ = lv_label_create(panel);
    lv_label_set_text(status_label_, "Press Enter to save. Backspace deletes.");
    lv_obj_set_width(status_label_, 296);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    auto* status_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(status_label_, status_font ? status_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(status_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    refresh_api_key_display();
    refresh_model_display();
    refresh_status_display();
}

bool ButterScreen::handle_key(uint32_t key, bool long_pressed) {
    if (long_pressed) {
        return false;
    }

    if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
        return save_settings();
    }

    if (key == LV_KEY_LEFT) {
        select_previous_model();
        return true;
    }

    if (key == LV_KEY_RIGHT) {
        select_next_model();
        return true;
    }

    if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL || key == 8u) {
        if (!api_key_buffer_.empty()) {
            api_key_buffer_.pop_back();
            dirty_ = true;
            refresh_api_key_display();
            refresh_status_display();
        }
        return true;
    }

    if (!is_printable_key(key)) {
        return false;
    }

    if (api_key_buffer_.size() < kMaxApiKeyLength) {
        api_key_buffer_.push_back(static_cast<char>(key));
        dirty_ = true;
        refresh_api_key_display();
        refresh_status_display();
    }
    return true;
}

void ButterScreen::refresh_api_key_display() {
    if (!api_key_textarea_) {
        return;
    }

    lv_textarea_set_text(api_key_textarea_, api_key_buffer_.c_str());
    lv_textarea_set_cursor_pos(api_key_textarea_, LV_TEXTAREA_CURSOR_LAST);
}

void ButterScreen::refresh_model_display() {
    if (!model_dropdown_) {
        return;
    }

    lv_dropdown_set_selected(model_dropdown_, static_cast<uint16_t>(selected_model_index_));
}

void ButterScreen::refresh_status_display() {
    if (!status_label_) {
        return;
    }

    std::string status = dirty_ ? "Unsaved changes" : "Saved";
    status.append(" • ");
    status.append("Model: ");
    status.append(selected_model());
    lv_label_set_text(status_label_, status.c_str());
}

void ButterScreen::select_previous_model() {
    if (selected_model_index_ == 0) {
        selected_model_index_ = app::Settings::kSupportedGeminiModels.size() - 1;
    }
    else {
        --selected_model_index_;
    }
    settings_.gemini_model = std::string(selected_model());
    dirty_ = true;
    refresh_model_display();
    refresh_status_display();
}

void ButterScreen::select_next_model() {
    selected_model_index_ = (selected_model_index_ + 1) % app::Settings::kSupportedGeminiModels.size();
    settings_.gemini_model = std::string(selected_model());
    dirty_ = true;
    refresh_model_display();
    refresh_status_display();
}

void ButterScreen::select_model_by_index(std::size_t index) {
    if (index >= app::Settings::kSupportedGeminiModels.size()) {
        return;
    }

    selected_model_index_ = index;
    settings_.gemini_model = std::string(selected_model());
    dirty_ = true;
    refresh_status_display();
}

bool ButterScreen::save_settings() {
    settings_.gemini_api_key = api_key_buffer_;
    settings_.gemini_model = std::string(selected_model());

    std::string error;
    if (!settings_.save(paths_.settings_file(), error)) {
        if (status_label_) {
            lv_label_set_text_fmt(status_label_, "Save failed: %s", error.c_str());
        }
        return false;
    }

    dirty_ = false;
    refresh_status_display();
    return true;
}

std::size_t ButterScreen::model_index_for(std::string_view model) {
    const auto& models = app::Settings::kSupportedGeminiModels;
    const auto it = std::find(models.begin(), models.end(), model);
    if (it == models.end()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(models.begin(), it));
}

std::string_view ButterScreen::selected_model() const {
    const auto& models = app::Settings::kSupportedGeminiModels;
    if (selected_model_index_ >= models.size()) {
        return app::Settings::kDefaultGeminiModel;
    }
    return models[selected_model_index_];
}

void ButterScreen::model_dropdown_changed_cb(lv_event_t* event) {
    auto* screen = static_cast<ButterScreen*>(lv_event_get_user_data(event));
    if (!screen || !screen->model_dropdown_) {
        return;
    }

    const auto selected = lv_dropdown_get_selected(screen->model_dropdown_);
    screen->select_model_by_index(selected);
}

} // namespace screen
