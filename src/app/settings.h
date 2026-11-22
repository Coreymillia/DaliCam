/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "model_catalog.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace app {

class Settings {
public:
    std::string generation_provider = std::string(kDefaultGenerationProvider);
    std::string gemini_api_key;
    std::string gemini_model = std::string(kDefaultGeminiModel);
    std::string openai_api_key;
    std::string openai_model = std::string(kDefaultOpenAIModel);
    std::string default_personality = "dali-dream";
    bool allow_text_only_generation = true;
    bool save_source_images = true;
    bool save_generated_images = true;

    static bool is_supported_gemini_model(std::string_view model);
    static bool is_supported_openai_model(std::string_view model);
    static GenerationProvider provider_from_setting(std::string_view provider);
    static std::string_view provider_to_setting(GenerationProvider provider);

    bool load(const std::filesystem::path& path, std::string& error);
    bool save(const std::filesystem::path& path, std::string& error) const;
};

} // namespace app
