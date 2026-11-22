/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "gemini_client.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace app {

ImageGenerationResult generate_openai_image_request(std::string_view api_key,
                                                    std::string_view model,
                                                    std::string_view prompt,
                                                    const std::filesystem::path& output_path,
                                                    const std::filesystem::path& temp_dir,
                                                    const std::optional<std::filesystem::path>& source_image_path = std::nullopt);

} // namespace app
