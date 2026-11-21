/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace app {
class Paths;
class Settings;
struct PromptPreset;
}

namespace screen {

class ButterScreen : public BaseScreen {
public:
    ButterScreen(viewmodel::BaseViewModel& view_model,
                 app::AssetManager& assets,
                 app::Settings& settings,
                 const app::Paths& paths);

    bool handle_key(uint32_t key, bool long_pressed) override;

private:
    enum class FocusField {
        ApiKey,
        Personality,
        Model,
    };

    void build_content(lv_obj_t* content) override;

    void refresh_api_key_display();
    void refresh_personality_display();
    void refresh_model_display();
    void refresh_status_display();
    void refresh_focus_display();
    void set_focus(FocusField field);
    void focus_previous();
    void focus_next();
    void select_previous_model();
    void select_next_model();
    void select_model_by_index(std::size_t index);
    void select_previous_personality();
    void select_next_personality();
    void select_personality_by_index(std::size_t index);
    bool save_settings();
    bool generate_image();
    static std::size_t model_index_for(std::string_view model);
    static std::size_t personality_index_for(std::string_view personality_id);
    std::string_view selected_model() const;
    std::string_view selected_personality_id() const;
    const app::PromptPreset& selected_prompt_preset() const;
    static void model_dropdown_changed_cb(lv_event_t* event);
    static void personality_dropdown_changed_cb(lv_event_t* event);
    void set_preview_image(const std::filesystem::path& image_path);
    static std::string to_lvgl_path(const std::filesystem::path& path);

    app::Settings& settings_;
    const app::Paths& paths_;
    std::string api_key_buffer_;
    std::size_t selected_model_index_{0};
    std::size_t selected_personality_index_{0};
    FocusField focus_{FocusField::ApiKey};
    lv_obj_t* root_panel_{nullptr};
    lv_obj_t* title_label_{nullptr};
    lv_obj_t* subtitle_label_{nullptr};
    lv_obj_t* api_key_card_{nullptr};
    lv_obj_t* personality_card_{nullptr};
    lv_obj_t* model_card_{nullptr};
    lv_obj_t* preview_card_{nullptr};
    lv_obj_t* api_key_textarea_{nullptr};
    lv_obj_t* personality_dropdown_{nullptr};
    lv_obj_t* model_dropdown_{nullptr};
    lv_obj_t* preview_image_{nullptr};
    lv_obj_t* preview_hint_label_{nullptr};
    lv_obj_t* status_label_{nullptr};
    std::string preview_path_;
    std::string preview_lvgl_path_;
    std::string action_status_;
    bool dirty_{false};
};

} // namespace screen
