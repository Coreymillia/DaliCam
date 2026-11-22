/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"

#include "prompt_presets.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace app {
namespace {

std::string trim(std::string value) {
    const auto is_not_space = [](unsigned char character) {
        return !std::isspace(character);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

std::string normalize_provider(std::string value) {
    value = lowercase(trim(std::move(value)));
    if (value == "openai") {
        return std::string(provider_to_string(GenerationProvider::OpenAI));
    }
    return std::string(provider_to_string(GenerationProvider::Gemini));
}

std::string normalize_personality(std::string value) {
    value = lowercase(trim(std::move(value)));
    for (char& character : value) {
        if (character == ' ' || character == '_') {
            character = '-';
        }
    }

    if (value.find("dali") != std::string::npos) {
        return std::string(default_prompt_preset().id);
    }
    if (value.find("vector") != std::string::npos) {
        const auto* preset = find_prompt_preset("vector-sheet");
        if (preset) {
            return std::string(preset->id);
        }
    }
    if (const auto* preset = find_prompt_preset(value); preset) {
        return std::string(preset->id);
    }
    return std::string(default_prompt_preset().id);
}

bool parse_bool(const std::string& value, bool& result) {
    const auto normalized = lowercase(trim(value));
    if (normalized == "yes" || normalized == "true" || normalized == "1") {
        result = true;
        return true;
    }
    if (normalized == "no" || normalized == "false" || normalized == "0") {
        result = false;
        return true;
    }
    return false;
}

} // namespace

bool Settings::is_supported_gemini_model(std::string_view model) {
    return std::any_of(kGeminiModelOptions.begin(), kGeminiModelOptions.end(), [&](const auto& option) {
        return option.id == model;
    });
}

bool Settings::is_supported_openai_model(std::string_view model) {
    return std::any_of(kOpenAIModelOptions.begin(), kOpenAIModelOptions.end(), [&](const auto& option) {
        return option.id == model;
    });
}

GenerationProvider Settings::provider_from_setting(std::string_view provider) {
    return provider_from_string(provider);
}

std::string_view Settings::provider_to_setting(GenerationProvider provider) {
    return provider_to_string(provider);
}

bool Settings::load(const std::filesystem::path& path, std::string& error) {
    error.clear();

    std::ifstream input(path);
    if (!input) {
        error = "could not open settings file";
        return false;
    }

    std::string section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = lowercase(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const auto key = lowercase(trim(line.substr(0, separator)));
        const auto value = unquote(line.substr(separator + 1));

        if (section == "gemini") {
            if (key == "api_key") {
                gemini_api_key = value;
            }
            else if (key == "model") {
                gemini_model = value;
            }
        }
        else if (section == "openai") {
            if (key == "api_key") {
                openai_api_key = value;
            }
            else if (key == "model") {
                openai_model = value;
            }
        }
        else if (section == "generation") {
            if (key == "provider") {
                generation_provider = normalize_provider(value);
            }
            else if (key == "default_personality") {
                default_personality = normalize_personality(value);
            }
            else if (key == "allow_text_only_generation") {
                if (!parse_bool(value, allow_text_only_generation)) {
                    error = "invalid allow_text_only_generation value at line " +
                            std::to_string(line_number);
                    return false;
                }
            }
        }
        else if (section == "storage") {
            if (key == "save_source_images") {
                if (!parse_bool(value, save_source_images)) {
                    error = "invalid save_source_images value at line " +
                            std::to_string(line_number);
                    return false;
                }
            }
            else if (key == "save_generated_images") {
                if (!parse_bool(value, save_generated_images)) {
                    error = "invalid save_generated_images value at line " +
                            std::to_string(line_number);
                    return false;
                }
            }
        }
    }

    if (!is_supported_gemini_model(gemini_model)) {
        gemini_model = std::string(kDefaultGeminiModel);
    }
    if (!is_supported_openai_model(openai_model)) {
        openai_model = std::string(kDefaultOpenAIModel);
    }

    generation_provider = normalize_provider(generation_provider);
    default_personality = normalize_personality(default_personality);

    return true;
}

bool Settings::save(const std::filesystem::path& path, std::string& error) const {
    error.clear();

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create parent directory: " + filesystem_error.message();
            return false;
        }
    }

    const std::string gemini_model_to_save = is_supported_gemini_model(gemini_model)
                                                 ? gemini_model
                                                 : std::string(kDefaultGeminiModel);
    const std::string openai_model_to_save = is_supported_openai_model(openai_model)
                                                 ? openai_model
                                                 : std::string(kDefaultOpenAIModel);

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "could not open settings file for writing";
        return false;
    }

    output << "[gemini]\n";
    output << "api_key=" << gemini_api_key << '\n';
    output << "model=" << gemini_model_to_save << "\n\n";

    output << "[openai]\n";
    output << "api_key=" << openai_api_key << '\n';
    output << "model=" << openai_model_to_save << "\n\n";

    output << "[generation]\n";
    output << "provider=" << normalize_provider(generation_provider) << '\n';
    output << "default_personality=" << normalize_personality(default_personality) << '\n';
    output << "allow_text_only_generation=" << (allow_text_only_generation ? "yes" : "no")
           << "\n\n";

    output << "[storage]\n";
    output << "save_source_images=" << (save_source_images ? "yes" : "no") << '\n';
    output << "save_generated_images=" << (save_generated_images ? "yes" : "no") << '\n';

    if (!output) {
        error = "failed while writing settings file";
        return false;
    }

    return true;
}

} // namespace app
