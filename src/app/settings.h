/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace app {

class Settings {
public:
    static constexpr std::string_view kDefaultGeminiModel = "gemini-nano-banana-2.1";
    static constexpr std::array<std::string_view, 3> kSupportedGeminiModels = {{
        "gemini-nano-banana-2.1",
        "gemini-3.1-flash-lite-image",
        "gemini-3-pro-image",
    }};

    std::string gemini_api_key;
    std::string gemini_model = std::string(kDefaultGeminiModel);
    std::string default_personality = "dali-dream";
    bool allow_text_only_generation = true;
    bool save_source_images = true;
    bool save_generated_images = true;

    static bool is_supported_gemini_model(std::string_view model);

    bool load(const std::filesystem::path& path, std::string& error);
    bool save(const std::filesystem::path& path, std::string& error) const;
};

} // namespace app
