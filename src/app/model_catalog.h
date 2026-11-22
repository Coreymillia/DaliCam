/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <string_view>

namespace app {

enum class GenerationProvider {
    Gemini,
    OpenAI,
};

struct ModelOption {
    std::string_view id;
    std::string_view label;
};

inline constexpr std::string_view kDefaultGeminiModel = "gemini-nano-banana-2.1";
inline constexpr std::string_view kDefaultOpenAIModel = "gpt-image-2.5-flare";
inline constexpr std::string_view kDefaultGenerationProvider = "gemini";

inline constexpr std::array<ModelOption, 3> kGeminiModelOptions = {{
    {"gemini-nano-banana-2.1", "Nano Banana 2.1"},
    {"gemini-3.1-flash-lite-image", "3.1 Flash Lite"},
    {"gemini-3-pro-image", "3 Pro Image"},
}};

inline constexpr std::array<ModelOption, 4> kOpenAIModelOptions = {{
    {"gpt-image-2.5-flare", "2.5 Flare"},
    {"gpt-image-2.5-sunburst", "2.5 Sunburst"},
    {"gpt-image-2.6-flare", "2.6 Flare"},
    {"gpt-image-2.6-sunburst", "2.6 Sunburst"},
}};

inline constexpr std::string_view provider_to_string(GenerationProvider provider) {
    switch (provider) {
        case GenerationProvider::Gemini:
            return "gemini";
        case GenerationProvider::OpenAI:
            return "openai";
    }
    return kDefaultGenerationProvider;
}

inline constexpr GenerationProvider provider_from_string(std::string_view provider) {
    return (provider == "openai") ? GenerationProvider::OpenAI : GenerationProvider::Gemini;
}

inline constexpr std::string_view provider_label(GenerationProvider provider) {
    switch (provider) {
        case GenerationProvider::Gemini:
            return "Gemini";
        case GenerationProvider::OpenAI:
            return "OpenAI";
    }
    return "Gemini";
}

inline constexpr const std::array<ModelOption, 3>& supported_gemini_models() {
    return kGeminiModelOptions;
}

inline constexpr const std::array<ModelOption, 4>& supported_openai_models() {
    return kOpenAIModelOptions;
}

inline constexpr const ModelOption* find_model_option(GenerationProvider provider, std::string_view model) {
    if (provider == GenerationProvider::OpenAI) {
        for (const auto& option : kOpenAIModelOptions) {
            if (option.id == model) {
                return &option;
            }
        }
        return nullptr;
    }

    for (const auto& option : kGeminiModelOptions) {
        if (option.id == model) {
            return &option;
        }
    }
    return nullptr;
}

inline constexpr bool is_supported_model(GenerationProvider provider, std::string_view model) {
    return find_model_option(provider, model) != nullptr;
}

inline constexpr std::string_view default_model_for(GenerationProvider provider) {
    return provider == GenerationProvider::OpenAI ? kDefaultOpenAIModel : kDefaultGeminiModel;
}

} // namespace app
