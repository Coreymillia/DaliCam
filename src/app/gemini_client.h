/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace app {

struct ImageGenerationResult {
    bool ok{false};
    std::string message;
    std::filesystem::path response_path;
    std::filesystem::path error_path;
    std::filesystem::path image_path;
};

using GeminiImageResult = ImageGenerationResult;

GeminiImageResult generate_gemini_image_request(std::string_view api_key,
                                                std::string_view model,
                                                std::string_view prompt,
                                                const std::filesystem::path& output_path,
                                                const std::filesystem::path& temp_dir,
                                                const std::optional<std::filesystem::path>& source_image_path = std::nullopt);

GeminiImageResult test_gemini_image_request(std::string_view api_key,
                                           std::string_view model,
                                           const std::filesystem::path& output_path,
                                           const std::filesystem::path& temp_dir);

} // namespace app
