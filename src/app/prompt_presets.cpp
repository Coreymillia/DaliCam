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

const std::array<PromptPreset, 5> kPresets{{
    {
        "dali-dream",
        "Dali Dream",
        R"PROMPT(Apply a bold surrealist treatment with dreamlike symbolism, warped perspective, impossible spatial logic, elongated forms, melting details, painterly texture, and dramatic cinematic lighting. Keep the subject recognizable while pushing it into a strange, imaginative dream world.)PROMPT",
        R"PROMPT(Transform the image into a surreal dreamscape with melting forms, symbolic details, and painterly surrealist atmosphere while preserving the core subject.)PROMPT",
    },
    {
        "van-gogh",
        "Van Gogh",
        R"PROMPT(Restyle the photo with a dramatic post-impressionist oil painting treatment inspired by expressive brushwork, swirling motion, luminous moonlit color energy, vivid cobalt and golden tones, textured paint buildup, and emotionally charged lighting. Preserve the original subject and composition while translating it into a richly painted scene.)PROMPT",
        R"PROMPT(Transform the image into a vivid post-impressionist oil painting with swirling brushwork, bold texture, glowing night-sky color energy, and expressive painterly motion while keeping the subject recognizable.)PROMPT",
    },
    {
        "picasso",
        "Picasso",
        R"PROMPT(Reimagine the photo as a bold cubist painting with fractured planes, geometric simplification, angular forms, flattened perspective, and a confident modernist palette. Preserve the core subject and main composition, but break the scene into stylized overlapping shapes and strong painterly structure.)PROMPT",
        R"PROMPT(Turn the image into a striking cubist artwork with geometric planes, abstracted facial or object structure, flattened depth, and expressive modernist color.)PROMPT",
    },
    {
        "alex-grey",
        "Alex Grey",
        R"PROMPT(Transform the image into a visionary sacred-geometric artwork inspired by Alex Grey. Reshape the subject into highly structured sacred geometry rather than simply overlaying lines: use flower-of-life patterns, mandala-like symmetry, nested polygons, interlocking circles, radiant fractal forms, and precise luminous geometry that appears to reconstruct the scene. Keep the original image recognizable beneath the transformation, but make the geometry dominant, intricate, and visually distinct, as if the subject has been cut apart and remade into a spiritual geometric structure. Add layered patterns, translucent energy fields, and occasionally integrate a mysterious human figure, symbolic object, or spiritual presence blending into the sacred design. The result should feel visionary, anatomical, and metaphysical, with crisp geometry, luminous depth, and a strong sense of sacred order.)PROMPT",
        R"PROMPT(Transform the image into a sacred-geometric visionary artwork with flower-of-life patterns, mandala symmetry, layered luminous geometry, and integrated mystical forms while preserving the recognizable subject beneath the structure.)PROMPT",
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

const std::array<PromptPreset, 5>& prompt_presets() {
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
