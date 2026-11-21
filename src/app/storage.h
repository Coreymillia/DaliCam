/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "paths.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace app {

class Storage {
public:
    explicit Storage(Paths paths);

    const Paths& paths() const noexcept;

    bool prepare(std::string& error) const;
    bool write_text_file(const std::filesystem::path& file,
                         std::string_view content,
                         std::string& error) const;
    bool read_text_file(const std::filesystem::path& file,
                        std::string& content,
                        std::string& error) const;
    bool copy_file(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::string& error) const;

private:
    Paths paths_;
};

} // namespace app
