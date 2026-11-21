/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace app {
class Paths;
class Settings;
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
    void build_content(lv_obj_t* content) override;
    void refresh_api_key_display();
    void refresh_model_display();
    void refresh_status_display();
    void select_previous_model();
    void select_next_model();
    void select_model_by_index(std::size_t index);
    bool save_settings();
    static std::size_t model_index_for(std::string_view model);
    std::string_view selected_model() const;
    static void model_dropdown_changed_cb(lv_event_t* event);

    app::Settings& settings_;
    const app::Paths& paths_;
    std::string api_key_buffer_;
    std::size_t selected_model_index_{0};
    lv_obj_t* title_label_{nullptr};
    lv_obj_t* subtitle_label_{nullptr};
    lv_obj_t* api_key_textarea_{nullptr};
    lv_obj_t* model_dropdown_{nullptr};
    lv_obj_t* status_label_{nullptr};
    bool dirty_{false};
};

} // namespace screen
