/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    return std::find(kSupportedGeminiModels.begin(), kSupportedGeminiModels.end(), model) !=
           kSupportedGeminiModels.end();
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
        else if (section == "generation") {
            if (key == "default_personality") {
                default_personality = lowercase(value);
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

    const std::string model_to_save = is_supported_gemini_model(gemini_model)
                                         ? gemini_model
                                         : std::string(kDefaultGeminiModel);

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "could not open settings file for writing";
        return false;
    }

    output << "[gemini]\n";
    output << "api_key=" << gemini_api_key << '\n';
    output << "model=" << model_to_save << "\n\n";

    output << "[generation]\n";
    output << "default_personality=" << default_personality << '\n';
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
