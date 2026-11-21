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

    bool handle_key(uint32_t key, bool long_pressed) override;

private:
    enum class FocusTarget {
        Prompt,
        Capture,
        Settings,
    };

    enum class ScreenMode {
        Prompt,
        Preview,
        CaptureConfirm,
    };

    void build_content(lv_obj_t* content) override;
    void refresh_prompt_display();
    void refresh_status_display();
    void refresh_focus_display();
    void show_prompt_mode();
    void clear_prompt();
    void show_preview_mode();
    void show_capture_confirm_mode();
    bool generate_image(const std::optional<std::filesystem::path>& source_image_path = std::nullopt);
    bool capture_photo();
    void set_preview_image(const std::filesystem::path& image_path);
    void open_settings();
    static void settings_button_cb(lv_event_t* event);
    static void capture_button_cb(lv_event_t* event);
    static void prompt_button_cb(lv_event_t* event);
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
    lv_obj_t* prompt_button_{nullptr};
    std::string preview_path_;
    std::string preview_lvgl_path_;
    std::filesystem::path pending_capture_path_;
    std::string action_status_;
    FocusTarget focus_{FocusTarget::Prompt};
    ScreenMode mode_{ScreenMode::Prompt};
    bool busy_{false};
};

} // namespace screen
