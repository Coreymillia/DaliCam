/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "butter_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "gemini_client.h"
#include "linux_input.h"
#include "logger.h"
#include "paths.h"
#include "prompt_presets.h"
#include "settings.h"
#include "theme.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

namespace screen {
namespace {

constexpr std::size_t kMaxApiKeyLength = 128;
constexpr std::string_view kModelDropdownOptions =
    "Nano Banana 2.1\n"
    "3.1 Flash Lite\n"
    "3 Pro Image";

std::string personality_dropdown_options() {
    std::string options;
    const auto& presets = app::prompt_presets();
    for (std::size_t index = 0; index < presets.size(); ++index) {
        if (index > 0) {
            options.push_back('\n');
        }
        options.append(presets[index].label);
    }
    return options;
}

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
      selected_model_index_(model_index_for(settings.gemini_model)),
      selected_personality_index_(personality_index_for(settings.default_personality)) {
    settings_.gemini_model = std::string(selected_model());
    settings_.default_personality = std::string(selected_personality_id());
    if (api_key_buffer_.empty()) {
        focus_ = FocusField::ApiKey;
    }
    init();
}

void ButterScreen::build_content(lv_obj_t* content) {
    root_panel_ = lv_obj_create(content);
    lv_obj_remove_style_all(root_panel_);
    lv_obj_set_size(root_panel_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(root_panel_, 12, 0);
    lv_obj_set_style_pad_right(root_panel_, 12, 0);
    lv_obj_set_style_pad_top(root_panel_, 8, 0);
    lv_obj_set_style_pad_bottom(root_panel_, 8, 0);
    lv_obj_set_style_pad_row(root_panel_, 6, 0);
    lv_obj_set_scrollbar_mode(root_panel_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(root_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(root_panel_, LV_DIR_VER);

    title_label_ = lv_label_create(root_panel_);
    lv_label_set_text(title_label_, "Gemini settings");
    auto* title_font = assets().load_standard_font(17, app::StandardFontWeight::Bold);
    lv_obj_set_style_text_font(title_label_, title_font ? title_font : &lv_font_montserrat_18, 0);
    reactive::bind_theme(title_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    subtitle_label_ = lv_label_create(root_panel_);
    lv_label_set_text(subtitle_label_,
                     "Up/Down switch fields. Left/Right change dropdowns. Enter saves settings. "
                     "Use the main screen prompt to generate images.");
    lv_obj_set_width(subtitle_label_, 296);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    auto* subtitle_font = assets().load_standard_font(11, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(subtitle_label_,
                               subtitle_font ? subtitle_font : &lv_font_montserrat_12,
                               0);
    reactive::bind_theme(subtitle_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    status_label_ = lv_label_create(root_panel_);
    lv_label_set_text(status_label_, "Ready");
    lv_obj_set_width(status_label_, 296);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    auto* status_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(status_label_, status_font ? status_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(status_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    const auto make_card = [&](const char* title, lv_obj_t*& card) {
        card = lv_obj_create(root_panel_);
        lv_obj_remove_style_all(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_row(card, 3, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        reactive::bind_theme(card, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);

        auto* label = lv_label_create(card);
        lv_label_set_text(label, title);
        auto* field_font = assets().load_standard_font(11, app::StandardFontWeight::Bold);
        lv_obj_set_style_text_font(label, field_font ? field_font : &lv_font_montserrat_12, 0);
        reactive::bind_theme(label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
        return card;
    };

    make_card("API key", api_key_card_);
    api_key_textarea_ = lv_textarea_create(api_key_card_);
    lv_obj_set_width(api_key_textarea_, LV_PCT(100));
    lv_obj_set_height(api_key_textarea_, 24);
    lv_textarea_set_one_line(api_key_textarea_, true);
    lv_textarea_set_max_length(api_key_textarea_, kMaxApiKeyLength);
    lv_textarea_set_placeholder_text(api_key_textarea_, "Paste Gemini key");
    lv_textarea_set_text(api_key_textarea_, api_key_buffer_.c_str());
    lv_textarea_set_cursor_pos(api_key_textarea_, LV_TEXTAREA_CURSOR_LAST);
    reactive::bind_theme(api_key_textarea_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    make_card("Personality", personality_card_);
    personality_dropdown_ = lv_dropdown_create(personality_card_);
    lv_obj_set_width(personality_dropdown_, LV_PCT(100));
    const auto personality_options = personality_dropdown_options();
    lv_dropdown_set_options(personality_dropdown_, personality_options.c_str());
    lv_dropdown_set_selected(personality_dropdown_, static_cast<uint16_t>(selected_personality_index_));
    lv_obj_add_event_cb(personality_dropdown_, personality_dropdown_changed_cb, LV_EVENT_VALUE_CHANGED, this);
    reactive::bind_theme(personality_dropdown_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    make_card("Model", model_card_);
    model_dropdown_ = lv_dropdown_create(model_card_);
    lv_obj_set_width(model_dropdown_, LV_PCT(100));
    lv_dropdown_set_options(model_dropdown_, kModelDropdownOptions.data());
    lv_dropdown_set_selected(model_dropdown_, static_cast<uint16_t>(selected_model_index_));
    lv_obj_add_event_cb(model_dropdown_, model_dropdown_changed_cb, LV_EVENT_VALUE_CHANGED, this);
    reactive::bind_theme(model_dropdown_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    make_card("Preview", preview_card_);
    preview_hint_label_ = lv_label_create(preview_card_);
    lv_label_set_text(preview_hint_label_, "Generate an image to show the latest preview here.");
    lv_obj_set_width(preview_hint_label_, LV_PCT(100));
    lv_label_set_long_mode(preview_hint_label_, LV_LABEL_LONG_WRAP);
    auto* hint_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(preview_hint_label_, hint_font ? hint_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(preview_hint_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    preview_image_ = lv_image_create(preview_card_);
    lv_obj_set_width(preview_image_, LV_PCT(100));
    lv_obj_set_height(preview_image_, 72);
    lv_image_set_inner_align(preview_image_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    refresh_api_key_display();
    refresh_personality_display();
    refresh_model_display();
    refresh_focus_display();
    refresh_status_display();
}

bool ButterScreen::handle_key(uint32_t key, bool long_pressed) {
    if (key == platform::kKeyPrintScreen) {
        return false;
    }

    if (key == LV_KEY_ESC) {
        view_model().show_apple_page();
        return true;
    }

    if (long_pressed) {
        return false;
    }

    if (key == LV_KEY_UP) {
        focus_previous();
        return true;
    }

    if (key == LV_KEY_DOWN) {
        focus_next();
        return true;
    }

    if (key == LV_KEY_LEFT) {
        if (focus_ == FocusField::Personality) {
            select_previous_personality();
            return true;
        }
        if (focus_ == FocusField::Model) {
            select_previous_model();
            return true;
        }
        return false;
    }

    if (key == LV_KEY_RIGHT) {
        if (focus_ == FocusField::Personality) {
            select_next_personality();
            return true;
        }
        if (focus_ == FocusField::Model) {
            select_next_model();
            return true;
        }
        return false;
    }

    if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
        return save_settings();
    }

    if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL || key == 8u) {
        bool handled = false;
        switch (focus_) {
            case FocusField::ApiKey:
                if (!api_key_buffer_.empty()) {
                    api_key_buffer_.pop_back();
                    dirty_ = true;
                    handled = true;
                    refresh_api_key_display();
                }
                break;
            case FocusField::Personality:
            case FocusField::Model:
                handled = true;
                break;
        }
        if (handled) {
            action_status_.clear();
            refresh_status_display();
        }
        return handled;
    }

    if (!is_printable_key(key)) {
        return false;
    }

    switch (focus_) {
        case FocusField::ApiKey:
            if (api_key_buffer_.size() < kMaxApiKeyLength) {
                api_key_buffer_.push_back(static_cast<char>(key));
                dirty_ = true;
                action_status_.clear();
                refresh_api_key_display();
                refresh_status_display();
            }
            return true;
        case FocusField::Personality:
        case FocusField::Model:
            return false;
    }

    return false;
}

void ButterScreen::refresh_api_key_display() {
    if (!api_key_textarea_) {
        return;
    }

    lv_textarea_set_text(api_key_textarea_, api_key_buffer_.c_str());
    lv_textarea_set_cursor_pos(api_key_textarea_, LV_TEXTAREA_CURSOR_LAST);
}

void ButterScreen::refresh_personality_display() {
    if (!personality_dropdown_) {
        return;
    }

    lv_dropdown_set_selected(personality_dropdown_, static_cast<uint16_t>(selected_personality_index_));
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

    std::string status = action_status_.empty() ? (dirty_ ? "Unsaved changes" : "Ready") : action_status_;
    status.append(" • Focus: ");
    switch (focus_) {
        case FocusField::ApiKey:
            status.append("API key");
            break;
        case FocusField::Personality:
            status.append("Personality");
            break;
        case FocusField::Model:
            status.append("Model");
            break;
    }
    lv_label_set_text(status_label_, status.c_str());
}

void ButterScreen::refresh_focus_display() {
    const auto colors = view::palette(view_model().is_dark_mode());

    const auto set_card_style = [&](lv_obj_t* card, bool active) {
        if (!card) {
            return;
        }
        lv_obj_set_style_border_width(card, active ? 2 : 1, 0);
        lv_obj_set_style_border_color(card, active ? colors.primary : colors.border, 0);
    };

    set_card_style(api_key_card_, focus_ == FocusField::ApiKey);
    set_card_style(personality_card_, focus_ == FocusField::Personality);
    set_card_style(model_card_, focus_ == FocusField::Model);
    set_card_style(preview_card_, false);

    auto* focus_widget = static_cast<lv_obj_t*>(nullptr);
    switch (focus_) {
        case FocusField::ApiKey:
            focus_widget = api_key_textarea_;
            break;
        case FocusField::Personality:
            focus_widget = personality_dropdown_;
            break;
        case FocusField::Model:
            focus_widget = model_dropdown_;
            break;
    }

    const std::array<lv_obj_t*, 3> widgets = {
        api_key_textarea_,
        personality_dropdown_,
        model_dropdown_,
    };
    for (auto* widget : widgets) {
        if (widget) {
            lv_obj_clear_state(widget, LV_STATE_FOCUSED);
        }
    }
    if (focus_widget) {
        lv_obj_add_state(focus_widget, LV_STATE_FOCUSED);
        lv_obj_scroll_to_view_recursive(focus_widget, LV_ANIM_OFF);
    }
}

void ButterScreen::set_focus(FocusField field) {
    focus_ = field;
    refresh_focus_display();
    refresh_status_display();
}

void ButterScreen::focus_previous() {
    switch (focus_) {
        case FocusField::ApiKey:
            set_focus(FocusField::Model);
            break;
        case FocusField::Personality:
            set_focus(FocusField::ApiKey);
            break;
        case FocusField::Model:
            set_focus(FocusField::Personality);
            break;
    }
}

void ButterScreen::focus_next() {
    switch (focus_) {
        case FocusField::ApiKey:
            set_focus(FocusField::Personality);
            break;
        case FocusField::Personality:
            set_focus(FocusField::Model);
            break;
        case FocusField::Model:
            set_focus(FocusField::ApiKey);
            break;
    }
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
    action_status_.clear();
    refresh_model_display();
    refresh_status_display();
}

void ButterScreen::select_next_model() {
    selected_model_index_ = (selected_model_index_ + 1) % app::Settings::kSupportedGeminiModels.size();
    settings_.gemini_model = std::string(selected_model());
    dirty_ = true;
    action_status_.clear();
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
    action_status_.clear();
    refresh_model_display();
    refresh_status_display();
}

void ButterScreen::select_previous_personality() {
    const auto& presets = app::prompt_presets();
    if (presets.empty()) {
        return;
    }
    if (selected_personality_index_ == 0) {
        selected_personality_index_ = presets.size() - 1;
    }
    else {
        --selected_personality_index_;
    }
    settings_.default_personality = std::string(selected_personality_id());
    dirty_ = true;
    action_status_.clear();
    refresh_personality_display();
    refresh_status_display();
}

void ButterScreen::select_next_personality() {
    const auto& presets = app::prompt_presets();
    if (presets.empty()) {
        return;
    }
    selected_personality_index_ = (selected_personality_index_ + 1) % presets.size();
    settings_.default_personality = std::string(selected_personality_id());
    dirty_ = true;
    action_status_.clear();
    refresh_personality_display();
    refresh_status_display();
}

void ButterScreen::select_personality_by_index(std::size_t index) {
    const auto& presets = app::prompt_presets();
    if (index >= presets.size()) {
        return;
    }

    selected_personality_index_ = index;
    settings_.default_personality = std::string(selected_personality_id());
    dirty_ = true;
    action_status_.clear();
    refresh_personality_display();
    refresh_status_display();
}

bool ButterScreen::save_settings() {
    settings_.gemini_api_key = api_key_buffer_;
    settings_.gemini_model = std::string(selected_model());
    settings_.default_personality = std::string(selected_personality_id());

    std::string error;
    if (!settings_.save(paths_.settings_file(), error)) {
        action_status_ = "Save failed: " + error;
        refresh_status_display();
        return false;
    }

    dirty_ = false;
    action_status_ = "Settings saved";
    refresh_status_display();
    return true;
}

bool ButterScreen::generate_image() {
    if (api_key_buffer_.empty()) {
        action_status_ = "Enter API key first";
        refresh_status_display();
        return false;
    }

    if (!save_settings()) {
        return false;
    }

    const std::string prompt = app::build_prompt(selected_prompt_preset(), std::string_view{});
    action_status_ = "Generating image...";
    refresh_status_display();

    const auto output_path = paths_.build_output_path("gemini-image", ".jpg");
    const auto result = app::generate_gemini_image_request(api_key_buffer_,
                                                           selected_model(),
                                                           prompt,
                                                           output_path,
                                                           paths_.temp_dir());

    if (!result.ok) {
        action_status_ = result.message.empty() ? "Gemini image request failed" : result.message;
        refresh_status_display();
        LOG_WARN("Gemini image generation failed for {} / {}: {}",
                 selected_model(),
                 selected_personality_id(),
                 result.message);
        return false;
    }

    set_preview_image(result.image_path);
    action_status_ = "Saved image";
    refresh_status_display();
    LOG_INFO("Gemini image saved to {} using {} / {}",
             result.image_path.string(),
             selected_model(),
             selected_personality_id());
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

std::size_t ButterScreen::personality_index_for(std::string_view personality_id) {
    const auto& presets = app::prompt_presets();
    const auto it = std::find_if(presets.begin(), presets.end(), [&](const auto& preset) {
        return preset.id == personality_id;
    });
    if (it == presets.end()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(presets.begin(), it));
}

std::string_view ButterScreen::selected_model() const {
    const auto& models = app::Settings::kSupportedGeminiModels;
    if (selected_model_index_ >= models.size()) {
        return app::Settings::kDefaultGeminiModel;
    }
    return models[selected_model_index_];
}

std::string_view ButterScreen::selected_personality_id() const {
    const auto& presets = app::prompt_presets();
    if (selected_personality_index_ >= presets.size()) {
        return app::default_prompt_preset().id;
    }
    return presets[selected_personality_index_].id;
}

const app::PromptPreset& ButterScreen::selected_prompt_preset() const {
    const auto& presets = app::prompt_presets();
    if (selected_personality_index_ >= presets.size()) {
        return app::default_prompt_preset();
    }
    return presets[selected_personality_index_];
}

void ButterScreen::set_preview_image(const std::filesystem::path& image_path) {
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

std::string ButterScreen::to_lvgl_path(const std::filesystem::path& path) {
    return std::string("A:") + path.generic_string();
}

void ButterScreen::model_dropdown_changed_cb(lv_event_t* event) {
    auto* screen = static_cast<ButterScreen*>(lv_event_get_user_data(event));
    if (!screen || !screen->model_dropdown_) {
        return;
    }

    const auto selected = lv_dropdown_get_selected(screen->model_dropdown_);
    screen->set_focus(FocusField::Model);
    screen->select_model_by_index(selected);
}

void ButterScreen::personality_dropdown_changed_cb(lv_event_t* event) {
    auto* screen = static_cast<ButterScreen*>(lv_event_get_user_data(event));
    if (!screen || !screen->personality_dropdown_) {
        return;
    }

    const auto selected = lv_dropdown_get_selected(screen->personality_dropdown_);
    screen->set_focus(FocusField::Personality);
    screen->select_personality_by_index(selected);
}

} // namespace screen
