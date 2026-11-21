/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "paths.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace app {
namespace {

std::filesystem::path normalize_root(std::filesystem::path root) {
    if (root.empty()) {
        return std::filesystem::current_path() / Paths::kAppName;
    }
    if (root.is_relative()) {
        root = std::filesystem::current_path() / root;
    }
    return root.lexically_normal();
}

std::string timestamp_string() {
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local_time);
    return buffer;
}

std::string unique_suffix() {
    static std::atomic_uint64_t sequence{0};
    return std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::filesystem::path make_file_name(std::string_view prefix, std::string_view extension) {
    std::ostringstream stream;
    stream << prefix << '-' << timestamp_string() << '-' << unique_suffix() << extension;
    return stream.str();
}

} // namespace

std::filesystem::path Paths::default_root() {
    if (const char* override_root = std::getenv("DALICAM_STORAGE_ROOT")) {
        if (*override_root != '\0') {
            return normalize_root(override_root);
        }
    }

    if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME")) {
        if (*xdg_data_home != '\0') {
            return normalize_root(std::filesystem::path(xdg_data_home) / kAppName);
        }
    }

    if (const char* home = std::getenv("HOME")) {
        if (*home != '\0') {
            return normalize_root(std::filesystem::path(home) / ".local" / "share" / kAppName);
        }
    }

    return normalize_root(std::filesystem::current_path() / kAppName);
}

Paths::Paths(std::filesystem::path root)
    : root_(normalize_root(std::move(root)))
{
}

const std::filesystem::path& Paths::root() const noexcept {
    return root_;
}

std::filesystem::path Paths::settings_file() const {
    return root_ / kSettingsFilename;
}

std::filesystem::path Paths::input_dir() const {
    return root_ / kInputDirectory;
}

std::filesystem::path Paths::camera_dir() const {
    return input_dir() / kCameraDirectory;
}

std::filesystem::path Paths::uploads_dir() const {
    return input_dir() / kUploadsDirectory;
}

std::filesystem::path Paths::generated_dir() const {
    return root_ / kGeneratedDirectory;
}

std::filesystem::path Paths::temp_dir() const {
    return root_ / kTempDirectory;
}

std::filesystem::path Paths::logs_dir() const {
    return root_ / kLogsDirectory;
}

std::filesystem::path Paths::build_capture_path(std::string_view prefix,
                                                std::string_view extension) const {
    return camera_dir() / make_file_name(prefix, extension);
}

std::filesystem::path Paths::build_output_path(std::string_view prefix,
                                               std::string_view extension) const {
    return generated_dir() / make_file_name(prefix, extension);
}

bool Paths::ensure_directories(std::string& error) const {
    error.clear();

    const std::filesystem::path directories[] = {
        root_,
        input_dir(),
        camera_dir(),
        uploads_dir(),
        generated_dir(),
        temp_dir(),
        logs_dir(),
    };

    for (const auto& directory : directories) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(directory, filesystem_error);
        if (filesystem_error) {
            error = "failed to create directory '" + directory.string() + "': " +
                    filesystem_error.message();
            return false;
        }
    }

    return true;
}

} // namespace app
