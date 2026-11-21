/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <string>
#include <string_view>

namespace app {

struct PromptPreset {
    std::string_view id;
    std::string_view label;
    std::string_view style_prompt;
    std::string_view fallback_prompt;
};

const std::array<PromptPreset, 2>& prompt_presets();
const PromptPreset* find_prompt_preset(std::string_view id);
const PromptPreset& default_prompt_preset();
std::string build_prompt(const PromptPreset& preset, std::string_view user_prompt);

} // namespace app
