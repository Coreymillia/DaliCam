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

#include <algorithm>
#include <cctype>
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
constexpr int kGalleryCanvasWidth = 300;
constexpr int kGalleryCanvasHeight = 140;
constexpr std::size_t kPreviewCanvasBufferSize = static_cast<std::size_t>(kPreviewCanvasWidth) *
                                                 static_cast<std::size_t>(kPreviewCanvasHeight) * 4U;
constexpr std::size_t kGalleryCanvasBufferSize = static_cast<std::size_t>(kGalleryCanvasWidth) *
                                                 static_cast<std::size_t>(kGalleryCanvasHeight) * 4U;

bool is_printable_key(uint32_t key) {
    return key >= 32 && key <= 126;
}

bool is_yes_key(uint32_t key) {
    return key == 'y' || key == 'Y';
}

bool is_no_key(uint32_t key) {
    return key == 'n' || key == 'N';
}

std::string abbreviate_words(std::string_view value) {
    std::string result;
    bool take_next = true;
    for (const char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            if (take_next && result.size() < 4) {
                result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                take_next = false;
            }
        }
        else {
            take_next = true;
        }
    }
    if (result.empty()) {
        result.assign(value.begin(), value.end());
        if (result.size() > 4) {
            result.resize(4);
        }
    }
    return result;
}

std::string compact_filename_label(const std::filesystem::path& path, std::string_view source) {
    std::string label(source);
    label.append(" • ");
    label.append(path.filename().string());
    return label;
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

bool load_canvas_preview(const std::filesystem::path& image_path,
                         const std::filesystem::path& preview_file,
                         int canvas_width,
                         int canvas_height,
                         std::vector<uint8_t>& buffer,
                         lv_obj_t* canvas,
                         lv_obj_t* hint_label,
                         std::string& error) {
    if (!create_canvas_preview(image_path, preview_file, canvas_width, canvas_height, error)) {
        return false;
    }

    buffer.assign(static_cast<std::size_t>(canvas_width) * static_cast<std::size_t>(canvas_height) * 4U, 0);
    std::ifstream raw_file(preview_file, std::ios::binary);
    if (!raw_file) {
        error = "Unable to open preview buffer";
        return false;
    }

    raw_file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (raw_file.gcount() != static_cast<std::streamsize>(buffer.size())) {
        error = "Preview buffer size mismatch";
        return false;
    }

    if (canvas) {
        lv_canvas_set_buffer(canvas, buffer.data(), canvas_width, canvas_height, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(canvas);
    }
    if (hint_label) {
        lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
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
    lv_label_set_text(status_label_, "M:NB2.1 • P:DD");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_width(status_label_, 126);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    auto* status_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(status_label_, status_font ? status_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(status_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    ai_gallery_button_ = lv_button_create(panel);
    lv_obj_remove_style_all(ai_gallery_button_);
    lv_obj_add_flag(ai_gallery_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(ai_gallery_button_, 88, 24);
    lv_obj_align(ai_gallery_button_, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_radius(ai_gallery_button_, 12, 0);
    lv_obj_set_style_pad_hor(ai_gallery_button_, 8, 0);
    lv_obj_set_style_pad_ver(ai_gallery_button_, 4, 0);
    lv_obj_set_style_border_width(ai_gallery_button_, 1, 0);
    lv_obj_set_style_border_opa(ai_gallery_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(ai_gallery_button_, LV_OPA_70, 0);
    lv_obj_add_event_cb(ai_gallery_button_, ai_gallery_button_cb, LV_EVENT_CLICKED, this);
    auto* ai_gallery_font = assets().load_standard_font(9, app::StandardFontWeight::Bold);
    auto* ai_gallery_label = lv_label_create(ai_gallery_button_);
    lv_label_set_text(ai_gallery_label, "AI Gallery");
    lv_obj_set_style_text_font(ai_gallery_label, ai_gallery_font ? ai_gallery_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(ai_gallery_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(ai_gallery_label);
    reactive::bind_theme(ai_gallery_button_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    media_gallery_button_ = lv_button_create(panel);
    lv_obj_remove_style_all(media_gallery_button_);
    lv_obj_add_flag(media_gallery_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(media_gallery_button_, 88, 24);
    lv_obj_align(media_gallery_button_, LV_ALIGN_TOP_LEFT, 6, 36);
    lv_obj_set_style_radius(media_gallery_button_, 12, 0);
    lv_obj_set_style_pad_hor(media_gallery_button_, 8, 0);
    lv_obj_set_style_pad_ver(media_gallery_button_, 4, 0);
    lv_obj_set_style_border_width(media_gallery_button_, 1, 0);
    lv_obj_set_style_border_opa(media_gallery_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(media_gallery_button_, LV_OPA_70, 0);
    lv_obj_add_event_cb(media_gallery_button_, media_gallery_button_cb, LV_EVENT_CLICKED, this);
    auto* media_gallery_font = assets().load_standard_font(9, app::StandardFontWeight::Bold);
    auto* media_gallery_label = lv_label_create(media_gallery_button_);
    lv_label_set_text(media_gallery_label, "Photos");
    lv_obj_set_style_text_font(media_gallery_label, media_gallery_font ? media_gallery_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(media_gallery_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(media_gallery_label);
    reactive::bind_theme(media_gallery_button_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

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

    gallery_card_ = lv_obj_create(panel);
    lv_obj_remove_style_all(gallery_card_);
    lv_obj_set_size(gallery_card_, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(gallery_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(gallery_card_, 8, 0);
    lv_obj_set_style_pad_all(gallery_card_, 8, 0);
    lv_obj_set_style_pad_row(gallery_card_, 4, 0);
    lv_obj_set_flex_flow(gallery_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(gallery_card_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    reactive::bind_theme(gallery_card_, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
    lv_obj_set_style_border_width(gallery_card_, 1, 0);
    lv_obj_set_style_border_opa(gallery_card_, LV_OPA_COVER, 0);
    lv_obj_add_flag(gallery_card_, LV_OBJ_FLAG_HIDDEN);

    gallery_title_label_ = lv_label_create(gallery_card_);
    lv_label_set_text(gallery_title_label_, "Gallery");
    auto* gallery_title_font = assets().load_standard_font(11, app::StandardFontWeight::Bold);
    lv_obj_set_style_text_font(gallery_title_label_, gallery_title_font ? gallery_title_font : &lv_font_montserrat_12, 0);
    reactive::bind_theme(gallery_title_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    gallery_close_button_ = lv_button_create(gallery_card_);
    lv_obj_remove_style_all(gallery_close_button_);
    lv_obj_add_flag(gallery_close_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(gallery_close_button_, 54, 22);
    lv_obj_align(gallery_close_button_, LV_ALIGN_TOP_RIGHT, -2, -2);
    lv_obj_set_style_radius(gallery_close_button_, 11, 0);
    lv_obj_set_style_pad_hor(gallery_close_button_, 6, 0);
    lv_obj_set_style_pad_ver(gallery_close_button_, 3, 0);
    lv_obj_set_style_border_width(gallery_close_button_, 1, 0);
    lv_obj_set_style_border_opa(gallery_close_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(gallery_close_button_, LV_OPA_70, 0);
    lv_obj_add_event_cb(gallery_close_button_, gallery_close_button_cb, LV_EVENT_CLICKED, this);
    auto* gallery_close_font = assets().load_standard_font(9, app::StandardFontWeight::Bold);
    auto* gallery_close_label = lv_label_create(gallery_close_button_);
    lv_label_set_text(gallery_close_label, "Back");
    lv_obj_set_style_text_font(gallery_close_label, gallery_close_font ? gallery_close_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(gallery_close_label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(gallery_close_label);
    reactive::bind_theme(gallery_close_button_, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

    gallery_preview_card_ = lv_obj_create(gallery_card_);
    lv_obj_remove_style_all(gallery_preview_card_);
    lv_obj_set_width(gallery_preview_card_, LV_PCT(100));
    lv_obj_set_height(gallery_preview_card_, kGalleryCanvasHeight);
    lv_obj_clear_flag(gallery_preview_card_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(gallery_preview_card_, view_model().dark_mode_subject(), reactive::ThemeRole::Surface);
    lv_obj_set_style_radius(gallery_preview_card_, 8, 0);
    lv_obj_set_style_pad_all(gallery_preview_card_, 0, 0);
    lv_obj_set_style_border_width(gallery_preview_card_, 1, 0);
    lv_obj_set_style_border_opa(gallery_preview_card_, LV_OPA_COVER, 0);

    gallery_preview_hint_label_ = lv_label_create(gallery_preview_card_);
    lv_label_set_text(gallery_preview_hint_label_, "No image selected.");
    lv_obj_set_width(gallery_preview_hint_label_, LV_PCT(100));
    lv_label_set_long_mode(gallery_preview_hint_label_, LV_LABEL_LONG_WRAP);
    auto* gallery_hint_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(gallery_preview_hint_label_, gallery_hint_font ? gallery_hint_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(gallery_preview_hint_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    lv_obj_center(gallery_preview_hint_label_);

    gallery_canvas_buffer_.assign(kGalleryCanvasBufferSize, 0);
    gallery_preview_image_ = lv_canvas_create(gallery_preview_card_);
    lv_obj_set_size(gallery_preview_image_, kGalleryCanvasWidth, kGalleryCanvasHeight);
    lv_canvas_set_buffer(gallery_preview_image_,
                         gallery_canvas_buffer_.data(),
                         kGalleryCanvasWidth,
                         kGalleryCanvasHeight,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(gallery_preview_image_, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(gallery_preview_image_, LV_OBJ_FLAG_HIDDEN);

    gallery_info_label_ = lv_label_create(gallery_card_);
    lv_label_set_text(gallery_info_label_, "");
    lv_obj_set_width(gallery_info_label_, LV_PCT(100));
    lv_label_set_long_mode(gallery_info_label_, LV_LABEL_LONG_DOT);
    auto* gallery_info_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(gallery_info_label_, gallery_info_font ? gallery_info_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(gallery_info_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    gallery_nav_label_ = lv_label_create(gallery_card_);
    lv_label_set_text(gallery_nav_label_, "F = up   X = down   Enter = select   Esc = back");
    lv_obj_set_width(gallery_nav_label_, LV_PCT(100));
    lv_label_set_long_mode(gallery_nav_label_, LV_LABEL_LONG_WRAP);
    auto* gallery_nav_font = assets().load_standard_font(8, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(gallery_nav_label_, gallery_nav_font ? gallery_nav_font : &lv_font_montserrat_10, 0);
    reactive::bind_theme(gallery_nav_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    refresh_prompt_display();
    show_prompt_mode();
}

bool AppleScreen::handle_key(uint32_t key, bool long_pressed) {
    if (long_pressed) {
        return false;
    }

    if (mode_ == ScreenMode::Gallery) {
        if (key == LV_KEY_ESC) {
            close_gallery_mode();
            return true;
        }

        if (!gallery_items_.empty()) {
            if (key == 'f' || key == 'F' || key == LV_KEY_UP) {
                step_gallery_selection(-1);
                return true;
            }

            if (key == 'x' || key == 'X' || key == LV_KEY_DOWN) {
                step_gallery_selection(1);
                return true;
            }

            if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
                select_gallery_item(gallery_selected_index_);
                action_status_ = "Selected image";
                refresh_status_display();
                close_gallery_mode();
                return true;
            }
        }

        return false;
    }

    if (mode_ == ScreenMode::CaptureConfirm) {
        if (is_yes_key(key)) {
            if (confirm_action_ == ConfirmAction::GenerateFromSelection) {
                if (pending_confirm_source_path_.empty()) {
                    action_status_ = "No selected photo available";
                    refresh_status_display();
                    show_prompt_mode();
                    return true;
                }
                return generate_image(pending_confirm_source_path_);
            }

            if (confirm_action_ == ConfirmAction::Capture) {
                if (pending_capture_path_.empty()) {
                    action_status_ = "No captured photo available";
                    refresh_status_display();
                    show_prompt_mode();
                    return true;
                }
                return generate_image(pending_capture_path_);
            }
        }

        if (is_no_key(key) || key == LV_KEY_ESC) {
            if (confirm_action_ == ConfirmAction::Capture && !pending_capture_path_.empty()) {
                std::error_code delete_error;
                std::filesystem::remove(pending_capture_path_, delete_error);
                if (delete_error) {
                    LOG_WARN("failed to delete cancelled capture {}: {}",
                             pending_capture_path_.string(),
                             delete_error.message());
                }
                if (!selected_source_path_.empty() && selected_source_path_ == pending_capture_path_) {
                    selected_source_path_.clear();
                }
                pending_capture_path_.clear();
            }
            pending_confirm_source_path_.clear();
            confirm_action_ = ConfirmAction::None;
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

    if (focus_ == FocusTarget::AiGallery) {
        if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
            open_gallery(GalleryKind::Ai);
            return true;
        }
        if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) {
            focus_ = FocusTarget::MediaGallery;
            refresh_focus_display();
            return true;
        }
        if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
            focus_ = FocusTarget::Prompt;
            refresh_focus_display();
            return true;
        }
        return false;
    }

    if (focus_ == FocusTarget::MediaGallery) {
        if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
            open_gallery(GalleryKind::Media);
            return true;
        }
        if (key == LV_KEY_UP) {
            focus_ = FocusTarget::AiGallery;
            refresh_focus_display();
            return true;
        }
        if (key == LV_KEY_RIGHT) {
            focus_ = FocusTarget::Capture;
            refresh_focus_display();
            return true;
        }
        if (key == LV_KEY_LEFT || key == LV_KEY_DOWN) {
            focus_ = FocusTarget::Prompt;
            refresh_focus_display();
            return true;
        }
        return false;
    }

    if (focus_ == FocusTarget::Prompt) {
        if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
            focus_ = FocusTarget::AiGallery;
            refresh_focus_display();
            return true;
        }
        if (key == LV_KEY_DOWN) {
            focus_ = FocusTarget::MediaGallery;
            refresh_focus_display();
            return true;
        }
        if (key == LV_KEY_RIGHT) {
            focus_ = FocusTarget::Capture;
            refresh_focus_display();
            return true;
        }
    }

    if (key == LV_KEY_ENTER || key == '\r' || key == '\n') {
        if (selected_source_path_.empty()) {
            return generate_image();
        }
        pending_confirm_source_path_ = selected_source_path_;
        confirm_action_ = ConfirmAction::GenerateFromSelection;
        show_capture_confirm_mode();
        return true;
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
    const std::string compact_meta = "M:" + compact_model_label() + " • P:" + compact_personality_label();
    view_model().set_header_status(compact_meta.c_str());

    if (!status_label_) {
        return;
    }

    if (action_status_.empty()) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(status_label_, action_status_.c_str());
}

void AppleScreen::refresh_focus_display() {
    if (mode_ == ScreenMode::Gallery) {
        if (prompt_textarea_) {
            lv_obj_remove_state(prompt_textarea_, LV_STATE_FOCUSED);
        }
        if (ai_gallery_button_) {
            lv_obj_remove_state(ai_gallery_button_, LV_STATE_FOCUSED);
        }
        if (media_gallery_button_) {
            lv_obj_remove_state(media_gallery_button_, LV_STATE_FOCUSED);
        }
        if (settings_button_) {
            lv_obj_remove_state(settings_button_, LV_STATE_FOCUSED);
        }
        if (capture_button_) {
            lv_obj_remove_state(capture_button_, LV_STATE_FOCUSED);
        }
        const auto palette = view::palette(view_model().is_dark_mode());
        if (prompt_card_) {
            lv_obj_set_style_border_width(prompt_card_, 1, 0);
            lv_obj_set_style_border_color(prompt_card_, palette.border, 0);
        }
        if (ai_gallery_button_) {
            lv_obj_set_style_border_width(ai_gallery_button_, 1, 0);
            lv_obj_set_style_border_color(ai_gallery_button_, palette.border, 0);
        }
        if (media_gallery_button_) {
            lv_obj_set_style_border_width(media_gallery_button_, 1, 0);
            lv_obj_set_style_border_color(media_gallery_button_, palette.border, 0);
        }
        if (settings_button_) {
            lv_obj_set_style_border_width(settings_button_, 1, 0);
            lv_obj_set_style_border_color(settings_button_, palette.border, 0);
        }
        if (capture_button_) {
            lv_obj_set_style_border_width(capture_button_, 1, 0);
            lv_obj_set_style_border_color(capture_button_, palette.border, 0);
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

    if (ai_gallery_button_) {
        if (focus_ == FocusTarget::AiGallery) {
            lv_obj_add_state(ai_gallery_button_, LV_STATE_FOCUSED);
        }
        else {
            lv_obj_remove_state(ai_gallery_button_, LV_STATE_FOCUSED);
        }
    }

    if (media_gallery_button_) {
        if (focus_ == FocusTarget::MediaGallery) {
            lv_obj_add_state(media_gallery_button_, LV_STATE_FOCUSED);
        }
        else {
            lv_obj_remove_state(media_gallery_button_, LV_STATE_FOCUSED);
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
    if (ai_gallery_button_) {
        lv_obj_set_style_border_width(ai_gallery_button_, focus_ == FocusTarget::AiGallery ? 2 : 1, 0);
        lv_obj_set_style_border_color(ai_gallery_button_, focus_ == FocusTarget::AiGallery ? palette.primary : palette.border, 0);
    }
    if (media_gallery_button_) {
        lv_obj_set_style_border_width(media_gallery_button_, focus_ == FocusTarget::MediaGallery ? 2 : 1, 0);
        lv_obj_set_style_border_color(media_gallery_button_, focus_ == FocusTarget::MediaGallery ? palette.primary : palette.border, 0);
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
    if (gallery_card_) {
        lv_obj_add_flag(gallery_card_, LV_OBJ_FLAG_HIDDEN);
    }
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
    if (ai_gallery_button_) {
        lv_obj_remove_flag(ai_gallery_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (media_gallery_button_) {
        lv_obj_remove_flag(media_gallery_button_, LV_OBJ_FLAG_HIDDEN);
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
    if (ai_gallery_button_) {
        lv_obj_add_flag(ai_gallery_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (media_gallery_button_) {
        lv_obj_add_flag(media_gallery_button_, LV_OBJ_FLAG_HIDDEN);
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
    if (ai_gallery_button_) {
        lv_obj_add_flag(ai_gallery_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (media_gallery_button_) {
        lv_obj_add_flag(media_gallery_button_, LV_OBJ_FLAG_HIDDEN);
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
        const char* label_text = "Confirm? Y / N";
        if (confirm_action_ == ConfirmAction::GenerateFromSelection) {
            label_text = "Use selected image? Y / N";
        }
        else if (confirm_action_ == ConfirmAction::Capture) {
            label_text = "Use captured photo? Y / N";
        }
        lv_label_set_text(confirm_label_, label_text);
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
        pending_confirm_source_path_.clear();
        confirm_action_ = ConfirmAction::None;
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
    pending_confirm_source_path_.clear();
    confirm_action_ = ConfirmAction::None;
    show_prompt_mode();
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
        pending_confirm_source_path_.clear();
        confirm_action_ = ConfirmAction::None;
        refresh_status_display();
        show_prompt_mode();
        LOG_WARN("Camera capture failed: {}", result.error);
        return false;
    }

    pending_capture_path_ = result.path;
    pending_confirm_source_path_ = result.path;
    confirm_action_ = ConfirmAction::Capture;
    set_preview_image(capture_path);
    action_status_.clear();
    show_capture_confirm_mode();
    LOG_INFO("Camera photo captured to {}", result.path);
    return true;
}

void AppleScreen::open_settings() {
    view_model().show_butter_page();
}

std::string AppleScreen::compact_model_label() const {
    return abbreviate_words(settings_.gemini_model);
}

std::string AppleScreen::compact_personality_label() const {
    const auto* preset = app::find_prompt_preset(settings_.default_personality);
    const auto label = preset ? preset->label : settings_.default_personality;
    return abbreviate_words(label);
}

std::string AppleScreen::gallery_title() const {
    return gallery_kind_ == GalleryKind::Ai ? "AI Gallery" : "Photo Gallery";
}

void AppleScreen::show_gallery_mode(GalleryKind kind) {
    open_gallery(kind);
}

void AppleScreen::open_gallery(GalleryKind kind) {
    gallery_kind_ = kind;
    mode_ = ScreenMode::Gallery;
    if (preview_card_) {
        lv_obj_add_flag(preview_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (prompt_card_) {
        lv_obj_add_flag(prompt_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (ai_gallery_button_) {
        lv_obj_add_flag(ai_gallery_button_, LV_OBJ_FLAG_HIDDEN);
    }
    if (media_gallery_button_) {
        lv_obj_add_flag(media_gallery_button_, LV_OBJ_FLAG_HIDDEN);
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
    if (gallery_card_) {
        lv_obj_remove_flag(gallery_card_, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_gallery_display();
    focus_ = FocusTarget::Prompt;
    refresh_focus_display();
}

void AppleScreen::close_gallery_mode() {
    if (mode_ != ScreenMode::Gallery) {
        return;
    }
    show_prompt_mode();
}

void AppleScreen::select_gallery_item(std::size_t index) {
    if (index >= gallery_items_.size()) {
        return;
    }

    gallery_selected_index_ = index;
    const auto& item = gallery_items_[index];
    set_preview_image(item.path);
    set_gallery_preview_image(item.path);
    refresh_gallery_status();
}

void AppleScreen::step_gallery_selection(int step) {
    if (gallery_items_.empty()) {
        return;
    }

    const auto size = static_cast<int>(gallery_items_.size());
    auto next = static_cast<int>(gallery_selected_index_) + step;
    while (next < 0) {
        next += size;
    }
    while (next >= size) {
        next -= size;
    }
    select_gallery_item(static_cast<std::size_t>(next));
}

void AppleScreen::refresh_gallery_display() {
    gallery_items_.clear();

    const auto collect = [&](const std::filesystem::path& directory, std::string_view source_label) {
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

            GalleryItem item;
            item.path = entry.path();
            item.source = std::string(source_label);
            item.modified_at = entry.last_write_time(entry_error);
            if (entry_error) {
                item.modified_at = {};
            }
            gallery_items_.push_back(std::move(item));
        }
    };

    if (gallery_kind_ == GalleryKind::Ai) {
        collect(paths_.generated_dir(), "AI");
    }
    else {
        collect(paths_.camera_dir(), "Camera");
        collect(paths_.uploads_dir(), "Upload");
    }

    std::sort(gallery_items_.begin(), gallery_items_.end(), [](const auto& a, const auto& b) {
        return a.modified_at > b.modified_at;
    });

    if (gallery_title_label_) {
        lv_label_set_text(gallery_title_label_, gallery_title().c_str());
    }

    if (gallery_items_.empty()) {
        gallery_selected_index_ = 0;
        if (gallery_preview_hint_label_) {
            lv_label_set_text(gallery_preview_hint_label_, "No images found yet.");
            lv_obj_remove_flag(gallery_preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (gallery_preview_image_) {
            lv_obj_add_flag(gallery_preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (gallery_info_label_) {
            lv_label_set_text(gallery_info_label_, "");
        }
        if (gallery_nav_label_) {
            lv_obj_remove_flag(gallery_nav_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    std::size_t initial_index = 0;
    const std::filesystem::path preferred_path = !selected_source_path_.empty() ? selected_source_path_ : preview_path_;
    for (std::size_t i = 0; i < gallery_items_.size(); ++i) {
        if (gallery_items_[i].path == preferred_path) {
            initial_index = i;
            break;
        }
    }
    gallery_selected_index_ = initial_index;
    select_gallery_item(gallery_selected_index_);
}

void AppleScreen::refresh_gallery_preview() {
    if (gallery_items_.empty() || gallery_selected_index_ >= gallery_items_.size()) {
        return;
    }

    const auto& item = gallery_items_[gallery_selected_index_];
    set_gallery_preview_image(item.path);
    if (gallery_preview_hint_label_) {
        lv_obj_add_flag(gallery_preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppleScreen::refresh_gallery_status() {
    if (!gallery_info_label_ || !gallery_nav_label_) {
        return;
    }

    if (gallery_items_.empty()) {
        lv_label_set_text(gallery_info_label_, "");
        lv_obj_remove_flag(gallery_nav_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const auto index_text = std::to_string(gallery_selected_index_ + 1) + "/" + std::to_string(gallery_items_.size());
    const auto source_text = gallery_items_[gallery_selected_index_].source;
    const std::string info = source_text + " • " + index_text;
    lv_label_set_text(gallery_info_label_, info.c_str());
    lv_obj_remove_flag(gallery_nav_label_, LV_OBJ_FLAG_HIDDEN);
}

void AppleScreen::refresh_gallery_buttons() {
    refresh_gallery_status();
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

void AppleScreen::ai_gallery_button_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }
    screen->open_gallery(GalleryKind::Ai);
}

void AppleScreen::media_gallery_button_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }
    screen->open_gallery(GalleryKind::Media);
}

void AppleScreen::gallery_close_button_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }
    screen->close_gallery_mode();
}

void AppleScreen::gallery_item_cb(lv_event_t* event) {
    auto* screen = static_cast<AppleScreen*>(lv_event_get_user_data(event));
    if (!screen) {
        return;
    }

    if (screen->mode_ != ScreenMode::Gallery || screen->gallery_items_.empty()) {
        return;
    }

    screen->select_gallery_item(screen->gallery_selected_index_);
    screen->action_status_ = "Selected image";
    screen->refresh_status_display();
    screen->close_gallery_mode();
}

void AppleScreen::preview_refresh_timer_cb(lv_timer_t* timer) {
    auto* screen = static_cast<AppleScreen*>(lv_timer_get_user_data(timer));
    if (!screen) {
        return;
    }
    screen->refresh_latest_preview_from_storage();
}

void AppleScreen::refresh_latest_preview_from_storage() {
    if (mode_ != ScreenMode::Prompt || !preview_path_.empty()) {
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

    if (latest_path) {
        set_preview_image(*latest_path);
        return;
    }

    if (preview_image_ && preview_hint_label_) {
        preview_path_.clear();
        selected_source_path_.clear();
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppleScreen::set_preview_image(const std::filesystem::path& image_path) {
    preview_path_ = image_path.string();
    selected_source_path_ = image_path;

    const auto preview_file = paths_.temp_dir() / "latest-preview.raw";
    std::string preview_error;
    if (!load_canvas_preview(image_path,
                             preview_file,
                             kPreviewCanvasWidth,
                             kPreviewCanvasHeight,
                             preview_canvas_buffer_,
                             preview_image_,
                             preview_hint_label_,
                             preview_error)) {
        LOG_WARN("preview generation failed for {}: {}", image_path.string(), preview_error);
        preview_path_.clear();
        selected_source_path_.clear();
        if (preview_image_) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_hint_label_) {
            lv_obj_remove_flag(preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (preview_card_) {
        lv_obj_scroll_to_view_recursive(preview_card_, LV_ANIM_OFF);
    }
}

void AppleScreen::set_gallery_preview_image(const std::filesystem::path& image_path) {
    gallery_preview_path_ = image_path.string();

    const auto preview_file = paths_.temp_dir() / "gallery-preview.raw";
    std::string preview_error;
    if (!load_canvas_preview(image_path,
                             preview_file,
                             kGalleryCanvasWidth,
                             kGalleryCanvasHeight,
                             gallery_canvas_buffer_,
                             gallery_preview_image_,
                             gallery_preview_hint_label_,
                             preview_error)) {
        LOG_WARN("gallery preview generation failed for {}: {}", image_path.string(), preview_error);
        gallery_preview_path_.clear();
        if (gallery_preview_image_) {
            lv_obj_add_flag(gallery_preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (gallery_preview_hint_label_) {
            lv_obj_remove_flag(gallery_preview_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (gallery_preview_card_) {
        lv_obj_scroll_to_view_recursive(gallery_preview_card_, LV_ANIM_OFF);
    }
}

std::string AppleScreen::to_lvgl_path(const std::filesystem::path& path) {
    return std::string("A:") + path.generic_string();
}

bool AppleScreen::is_printable_key(uint32_t key) {
    return key >= 32 && key <= 126;
}

} // namespace screen
