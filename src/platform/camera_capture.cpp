/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "camera_capture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/wait.h>

namespace platform::camera {
namespace {

std::string shell_quote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char character : value) {
        if (character == '\'') {
            quoted.append("'\\''");
        }
        else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

int system_exit_code(const std::string& command) {
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
#if defined(_WIN32)
    return status;
#else
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
#endif
}

} // namespace

CaptureResult capture_photo(const std::filesystem::path& output_path) {
    CaptureResult result;
    result.path = output_path.string();

    const auto parent = output_path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            result.error = "Unable to create camera directory: " + filesystem_error.message();
            return result;
        }
    }

    std::error_code remove_error;
    std::filesystem::remove(output_path, remove_error);

    const auto command =
        "rpicam-still --nopreview --timeout 2s --rotation 180 --encoding jpg --quality 95 "
        "-o " + shell_quote(output_path.string());

    const int exit_code = system_exit_code(command);
    if (exit_code != 0) {
        result.error = "rpicam-still failed with code " + std::to_string(exit_code);
        return result;
    }

    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(output_path, file_error);
    if (file_error || file_size == 0) {
        result.error = "Camera capture produced an empty file";
        return result;
    }

    result.success = true;
    return result;
}

} // namespace platform::camera
