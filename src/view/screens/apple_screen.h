/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <filesystem>
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
    void build_content(lv_obj_t* content) override;
    void refresh_prompt_display();
    void refresh_status_display();
    void refresh_focus_display();
    bool generate_image();
    void set_preview_image(const std::filesystem::path& image_path);
    static std::string to_lvgl_path(const std::filesystem::path& path);
    static bool is_printable_key(uint32_t key);

    app::Settings& settings_;
    const app::Paths& paths_;
    std::string prompt_buffer_;
    lv_obj_t* title_label_{nullptr};
    lv_obj_t* prompt_card_{nullptr};
    lv_obj_t* prompt_label_{nullptr};
    lv_obj_t* prompt_textarea_{nullptr};
    lv_obj_t* preview_card_{nullptr};
    lv_obj_t* preview_hint_label_{nullptr};
    lv_obj_t* preview_image_{nullptr};
    lv_obj_t* status_label_{nullptr};
    std::string preview_path_;
    std::string preview_lvgl_path_;
    std::string action_status_;
    bool busy_{false};
};

} // namespace screen
