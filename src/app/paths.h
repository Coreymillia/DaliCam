/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace app {

class Paths {
public:
    static constexpr std::string_view kAppName = "DaliCam";
    static constexpr std::string_view kInputDirectory = "input";
    static constexpr std::string_view kCameraDirectory = "camera";
    static constexpr std::string_view kUploadsDirectory = "uploads";
    static constexpr std::string_view kGeneratedDirectory = "generated";
    static constexpr std::string_view kTempDirectory = "temp";
    static constexpr std::string_view kLogsDirectory = "logs";
    static constexpr std::string_view kSettingsFilename = "settings.ini";

    static std::filesystem::path default_root();

    explicit Paths(std::filesystem::path root = default_root());

    const std::filesystem::path& root() const noexcept;

    std::filesystem::path settings_file() const;
    std::filesystem::path input_dir() const;
    std::filesystem::path camera_dir() const;
    std::filesystem::path uploads_dir() const;
    std::filesystem::path generated_dir() const;
    std::filesystem::path temp_dir() const;
    std::filesystem::path logs_dir() const;

    std::filesystem::path build_capture_path(std::string_view prefix = "capture",
                                            std::string_view extension = ".png") const;
    std::filesystem::path build_output_path(std::string_view prefix = "generated",
                                            std::string_view extension = ".png") const;

    bool ensure_directories(std::string& error) const;

private:
    std::filesystem::path root_;
};

} // namespace app
