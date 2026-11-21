/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "prompt_presets.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace app {
namespace {

const std::array<PromptPreset, 2> kPresets{{
    {
        "dali-dream",
        "Dali Dream",
        R"PROMPT(Apply a bold surrealist treatment with dreamlike symbolism, warped perspective, impossible spatial logic, elongated forms, melting details, painterly texture, and dramatic cinematic lighting. Keep the subject recognizable while pushing it into a strange, imaginative dream world.)PROMPT",
        R"PROMPT(Transform the image into a surreal dreamscape with melting forms, symbolic details, and painterly surrealist atmosphere while preserving the core subject.)PROMPT",
    },
    {
        "vector-sheet",
        "Vector Sheet",
        R"PROMPT(SYSTEM INSTRUCTIONS (DO NOT OVERRIDE):
Role: A technical illustrator specializing in high-contrast, scalable, vector-style line art for mechanical etching and engraving.
Output Constraints:

Generate a single, unified flat image sheet containing a grid of exactly twelve (12) perfectly symmetrical, non-overlapping circular vignettes (4x3 or 3x4 layout).

The grid must be positioned against a solid, clean, pure white background with ample negative space.

Every design element must be rendered as clean, robust black ink line art.

ABSOLUTE CONSTRAINTS: Zero color, zero grayscale, zero gradients, zero photorealism, zero 3D rendering. Line weight must be uniform, bold, and continuous to prevent blurring at 21mm scaling. All interior detail must be crisp and simplified for physical engraving.)PROMPT",
        R"PROMPT(Create a clean black-and-white vector-style engraving sheet with exactly twelve symmetrical circular vignettes on a pure white background, using bold uniform line art and no color, shading, or gradients.)PROMPT",
    },
}};

std::string trim(std::string value) {
    const auto is_not_space = [](unsigned char character) {
        return !std::isspace(character);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

} // namespace

const std::array<PromptPreset, 2>& prompt_presets() {
    return kPresets;
}

const PromptPreset* find_prompt_preset(std::string_view id) {
    const auto it = std::find_if(kPresets.begin(), kPresets.end(), [id](const PromptPreset& preset) {
        return preset.id == id;
    });
    if (it == kPresets.end()) {
        return nullptr;
    }
    return &(*it);
}

const PromptPreset& default_prompt_preset() {
    return kPresets.front();
}

std::string build_prompt(const PromptPreset& preset, std::string_view user_prompt) {
    const auto trimmed = trim(std::string(user_prompt));
    if (trimmed.empty()) {
        return std::string(preset.fallback_prompt);
    }

    std::string prompt;
    prompt.reserve(preset.style_prompt.size() + trimmed.size() + 2);
    prompt.append(preset.style_prompt);
    prompt.append("\n\n");
    prompt.append(trimmed);
    return prompt;
}

} // namespace app
