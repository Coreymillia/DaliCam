/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace app {
class Paths;
class Settings;
}

namespace screen {

class AppleScreen : public BaseScreen {
public:
    AppleScreen(viewmodel::BaseViewModel& view_model,
                app::AssetManager& assets,
                app::Settings& settings,
                const app::Paths& paths);
    ~AppleScreen() override;

    bool handle_key(uint32_t key, bool long_pressed) override;

private:
    enum class FocusTarget {
        Prompt,
        AiGallery,
        MediaGallery,
        Capture,
        Settings,
    };

    enum class ScreenMode {
        Prompt,
        Preview,
        CaptureConfirm,
        Gallery,
    };

    enum class GalleryKind {
        Ai,
        Media,
    };

    enum class ConfirmAction {
        None,
        Capture,
        GenerateFromSelection,
        DeleteGalleryImage,
    };

    struct GalleryItem {
        std::filesystem::path path;
        std::string source;
        std::filesystem::file_time_type modified_at{};
    };

    void build_content(lv_obj_t* content) override;
    void refresh_prompt_display();
    void refresh_status_display();
    void refresh_focus_display();
    void show_prompt_mode();
    void clear_prompt();
    void show_preview_mode();
    void show_capture_confirm_mode();
    void show_gallery_mode(GalleryKind kind);
    void close_gallery_mode();
    void refresh_latest_preview_from_storage();
    void refresh_gallery_display();
    void refresh_gallery_preview();
    void refresh_gallery_status();
    void refresh_gallery_buttons();
    bool generate_image(const std::optional<std::filesystem::path>& source_image_path = std::nullopt);
    bool capture_photo();
    void set_preview_image(const std::filesystem::path& image_path);
    void set_gallery_preview_image(const std::filesystem::path& image_path);
    void open_settings();
    void open_gallery(GalleryKind kind);
    void select_gallery_item(std::size_t index);
    void step_gallery_selection(int step);
    std::string compact_model_label() const;
    std::string compact_personality_label() const;
    std::string gallery_title() const;
    static void settings_button_cb(lv_event_t* event);
    static void capture_button_cb(lv_event_t* event);
    static void ai_gallery_button_cb(lv_event_t* event);
    static void media_gallery_button_cb(lv_event_t* event);
    static void gallery_close_button_cb(lv_event_t* event);
    static void gallery_item_cb(lv_event_t* event);
    static void preview_refresh_timer_cb(lv_timer_t* timer);
    static std::string to_lvgl_path(const std::filesystem::path& path);
    static bool is_printable_key(uint32_t key);

    app::Settings& settings_;
    const app::Paths& paths_;
    std::string prompt_buffer_;
    lv_obj_t* prompt_card_{nullptr};
    lv_obj_t* prompt_label_{nullptr};
    lv_obj_t* prompt_textarea_{nullptr};
    lv_obj_t* preview_card_{nullptr};
    lv_obj_t* preview_hint_label_{nullptr};
    lv_obj_t* preview_image_{nullptr};
    lv_obj_t* confirm_label_{nullptr};
    lv_obj_t* status_label_{nullptr};
    lv_obj_t* settings_button_{nullptr};
    lv_obj_t* capture_button_{nullptr};
    lv_obj_t* ai_gallery_button_{nullptr};
    lv_obj_t* media_gallery_button_{nullptr};
    lv_obj_t* gallery_card_{nullptr};
    lv_obj_t* gallery_title_label_{nullptr};
    lv_obj_t* gallery_preview_card_{nullptr};
    lv_obj_t* gallery_preview_hint_label_{nullptr};
    lv_obj_t* gallery_preview_image_{nullptr};
    lv_obj_t* gallery_info_label_{nullptr};
    lv_obj_t* gallery_nav_label_{nullptr};
    lv_obj_t* gallery_close_button_{nullptr};
    std::vector<GalleryItem> gallery_items_;
    std::vector<uint8_t> gallery_canvas_buffer_;
    std::filesystem::path selected_source_path_;
    std::filesystem::path preview_path_;
    std::string preview_lvgl_path_;
    std::string gallery_preview_path_;
    std::vector<uint8_t> preview_canvas_buffer_;
    std::filesystem::path pending_capture_path_;
    std::filesystem::path pending_confirm_source_path_;
    std::string action_status_;
    GalleryKind gallery_kind_{GalleryKind::Ai};
    ConfirmAction confirm_action_{ConfirmAction::None};
    FocusTarget focus_{FocusTarget::Prompt};
    ScreenMode mode_{ScreenMode::Prompt};
    std::size_t gallery_selected_index_{0};
    bool busy_{false};
    lv_timer_t* preview_refresh_timer_{nullptr};
};

} // namespace screen
