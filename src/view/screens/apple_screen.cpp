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
#include "camera_capture.h"
#include "prompt_presets.h"
#include "settings.h"
#include "theme.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <sys/wait.h>
#include <vector>

namespace screen {
namespace {

constexpr std::size_t kMaxPromptLength = 512;
constexpr int kPreviewCanvasWidth = 300;
constexpr int kPreviewCanvasHeight = 104;
constexpr std::size_t kPreviewCanvasBufferSize = static_cast<std::size_t>(kPreviewCanvasWidth) *
                                                 static_cast<std::size_t>(kPreviewCanvasHeight) * 4U;

bool is_printable_key(uint32_t key) {
    return key >= 32 && key <= 126;
}

bool is_yes_key(uint32_t key) {
    return key == 'y' || key == 'Y';
}

bool is_no_key(uint32_t key) {
    return key == 'n' || key == 'N';
}

std::string shell_quote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char character : value) {
        if (character == '\'') {
            quoted.append("'\\''");
        }
        else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

int system_exit_code(const std::string& command) {
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
#if defined(_WIN32)
    return status;
#else
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
#endif
}

bool create_canvas_preview(const std::filesystem::path& input,
                           const std::filesystem::path& output,
                           int max_width,
                           int max_height,
                           std::string& error) {
    error.clear();

    std::error_code filesystem_error;
    std::filesystem::create_directories(output.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "Unable to create preview directory: " + filesystem_error.message();
        return false;
    }

    const auto command =
        "python3 -c "
        "'from PIL import Image, ImageOps; import sys; "
        "src=sys.argv[1]; dst=sys.argv[2]; max_w=int(sys.argv[3]); max_h=int(sys.argv[4]); "
        "img=Image.open(src); img=ImageOps.exif_transpose(img).convert(\"RGBA\"); "
        "resample=Image.Resampling.LANCZOS if hasattr(Image, \"Resampling\") else Image.LANCZOS; "
        "img.thumbnail((max_w, max_h), resample); "
        "canvas=Image.new(\"RGBA\", (max_w, max_h), (0, 0, 0, 255)); "
        "canvas.paste(img, ((max_w - img.width) // 2, (max_h - img.height) // 2)); "
        "r, g, b, a = canvas.split(); "
        "bgra = Image.merge(\"RGBA\", (b, g, r, a)); "
        "open(dst, \"wb\").write(bgra.tobytes())' "
        + shell_quote(input.string()) +
        " " + shell_quote(output.string()) +
        " " + std::to_string(max_width) +
        " " + std::to_string(max_height);

    const int exit_code = system_exit_code(command);
    if (exit_code != 0) {
        error = "Thumbnail generation failed with code " + std::to_string(exit_code);
        return false;
    }

    if (!std::filesystem::exists(output, filesystem_error) || filesystem_error) {
        error = "Thumbnail output was not created";
        return false;
    }

    return true;
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
    refresh_latest_preview_from_storage();
    preview_refresh_timer_ = lv_timer_create(preview_refresh_timer_cb, 2000, this);
}

AppleScreen::~AppleScreen() {
    if (preview_refresh_timer_) {
        lv_timer_delete(preview_refresh_timer_);
        preview_refresh_timer_ = nullptr;
    }
}

void AppleScreen::build_content(lv_obj_t* content) {
    auto* panel = lv_obj_create(content);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(panel, 10, 0);
    lv_obj_set_style_pad_right(panel, 10, 0);
    lv_obj_set_style_pad_top(panel, 8, 0);
    lv_obj_set_style_pad_bottom(panel, 8, 0);

    preview_card_ = lv_obj_create(panel);
    lv_obj_remove_style_all(preview_card_);
    lv_obj_set_width(preview_card_, LV_PCT(100));
    lv_obj_set_height(preview_card_, kPreviewCanvasHeight);
    lv_obj_clear_flag(preview_card_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(preview_card_, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
    lv_obj_set_style_radius(preview_card_, 8, 0);
    lv_obj_set_style_pad_all(preview_card_, 0, 0);
    lv_obj_set_style_border_width(preview_card_, 1, 0);
    lv_obj_set_style_border_opa(preview_card_, LV_OPA_COVER, 0);

    preview_hint_label_ = lv_label_create(preview_card_);
    lv_label_set_text(preview_hint_label_, "Latest photo will appear here.");
    lv_obj_set_width(preview_hint_label_, LV_PCT(100));
    lv_label_set_long_mode(preview_hint_label_, LV_LABEL_LONG_WRAP);
    auto* hint_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(preview_hint_label_, hint_font ? hint_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(preview_hint_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(preview_hint_label_);

    preview_canvas_buffer_.assign(kPreviewCanvasBufferSize, 0);
    preview_image_ = lv_canvas_create(preview_card_);
    lv_obj_set_size(preview_image_, kPreviewCanvasWidth, kPreviewCanvasHeight);
    lv_canvas_set_buffer(preview_image_,
                         preview_canvas_buffer_.data(),
                         kPreviewCanvasWidth,
                         kPreviewCanvasHeight,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(preview_image_, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    confirm_label_ = lv_label_create(panel);
    lv_label_set_text(confirm_label_, "Confirm? Y / N");
    lv_obj_add_flag(confirm_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(confirm_label_, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_set_width(confirm_label_, 100);
    lv_label_set_long_mode(confirm_label_, LV_LABEL_LONG_WRAP);
    auto* confirm_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(confirm_label_, confirm_font ? confirm_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(confirm_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_add_flag(confirm_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(panel);
    lv_label_set_text(status_label_, "Ready");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_width(status_label_, 120);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    auto* status_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(status_label_, status_font ? status_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(status_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    settings_button_ = lv_button_create(panel);
    lv_obj_remove_style_all(settings_button_);
    lv_obj_add_flag(settings_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(settings_button_, 88, 24);
    lv_obj_align(settings_button_, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_radius(settings_button_, 12, 0);
    lv_obj_set_style_pad_hor(settings_button_, 8, 0);
    lv_obj_set_style_pad_ver(settings_button_, 4, 0);
    lv_obj_set_style_border_width(settings_button_, 1, 0);
    lv_obj_set_style_border_opa(settings_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(settings_button_, LV_OPA_70, 0);
    lv_obj_add_event_cb(settings_button_, settings_button_cb, LV_EVENT_CLICKED, this);
    auto* settings_font = assets().load_standard_font(10, app::StandardFontWeight::Bold);
    auto* settings_label = lv_label_create(settings_button_);
    lv_label_set_text(settings_label, "⚙ Settings");
    lv_obj_set_style_text_font(settings_label, settings_font ? settings_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(settings_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(settings_label);
    reactive::bind_theme(settings_button_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    capture_button_ = lv_button_create(panel);
    lv_obj_remove_style_all(capture_button_);
    lv_obj_add_flag(capture_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(capture_button_, 88, 24);
    lv_obj_align(capture_button_, LV_ALIGN_TOP_RIGHT, -6, 36);
    lv_obj_set_style_radius(capture_button_, 12, 0);
    lv_obj_set_style_pad_hor(capture_button_, 8, 0);
    lv_obj_set_style_pad_ver(capture_button_, 4, 0);
    lv_obj_set_style_border_width(capture_button_, 1, 0);
    lv_obj_set_style_border_opa(capture_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(capture_button_, LV_OPA_70, 0);
    lv_obj_add_event_cb(capture_button_, capture_button_cb, LV_EVENT_CLICKED, this);
    auto* capture_font = assets().load_standard_font(10, app::StandardFontWeight::Bold);
    auto* capture_label = lv_label_create(capture_button_);
    lv_label_set_text(capture_label, "Capture");
    lv_obj_set_style_text_font(capture_label, capture_font ? capture_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(capture_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(capture_label);
    reactive::bind_theme(capture_button_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    prompt_button_ = lv_button_create(panel);
    lv_obj_remove_style_all(prompt_button_);
    lv_obj_add_flag(prompt_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(prompt_button_, 88, 24);
    lv_obj_align(prompt_button_, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_radius(prompt_button_, 12, 0);
    lv_obj_set_style_pad_hor(prompt_button_, 8, 0);
    lv_obj_set_style_pad_ver(prompt_button_, 4, 0);
    lv_obj_set_style_border_width(prompt_button_, 1, 0);
    lv_obj_set_style_border_opa(prompt_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(prompt_button_, LV_OPA_70, 0);
    lv_obj_add_event_cb(prompt_button_, prompt_button_cb, LV_EVENT_CLICKED, this);
    auto* prompt_font = assets().load_standard_font(10, app::StandardFontWeight::Bold);
    auto* prompt_label_widget = lv_label_create(prompt_button_);
    lv_label_set_text(prompt_label_widget, "← Prompt");
    lv_obj_set_style_text_font(prompt_label_widget, prompt_font ? prompt_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(prompt_label_widget, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(prompt_label_widget);
    reactive::bind_theme(prompt_button_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    prompt_card_ = lv_obj_create(panel);
    lv_obj_remove_style_all(prompt_card_);
    lv_obj_add_flag(prompt_card_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(prompt_card_, LV_PCT(100));
    lv_obj_set_height(prompt_card_, 52);
    lv_obj_align(prompt_card_, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(prompt_card_, 8, 0);
    lv_obj_set_style_pad_all(prompt_card_, 8, 0);
    lv_obj_clear_flag(prompt_card_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(prompt_card_, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
    lv_obj_set_style_border_width(prompt_card_, 1, 0);
    lv_obj_set_style_border_opa(prompt_card_, LV_OPA_COVER, 0);

    prompt_textarea_ = lv_textarea_create(prompt_card_);
    lv_obj_set_size(prompt_textarea_, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_one_line(prompt_textarea_, true);
    lv_textarea_set_max_length(prompt_textarea_, kMaxPromptLength);
    lv_textarea_set_placeholder_text(prompt_textarea_, "Describe the image to generate");
    lv_textarea_set_text(prompt_textarea_, prompt_buffer_.c_str());
    lv_textarea_set_cursor_pos(prompt_textarea_, LV_TEXTAREA_CURSOR_LAST);
    reactive::bind_theme(prompt_textarea_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    refresh_prompt_display();
    show_prompt_mode();
}

bool AppleScreen::handle_key(uint32_t key, bool long_pressed) {
    if (long_pressed) {
        return false;
    }

    if (mode_ == ScreenMode::CaptureConfirm) {
        if (is_yes_key(key)) {
            if (pending_capture_path_.empty()) {
                action_status_ = "No captured photo available";
                refresh_status_display();
                show_prompt_mode();
                return true;
            }
            return generate_image(pending_capture_path_);
        }

        if (is_no_key(key) || key == LV_KEY_ESC) {
            if (!pending_capture_path_.empty()) {
                std::error_code delete_error;
                std::filesystem::remove(pending_capture_path_, delete_error);
                if (delete_error) {
                    LOG_WARN("failed to delete cancelled capture {}: {}",
                             pending_capture_path_.string(),
                             delete_error.message());
                }
                pending_capture_path_.clear();
            }
            action_status_.clear();
            refresh_status_display();
            show_prompt_mode();
            return true;
        }

        return true;
    }

    if (mode_ == ScreenMode::Preview) {
        if (key == LV_KEY_ESC) {
            clear_prompt();
            show_prompt_mode();
            return true;
        }

        if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
            clear_prompt();
            show_prompt_mode();
            return true;
        }

        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL || key == 8u || is_printable_key(key)) {
            clear_prompt();
            show_prompt_mode();
            if (is_printable_key(key)) {
                return handle_key(key, false);
            }
            return true;
        }

        return false;
    }

    if (focus_ == FocusTarget::Settings) {
        if (key == LV_KEY_ESC) {
            focus_ = FocusTarget::Prompt;
            refresh_focus_display();
            return true;
        }

        if (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            focus_ = FocusTarget::Capture;
            refresh_focus_display();
            return true;
        }

        if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
            open_settings();
            return true;
        }

        return false;
    }

    if (focus_ == FocusTarget::Capture) {
        if (key == LV_KEY_ESC) {
            focus_ = FocusTarget::Prompt;
            refresh_focus_display();
            return true;
        }

        if (key == LV_KEY_LEFT || key == LV_KEY_DOWN) {
            focus_ = FocusTarget::Prompt;
            refresh_focus_display();
            return true;
        }

        if (key == LV_KEY_RIGHT || key == LV_KEY_UP) {
            focus_ = FocusTarget::Settings;
            refresh_focus_display();
            return true;
        }

        if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
            return capture_photo();
        }

        if (is_printable_key(key)) {
            focus_ = FocusTarget::Prompt;
            refresh_focus_display();
        }
        else {
            return false;
        }
    }

    if (focus_ == FocusTarget::Prompt) {
        if (key == LV_KEY_UP || key == LV_KEY_RIGHT) {
            focus_ = FocusTarget::Capture;
            refresh_focus_display();
            return true;
        }
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

    if (focus_ != FocusTarget::Prompt) {
        focus_ = FocusTarget::Prompt;
        refresh_focus_display();
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

    if (mode_ != ScreenMode::Prompt) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    std::string status = action_status_.empty() ? "Ready" : action_status_;
    status.append(" • Model: ");
    status.append(settings_.gemini_model);
    status.append(" • Personality: ");
    status.append(settings_.default_personality);
    lv_label_set_text(status_label_, status.c_str());
}

void AppleScreen::refresh_focus_display() {
    if (mode_ == ScreenMode::Preview) {
        if (prompt_textarea_) {
            lv_obj_remove_state(prompt_textarea_, LV_STATE_FOCUSED);
        }
        if (settings_button_) {
            lv_obj_remove_state(settings_button_, LV_STATE_FOCUSED);
        }
        if (prompt_button_) {
            lv_obj_remove_state(prompt_button_, LV_STATE_FOCUSED);
        }
        if (prompt_card_) {
            lv_obj_set_style_border_width(prompt_card_, 1, 0);
            lv_obj_set_style_border_color(prompt_card_, view::palette(view_model().is_dark_mode()).border, 0);
        }
        if (settings_button_) {
            lv_obj_set_style_border_width(settings_button_, 1, 0);
            lv_obj_set_style_border_color(settings_button_, view::palette(view_model().is_dark_mode()).border, 0);
        }
        if (prompt_button_) {
            lv_obj_set_style_border_width(prompt_button_, 1, 0);
            lv_obj_set_style_border_color(prompt_button_, view::palette(view_model().is_dark_mode()).border, 0);
        }
        return;
    }

    if (prompt_textarea_) {
        if (focus_ == FocusTarget::Prompt) {
            lv_obj_add_state(prompt_textarea_, LV_STATE_FOCUSED);
        }
        else {
            lv_obj_remove_state(prompt_textarea_, LV_STATE_FOCUSED);
        }
    }

    if (settings_button_) {
        if (focus_ == FocusTarget::Settings) {
            lv_obj_add_state(settings_button_, LV_STATE_FOCUSED);
        }
        else {
            lv_obj_remove_state(settings_button_, LV_STATE_FOCUSED);
        }
    }

    if (capture_button_) {
        if (focus_ == FocusTarget::Capture) {
            lv_obj_add_state(capture_button_, LV_STATE_FOCUSED);
        }
        else {
            lv_obj_remove_state(capture_button_, LV_STATE_FOCUSED);
        }
    }

    const auto palette = view::palette(view_model().is_dark_mode());
    if (prompt_card_) {
        lv_obj_set_style_border_width(prompt_card_, focus_ == FocusTarget::Prompt ? 2 : 1, 0);
        lv_obj_set_style_border_color(prompt_card_, focus_ == FocusTarget::Prompt ? palette.primary : palette.border, 0);
    }
    if (settings_button_) {
        lv_obj_set_style_border_width(settings_button_, focus_ == FocusTarget::Settings ? 2 : 1, 0);
        lv_obj_set_style_border_color(settings_button_, focus_ == FocusTarget::Settings ? palette.primary : palette.border, 0);
    }
    if (capture_button_) {
        lv_obj_set_style_border_width(capture_button_, focus_ == FocusTarget::Capture ? 2 : 1, 0);
        lv_obj_set_style_border_color(capture_button_, focus_ == FocusTarget::Capture ? palette.primary : palette.border, 0);
    }
}

void AppleScreen::clear_prompt() {
    prompt_buffer_.clear();
    refresh_prompt_display();
    action_status_.clear();
    refresh_status_display();
}

void AppleScreen::show_prompt_mode() {
    mode_ = ScreenMode::Prompt;
    refresh_latest_preview_from_storage();
    if (preview_card_) {
        lv_obj_set_size(preview_card_, LV_PCT(100), kPreviewCanvasHeight);
        lv_obj_remove_flag(preview_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_) {
        if (preview_path_.empty()) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (preview_hint_label_) {
        if (preview_path_.empty()) {
            lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (prompt_card_) {
        lv_obj_remove_flag(prompt_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_button_) {
        lv_obj_remove_flag(settings_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (capture_button_) {
        lv_obj_remove_flag(capture_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_label_) {
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (confirm_label_) {
        lv_obj_add_flag(confirm_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (prompt_button_) {
        lv_obj_add_flag(prompt_button_, LV_OBJ_FLAG_HIDDEN);
    }
    focus_ = FocusTarget::Prompt;
    refresh_focus_display();
    refresh_status_display();
}

void AppleScreen::show_preview_mode() {
    mode_ = ScreenMode::Preview;
    if (preview_card_) {
        lv_obj_set_size(preview_card_, LV_PCT(100), kPreviewCanvasHeight);
        lv_obj_clear_flag(preview_card_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(preview_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (prompt_card_) {
        lv_obj_add_flag(prompt_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_button_) {
        lv_obj_add_flag(settings_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (capture_button_) {
        lv_obj_add_flag(capture_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_label_) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (confirm_label_) {
        lv_obj_add_flag(confirm_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (prompt_button_) {
        lv_obj_remove_flag(prompt_button_, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_focus_display();
    refresh_status_display();
}

void AppleScreen::show_capture_confirm_mode() {
    mode_ = ScreenMode::CaptureConfirm;
    if (preview_card_) {
        lv_obj_set_size(preview_card_, LV_PCT(100), kPreviewCanvasHeight);
        lv_obj_clear_flag(preview_card_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(preview_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (prompt_card_) {
        lv_obj_add_flag(prompt_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (settings_button_) {
        lv_obj_add_flag(settings_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (capture_button_) {
        lv_obj_add_flag(capture_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_label_) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (prompt_button_) {
        lv_obj_add_flag(prompt_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (confirm_label_) {
        lv_obj_remove_flag(confirm_label_, LV_OBJ_FLAG_HIDDEN);
    }
    focus_ = FocusTarget::Prompt;
    refresh_focus_display();
    refresh_status_display();
}

bool AppleScreen::generate_image(const std::optional<std::filesystem::path>& source_image_path) {
    if (busy_) {
        return false;
    }

    if (settings_.gemini_api_key.empty()) {
        action_status_ = "Set the API key in settings first";
        refresh_status_display();
        return false;
    }

    busy_ = true;
    const auto* preset = app::find_prompt_preset(settings_.default_personality);
    const auto& selected_preset = preset ? *preset : app::default_prompt_preset();
    const std::string prompt = app::build_prompt(selected_preset, prompt_buffer_);

    action_status_ = source_image_path ? "Generating from photo..." : "Generating image...";
    refresh_status_display();

    const auto output_path = paths_.build_output_path("gemini-image", ".jpg");
    const auto result = app::generate_gemini_image_request(settings_.gemini_api_key,
                                                           settings_.gemini_model,
                                                           prompt,
                                                           output_path,
                                                           paths_.temp_dir(),
                                                           source_image_path);
    busy_ = false;
    if (!result.ok) {
        action_status_ = result.message.empty() ? "Gemini image request failed" : result.message;
        refresh_status_display();
        show_prompt_mode();
        LOG_WARN("Gemini image generation failed for {} / {}: {}",
                 settings_.gemini_model,
                 settings_.default_personality,
                 result.message);
        return false;
    }

    set_preview_image(result.image_path);
    action_status_ = "Saved image";
    show_preview_mode();
    LOG_INFO("Gemini image saved to {} using {} / {}",
             result.image_path.string(),
             settings_.gemini_model,
             settings_.default_personality);
    return true;
}

bool AppleScreen::capture_photo() {
    if (busy_) {
        return false;
    }

    busy_ = true;
    action_status_ = "Capturing photo...";
    refresh_status_display();

    const auto capture_path = paths_.build_capture_path("capture", ".jpg");
    const auto result = platform::camera::capture_photo(capture_path);
    busy_ = false;
    if (!result.success) {
        action_status_ = result.error.empty() ? "Photo capture failed" : result.error;
        refresh_status_display();
        show_prompt_mode();
        LOG_WARN("Camera capture failed: {}", result.error);
        return false;
    }

    pending_capture_path_ = result.path;
    set_preview_image(capture_path);
    action_status_.clear();
    show_capture_confirm_mode();
    LOG_INFO("Camera photo captured to {}", result.path);
    return true;
}

void AppleScreen::open_settings() {
    view_model().show_butter_page();
}

void AppleScreen::settings_button_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }
    screen->open_settings();
}

void AppleScreen::capture_button_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }
    screen->capture_photo();
}

void AppleScreen::prompt_button_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }
    screen->clear_prompt();
    screen->show_prompt_mode();
}

void AppleScreen::preview_refresh_timer_cb(lv_timer_t* timer) {
    auto* screen = static_cast<AppleScreen*>(lv_timer_get_user_data(timer));
    if (!screen) {
        return;
    }
    screen->refresh_latest_preview_from_storage();
}

void AppleScreen::refresh_latest_preview_from_storage() {
    if (mode_ != ScreenMode::Prompt) {
        return;
    }

    std::optional<std::filesystem::path> latest_path;
    std::filesystem::file_time_type latest_time{};
    bool have_latest = false;

    const auto consider_directory = [&](const std::filesystem::path& directory) {
        std::error_code directory_error;
        if (!std::filesystem::exists(directory, directory_error) || directory_error) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory, directory_error)) {
            if (directory_error) {
                return;
            }
            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error) || entry_error) {
                continue;
            }

            const auto extension = entry.path().extension().string();
            if (extension != ".jpg" && extension != ".jpeg" && extension != ".png") {
                continue;
            }

            const auto write_time = entry.last_write_time(entry_error);
            if (entry_error) {
                continue;
            }

            if (!have_latest || write_time > latest_time) {
                latest_time = write_time;
                latest_path = entry.path();
                have_latest = true;
            }
        }
    };

    consider_directory(paths_.camera_dir());
    consider_directory(paths_.generated_dir());
    consider_directory(paths_.uploads_dir());

    if (latest_path && latest_path->string() != preview_path_) {
        set_preview_image(*latest_path);
        return;
    }

    if (!latest_path && preview_image_ && preview_hint_label_) {
        preview_path_.clear();
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppleScreen::set_preview_image(const std::filesystem::path& image_path) {
    preview_path_ = image_path.string();

    const auto preview_file = paths_.temp_dir() / "latest-preview.raw";
    std::string preview_error;
    if (!create_canvas_preview(image_path, preview_file, kPreviewCanvasWidth, kPreviewCanvasHeight, preview_error)) {
        LOG_WARN("preview generation failed for {}: {}", image_path.string(), preview_error);
        preview_path_.clear();
        if (preview_image_) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_hint_label_) {
            lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    preview_canvas_buffer_.assign(kPreviewCanvasBufferSize, 0);
    std::ifstream raw_file(preview_file, std::ios::binary);
    if (!raw_file) {
        LOG_WARN("unable to open preview buffer {}", preview_file.string());
        preview_path_.clear();
        if (preview_image_) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_hint_label_) {
            lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    raw_file.read(reinterpret_cast<char*>(preview_canvas_buffer_.data()),
                  static_cast<std::streamsize>(preview_canvas_buffer_.size()));
    if (raw_file.gcount() != static_cast<std::streamsize>(preview_canvas_buffer_.size())) {
        LOG_WARN("preview buffer size mismatch for {}", preview_file.string());
        preview_path_.clear();
        if (preview_image_) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_hint_label_) {
            lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (preview_image_) {
        lv_canvas_set_buffer(preview_image_,
                             preview_canvas_buffer_.data(),
                             kPreviewCanvasWidth,
                             kPreviewCanvasHeight,
                             LV_COLOR_FORMAT_ARGB8888);
        lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(preview_image_);
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
