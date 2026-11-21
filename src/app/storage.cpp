/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "storage.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace app {

Storage::Storage(Paths paths)
    : paths_(std::move(paths))
{
}

const Paths& Storage::paths() const noexcept {
    return paths_;
}

bool Storage::prepare(std::string& error) const {
    return paths_.ensure_directories(error);
}

bool Storage::write_text_file(const std::filesystem::path& file,
                              std::string_view content,
                              std::string& error) const {
    error.clear();

    const auto parent = file.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create parent directory: " + filesystem_error.message();
            return false;
        }
    }

    std::ofstream output(file, std::ios::trunc);
    if (!output) {
        error = "could not open file for writing";
        return false;
    }

    output << content;
    if (!output) {
        error = "failed while writing file";
        return false;
    }

    return true;
}

bool Storage::read_text_file(const std::filesystem::path& file,
                             std::string& content,
                             std::string& error) const {
    error.clear();
    std::ifstream input(file);
    if (!input) {
        error = "could not open file for reading";
        return false;
    }

    std::ostringstream stream;
    stream << input.rdbuf();
    if (!input && !input.eof()) {
        error = "failed while reading file";
        return false;
    }

    content = stream.str();
    return true;
}

bool Storage::copy_file(const std::filesystem::path& source,
                        const std::filesystem::path& destination,
                        std::string& error) const {
    error.clear();

    if (source == destination) {
        return true;
    }

    const auto parent = destination.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create destination directory: " + filesystem_error.message();
            return false;
        }
    }

    std::error_code filesystem_error;
    std::filesystem::copy_file(source,
                               destination,
                               std::filesystem::copy_options::overwrite_existing,
                               filesystem_error);
    if (filesystem_error) {
        error = "could not copy file: " + filesystem_error.message();
        return false;
    }

    return true;
}

} // namespace app
