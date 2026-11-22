/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace app {
class Settings;
}

namespace platform::webui {

class ImageViewerServer {
public:
    struct HttpRequest {
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    ImageViewerServer(std::filesystem::path camera_dir,
                      std::filesystem::path generated_dir,
                      std::filesystem::path uploads_dir,
                      std::filesystem::path settings_path,
                      uint16_t port = 8080);
    ~ImageViewerServer();

    ImageViewerServer(const ImageViewerServer&) = delete;
    ImageViewerServer& operator=(const ImageViewerServer&) = delete;

    bool start(std::string& error);
    void stop();

    bool running() const noexcept;
    uint16_t port() const noexcept;

private:
    struct ImageInfo {
        std::filesystem::path path;
        std::string filename;
        uintmax_t size_bytes{0};
        int width{-1};
        int height{-1};
        std::filesystem::file_time_type modified_at{};
    };

    enum class Source {
        Camera,
        Generated,
        Uploads,
    };

    struct LatestImage {
        Source source;
        ImageInfo image;
    };

    struct Route {
        Source source;
        bool raw{false};
        std::optional<std::string> filename;
    };

    std::filesystem::path source_dir(Source source) const;
    std::string source_label(Source source) const;
    std::vector<ImageInfo> list_images(Source source) const;
    std::optional<ImageInfo> latest_image(Source source) const;
    std::optional<ImageInfo> image_by_name(Source source, std::string_view filename) const;
    std::optional<LatestImage> latest_image_any() const;

    bool handle_client(int client_fd) const;
    void run();

    static bool read_request(int client_fd, HttpRequest& request, std::string& error);
    static std::string strip_query(std::string path);
    static std::optional<Route> parse_route(std::string_view path);
    static bool send_all(int client_fd, std::string_view data);
    static bool send_text(int client_fd,
                          int status_code,
                          std::string_view status_text,
                          std::string_view content_type,
                          std::string_view body);
    static bool send_file(int client_fd,
                          const std::filesystem::path& path,
                          std::string_view content_type,
                          std::string& error);
    static std::string html_escape(std::string_view text);
    static std::string content_type_for_path(const std::filesystem::path& path);
    static bool read_jpeg_dimensions(const std::filesystem::path& path,
                                     int& width,
                                     int& height,
                                     std::string& error);
    static bool read_png_dimensions(const std::filesystem::path& path,
                                    int& width,
                                    int& height,
                                    std::string& error);
    static std::string format_bytes(uintmax_t size);

    app::Settings load_settings_snapshot() const;
    std::string build_settings_page(const app::Settings& settings,
                                    const std::optional<LatestImage>& latest,
                                    std::optional<std::string> message) const;
    std::string build_page(Source source,
                           std::optional<std::string> selected_filename) const;

    std::filesystem::path camera_dir_;
    std::filesystem::path generated_dir_;
    std::filesystem::path uploads_dir_;
    std::filesystem::path settings_path_;
    uint16_t port_;
    int listen_fd_{-1};
    std::atomic_bool running_{false};
    std::atomic_bool stop_requested_{false};
    std::thread worker_;
};

} // namespace platform::webui
