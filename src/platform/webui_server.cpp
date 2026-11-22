/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "webui_server.h"

#include "camera_capture.h"
#include "../app/gemini_client.h"
#include "../app/prompt_presets.h"
#include "../app/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace platform::webui {
namespace {

constexpr std::size_t kRequestLimit = 32 * 1024 * 1024;
constexpr std::size_t kBufferSize = 8 * 1024;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string url_decode(std::string_view text) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '+') {
            output.push_back(' ');
            continue;
        }
        if (ch == '%' && index + 2 < text.size()) {
            const auto hex = text.substr(index + 1, 2);
            char* end = nullptr;
            const auto value = static_cast<char>(std::strtol(std::string(hex).c_str(), &end, 16));
            if (end != nullptr && *end == '\0') {
                output.push_back(value);
                index += 2;
                continue;
            }
        }
        output.push_back(ch);
    }

    return output;
}

std::unordered_map<std::string, std::string> parse_urlencoded_form(std::string_view body) {
    std::unordered_map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto amp = body.find('&', start);
        const auto token = body.substr(start, amp == std::string_view::npos ? body.size() - start : amp - start);
        if (!token.empty()) {
            const auto equals = token.find('=');
            if (equals != std::string_view::npos) {
                const auto key = url_decode(token.substr(0, equals));
                const auto value = url_decode(token.substr(equals + 1));
                values.emplace(std::move(key), std::move(value));
            }
        }

        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }
    return values;
}

std::optional<std::string> header_value(const ImageViewerServer::HttpRequest& request, std::string_view key) {
    const auto lowered = lowercase(std::string(key));
    for (const auto& [name, value] : request.headers) {
        if (lowercase(name) == lowered) {
            return value;
        }
    }
    return std::nullopt;
}

bool is_jpeg_sof_marker(unsigned char marker) {
    switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

std::string escape_html(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        case '\'': output += "&#39;"; break;
        default: output.push_back(ch); break;
        }
    }
    return output;
}

std::string bool_option(bool value) {
    return value ? "yes" : "no";
}

std::string select_option(std::string_view value, std::string_view current) {
    if (value == current) {
        return " selected";
    }
    return "";
}

std::string message_box(std::string_view title, std::string_view message, bool success) {
    std::ostringstream out;
    out << "<div class=\"message " << (success ? "ok" : "error") << "\">"
        << "<strong>" << escape_html(title) << "</strong>"
        << "<div>" << escape_html(message) << "</div>"
        << "</div>";
    return out.str();
}

std::string unique_upload_name(std::string_view prefix, std::string_view extension) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif

    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time);

    std::ostringstream out;
    out << prefix << '-' << timestamp << '-' << sequence.fetch_add(1, std::memory_order_relaxed) << extension;
    return out.str();
}

std::tm local_tm_from_file_time(std::filesystem::file_time_type time_point) {
    using namespace std::chrono;
    const auto now_system = system_clock::now();
    const auto now_file = std::filesystem::file_time_type::clock::now();
    const auto system_time = time_point_cast<system_clock::duration>(time_point - now_file + now_system);
    const auto system_time_t = system_clock::to_time_t(system_time);

    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &system_time_t);
#else
    localtime_r(&system_time_t, &local_time);
#endif
    return local_time;
}

std::string format_day_label(std::filesystem::file_time_type time_point) {
    const auto local_time = local_tm_from_file_time(time_point);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_time);
    return buffer;
}

std::string format_time_label(std::filesystem::file_time_type time_point) {
    const auto local_time = local_tm_from_file_time(time_point);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local_time);
    return buffer;
}

std::string url_encode(std::string_view value) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        }
        else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

bool string_ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::string upload_extension(std::string_view content_type, std::string_view filename) {
    const auto lowered_content_type = lowercase(std::string(content_type));
    const auto lowered_filename = lowercase(std::string(filename));

    if (lowered_content_type.find("png") != std::string::npos ||
        string_ends_with(lowered_filename, ".png")) {
        return ".png";
    }
    if (lowered_content_type.find("jpeg") != std::string::npos ||
        lowered_content_type.find("jpg") != std::string::npos ||
        string_ends_with(lowered_filename, ".jpg") ||
        string_ends_with(lowered_filename, ".jpeg")) {
        return ".jpg";
    }
    return {};
}

bool socket_send_all(int client_fd, std::string_view data) {
    std::size_t sent_total = 0;
    while (sent_total < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t sent = ::send(client_fd,
                                    data.data() + sent_total,
                                    data.size() - sent_total,
                                    MSG_NOSIGNAL);
#else
        const ssize_t sent = ::send(client_fd,
                                    data.data() + sent_total,
                                    data.size() - sent_total,
                                    0);
#endif
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string http_status_text(int status_code) {
    switch (status_code) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

std::string make_http_response(int status_code,
                               std::string_view content_type,
                               std::string_view body,
                               std::string_view extra_headers = {}) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << ' ' << http_status_text(status_code) << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n";
    out << "Pragma: no-cache\r\n";
    out << "Connection: close\r\n";
    if (!extra_headers.empty()) {
        out << extra_headers;
        if (extra_headers.back() != '\n') {
            out << "\r\n";
        }
    }
    out << "\r\n";
    out << body;
    return out.str();
}

std::string make_error_page(int status_code, std::string_view title, std::string_view message) {
    std::ostringstream out;
    out << "<!doctype html><html><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        << "<title>DaliCam Web UI</title>"
        << "<style>"
        << ":root{color-scheme:dark;}"
        << "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:24px;}"
        << "a{color:#9fd3ff;}"
        << "</style></head><body>"
        << "<h1>" << status_code << ' ' << escape_html(title) << "</h1>"
        << "<p>" << escape_html(message) << "</p>"
        << "<p><a href=\"/\">Back to home</a> | <a href=\"/camera\">Open camera viewer</a></p>"
        << "</body></html>";
    return out.str();
}

} // namespace

ImageViewerServer::ImageViewerServer(std::filesystem::path camera_dir,
                                     std::filesystem::path generated_dir,
                                     std::filesystem::path uploads_dir,
                                     std::filesystem::path settings_path,
                                     uint16_t port)
    : camera_dir_(std::move(camera_dir)),
      generated_dir_(std::move(generated_dir)),
      uploads_dir_(std::move(uploads_dir)),
      settings_path_(std::move(settings_path)),
      port_(port) {
}

ImageViewerServer::~ImageViewerServer() {
    stop();
}

bool ImageViewerServer::start(std::string& error) {
    error.clear();
    if (running_.load()) {
        error = "web server is already running";
        return false;
    }

    stop_requested_.store(false);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error = "socket() failed: " + std::string(std::strerror(errno));
        return false;
    }

    int reuse_address = 1;
    (void) ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        error = "bind() failed on port " + std::to_string(port_) + ": " + std::string(std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 4) < 0) {
        error = "listen() failed: " + std::string(std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    try {
        worker_ = std::thread(&ImageViewerServer::run, this);
    }
    catch (...) {
        error = "failed to start web server thread";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    return true;
}

void ImageViewerServer::stop() {
    stop_requested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

bool ImageViewerServer::running() const noexcept {
    return running_.load();
}

uint16_t ImageViewerServer::port() const noexcept {
    return port_;
}

std::filesystem::path ImageViewerServer::source_dir(Source source) const {
    switch (source) {
    case Source::Camera:
        return camera_dir_;
    case Source::Generated:
        return generated_dir_;
    case Source::Uploads:
        return uploads_dir_;
    }
    return camera_dir_;
}

std::string ImageViewerServer::source_label(Source source) const {
    switch (source) {
    case Source::Camera:
        return "Camera";
    case Source::Generated:
        return "Generated";
    case Source::Uploads:
        return "Uploads";
    }
    return "Camera";
}

std::vector<ImageViewerServer::ImageInfo> ImageViewerServer::list_images(Source source) const {
    const auto directory = source_dir(source);
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return {};
    }

    std::vector<ImageInfo> images;
    for (std::filesystem::directory_iterator it(directory, error); !error && it != std::filesystem::directory_iterator(); ++it) {
        const auto& entry = *it;
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }

        const auto ext = lowercase(entry.path().extension().string());
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") {
            continue;
        }

        const auto write_time = entry.last_write_time(entry_error);
        if (entry_error) {
            continue;
        }

        ImageInfo info;
        info.path = entry.path();
        info.filename = entry.path().filename().string();
        info.size_bytes = std::filesystem::file_size(entry.path(), entry_error);
        if (entry_error) {
            continue;
        }

        if (ext == ".jpg" || ext == ".jpeg") {
            int width = -1;
            int height = -1;
            std::string dimension_error;
            if (read_jpeg_dimensions(entry.path(), width, height, dimension_error)) {
                info.width = width;
                info.height = height;
            }
        } else if (ext == ".png") {
            int width = -1;
            int height = -1;
            std::string dimension_error;
            if (read_png_dimensions(entry.path(), width, height, dimension_error)) {
                info.width = width;
                info.height = height;
            }
        }

        info.modified_at = write_time;
        images.push_back(std::move(info));
    }

    std::sort(images.begin(), images.end(), [](const ImageInfo& a, const ImageInfo& b) {
        return a.modified_at > b.modified_at;
    });
    return images;
}

std::optional<ImageViewerServer::ImageInfo> ImageViewerServer::latest_image(Source source) const {
    const auto images = list_images(source);
    if (images.empty()) {
        return std::nullopt;
    }
    return images.front();
}

std::optional<ImageViewerServer::ImageInfo> ImageViewerServer::image_by_name(Source source, std::string_view filename) const {
    if (filename.empty()) {
        return std::nullopt;
    }

    const auto images = list_images(source);
    const auto target = std::filesystem::path(filename).filename();
    for (const auto& image : images) {
        if (image.filename == target.string()) {
            return image;
        }
    }
    return std::nullopt;
}

std::optional<ImageViewerServer::LatestImage> ImageViewerServer::latest_image_any() const {
    std::optional<LatestImage> latest;

    for (const auto source : {Source::Camera, Source::Generated, Source::Uploads}) {
        const auto image = latest_image(source);
        if (!image) {
            continue;
        }
        if (!latest || image->modified_at > latest->image.modified_at) {
            latest = LatestImage{source, *image};
        }
    }

    return latest;
}

bool ImageViewerServer::handle_client(int client_fd) const {
    HttpRequest request;
    std::string error;
    if (!read_request(client_fd, request, error)) {
        const auto body = make_error_page(400, "Bad Request", error.empty() ? "failed to read request" : error);
        return send_text(client_fd, 400, "Bad Request", "text/html; charset=utf-8", body);
    }

    const auto query_pos = request.path.find('?');
    const std::string path = strip_query(request.path);
    const auto query_fields = query_pos == std::string::npos
                                  ? std::unordered_map<std::string, std::string>{}
                                  : parse_urlencoded_form(std::string_view(request.path).substr(query_pos + 1));

    if (request.method == "GET" && (path == "/" || path == "/settings")) {
        const auto settings = load_settings_snapshot();
        std::optional<LatestImage> current = latest_image_any();
        if (const auto source_it = query_fields.find("source"); source_it != query_fields.end()) {
            if (const auto filename_it = query_fields.find("file"); filename_it != query_fields.end() && !filename_it->second.empty()) {
                std::optional<Source> source;
                const auto lowered_source = lowercase(source_it->second);
                if (lowered_source == "camera") {
                    source = Source::Camera;
                }
                else if (lowered_source == "generated") {
                    source = Source::Generated;
                }
                else if (lowered_source == "uploads") {
                    source = Source::Uploads;
                }
                if (source) {
                    if (const auto image = image_by_name(*source, filename_it->second)) {
                        current = LatestImage{*source, *image};
                    }
                }
            }
        }
        const auto body = build_settings_page(settings, current, std::nullopt);
        return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    }

    if (request.method == "POST" && path == "/upload") {
        const auto content_type = header_value(request, "Content-Type").value_or("");
        const auto filename = header_value(request, "X-Filename").value_or("");
        const auto extension = upload_extension(content_type, filename);
        if (extension.empty()) {
            const auto body = make_error_page(415, "Unsupported Media Type", "Please upload a PNG or JPEG image.");
            return send_text(client_fd, 415, "Unsupported Media Type", "text/html; charset=utf-8", body);
        }

        std::error_code filesystem_error;
        std::filesystem::create_directories(uploads_dir_, filesystem_error);
        if (filesystem_error) {
            const auto body = make_error_page(500, "Internal Server Error", filesystem_error.message());
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto upload_path = uploads_dir_ / unique_upload_name("upload", extension);
        std::ofstream file(upload_path, std::ios::binary);
        if (!file) {
            const auto body = make_error_page(500, "Internal Server Error", "Unable to create upload file");
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }
        file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
        if (!file) {
            const auto body = make_error_page(500, "Internal Server Error", "Unable to write upload file");
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }
        file.flush();
        file.close();

        const auto settings = load_settings_snapshot();
        const auto body = build_settings_page(settings,
                                              latest_image_any(),
                                              message_box("Upload saved",
                                                          "The image was stored and should appear in the preview shortly.",
                                                          true));
        return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    }

    if (request.method == "POST" && path == "/capture") {
        app::Settings settings = load_settings_snapshot();
        std::error_code filesystem_error;
        std::filesystem::create_directories(camera_dir_, filesystem_error);
        if (filesystem_error) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Capture failed", filesystem_error.message(), false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto capture_path = camera_dir_ / unique_upload_name("capture", ".jpg");
        const auto result = platform::camera::capture_photo(capture_path);
        if (!result.success) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Capture failed",
                                                              result.error.empty() ? "The camera could not capture a photo." : result.error,
                                                              false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto body = build_settings_page(settings,
                                              latest_image_any(),
                                              message_box("Photo captured",
                                                          "The new photo was saved and should appear in the gallery shortly.",
                                                          true));
        return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    }

    if (request.method == "POST" && path == "/generate") {
        auto settings = load_settings_snapshot();
        const auto fields = parse_urlencoded_form(request.body);
        const auto prompt_it = fields.find("prompt");
        const std::string user_prompt = prompt_it != fields.end() ? prompt_it->second : std::string();

        const auto* preset = app::find_prompt_preset(settings.default_personality);
        const auto& selected_preset = preset ? *preset : app::default_prompt_preset();
        const std::string prompt = app::build_prompt(selected_preset, user_prompt);

        std::optional<std::filesystem::path> source_image_path;
        if (const auto source_it = fields.find("source"); source_it != fields.end()) {
            if (const auto filename_it = fields.find("source_filename"); filename_it != fields.end() && !filename_it->second.empty()) {
                std::optional<Source> source;
                const auto lowered_source = lowercase(source_it->second);
                if (lowered_source == "camera") {
                    source = Source::Camera;
                }
                else if (lowered_source == "generated") {
                    source = Source::Generated;
                }
                else if (lowered_source == "uploads") {
                    source = Source::Uploads;
                }

                if (source) {
                    if (const auto image = image_by_name(*source, filename_it->second)) {
                        source_image_path = image->path;
                    }
                }
            }
        }

        if (!source_image_path && !settings.allow_text_only_generation) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Generation blocked",
                                                              "Text-only generation is disabled, and no source image was selected.",
                                                              false));
            return send_text(client_fd, 400, "Bad Request", "text/html; charset=utf-8", body);
        }

        if (settings.gemini_api_key.empty()) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Generation blocked",
                                                              "Set the Gemini API key first.",
                                                              false));
            return send_text(client_fd, 400, "Bad Request", "text/html; charset=utf-8", body);
        }

        std::error_code filesystem_error;
        std::filesystem::create_directories(generated_dir_, filesystem_error);
        if (filesystem_error) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Generation failed", filesystem_error.message(), false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto temp_dir = std::filesystem::temp_directory_path() / "dalicam-webui";
        std::filesystem::create_directories(temp_dir, filesystem_error);
        if (filesystem_error) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Generation failed", filesystem_error.message(), false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto output_path = generated_dir_ / unique_upload_name("webui-gemini", ".jpg");
        const auto result = app::generate_gemini_image_request(settings.gemini_api_key,
                                                               settings.gemini_model,
                                                               prompt,
                                                               output_path,
                                                               temp_dir,
                                                               source_image_path);
        if (!result.ok) {
            const auto body = build_settings_page(settings,
                                                  latest_image_any(),
                                                  message_box("Generation failed",
                                                              result.message.empty() ? "The image request did not complete." : result.message,
                                                              false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto body = build_settings_page(settings,
                                              latest_image_any(),
                                              message_box("Image generated",
                                                          "The new image was saved and should appear in the gallery shortly.",
                                                          true));
        return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    }

    if (request.method == "POST" && (path == "/" || path == "/settings")) {
        auto settings = load_settings_snapshot();
        const auto fields = parse_urlencoded_form(request.body);
        if (const auto it = fields.find("api_key"); it != fields.end() && !it->second.empty()) {
            settings.gemini_api_key = it->second;
        }
        if (const auto it = fields.find("gemini_model"); it != fields.end()) {
            settings.gemini_model = it->second;
        }
        if (const auto it = fields.find("default_personality"); it != fields.end()) {
            settings.default_personality = it->second;
        }
        if (const auto it = fields.find("allow_text_only_generation"); it != fields.end()) {
            settings.allow_text_only_generation = it->second == "yes";
        }
        if (const auto it = fields.find("save_source_images"); it != fields.end()) {
            settings.save_source_images = it->second == "yes";
        }
        if (const auto it = fields.find("save_generated_images"); it != fields.end()) {
            settings.save_generated_images = it->second == "yes";
        }

        std::string save_error;
        if (!settings.save(settings_path_, save_error)) {
            const auto body = build_settings_page(settings, latest_image_any(), message_box("Save failed", save_error, false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto body = build_settings_page(settings,
                                              latest_image_any(),
                                              message_box("Settings saved", "Your settings were written to disk.", true));
        return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    }

    const auto route = parse_route(path);
    if (!route) {
        const auto body = make_error_page(404, "Not Found", "The requested page does not exist");
        return send_text(client_fd, 404, "Not Found", "text/html; charset=utf-8", body);
    }

    if (route->raw || route->download) {
        const auto image = route->filename ? image_by_name(route->source, *route->filename)
                                           : latest_image(route->source);
        if (!image) {
            const auto body = make_error_page(404, "Not Found", "No image is available yet");
            return send_text(client_fd, 404, "Not Found", "text/html; charset=utf-8", body);
        }

        std::string file_error;
        if (route->download) {
            const auto headers = std::string("Content-Disposition: attachment; filename=\"") + image->filename + "\"\r\n";
            return send_file(client_fd, image->path, content_type_for_path(image->path), file_error, headers);
        }
        return send_file(client_fd, image->path, content_type_for_path(image->path), file_error);
    }

    if (route->delete_request) {
        if (request.method != "POST") {
            const auto body = make_error_page(405, "Method Not Allowed", "Delete requires a POST request.");
            return send_text(client_fd, 405, "Method Not Allowed", "text/html; charset=utf-8", body);
        }

        const auto image = route->filename ? image_by_name(route->source, *route->filename) : std::optional<ImageInfo>{};
        if (!image) {
            const auto body = build_page(route->source,
                                         std::nullopt,
                                         message_box("Delete failed", "No matching image was found.", false));
            return send_text(client_fd, 404, "Not Found", "text/html; charset=utf-8", body);
        }

        std::error_code remove_error;
        const auto removed = std::filesystem::remove(image->path, remove_error);
        if (!removed || remove_error) {
            const auto body = build_page(route->source,
                                         std::nullopt,
                                         message_box("Delete failed",
                                                     remove_error ? remove_error.message() : "The file could not be deleted.",
                                                     false));
            return send_text(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8", body);
        }

        const auto body = build_page(route->source,
                                     std::nullopt,
                                     message_box("Image deleted",
                                                 "The image was removed from storage.",
                                                 true));
        return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    }

    const auto body = build_page(route->source, route->filename);
    return send_text(client_fd, 200, "OK", "text/html; charset=utf-8", body);
}

void ImageViewerServer::run() {
    while (!stop_requested_.load()) {
        pollfd fd{};
        fd.fd = listen_fd_;
        fd.events = POLLIN;

        const int result = ::poll(&fd, 1, 250);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }
        if (result == 0) {
            continue;
        }

        if (fd.revents & POLLIN) {
            sockaddr_storage client_address{};
            socklen_t length = sizeof(client_address);
            const int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &length);
            if (client_fd < 0) {
                continue;
            }

            (void) handle_client(client_fd);
            ::close(client_fd);
        }
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    running_.store(false);
}

bool ImageViewerServer::read_request(int client_fd, ImageViewerServer::HttpRequest& request, std::string& error) {
    request = {};
    error.clear();

    std::string raw;
    raw.reserve(1024);
    std::array<char, kBufferSize> buffer{};
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    while (raw.size() < kRequestLimit) {
        const ssize_t received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("recv() failed: ") + std::strerror(errno);
            return false;
        }
        if (received == 0) {
            break;
        }

        raw.append(buffer.data(), static_cast<std::size_t>(received));
        header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
    }

    if (header_end == std::string::npos) {
        error = "request headers not complete";
        return false;
    }

    const auto header_blob = raw.substr(0, header_end);
    std::istringstream header_stream(header_blob);
    std::string line;
    if (!std::getline(header_stream, line)) {
        error = "request line missing";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    {
        std::istringstream request_line(line);
        request_line >> request.method >> request.path;
        std::string version;
        request_line >> version;
        if (request.method.empty() || request.path.empty()) {
            error = "invalid request line";
            return false;
        }
    }

    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto name = trim(line.substr(0, colon));
        auto value = trim(line.substr(colon + 1));
        if (!name.empty()) {
            request.headers.emplace(std::move(name), std::move(value));
        }
    }

    if (const auto value = header_value(request, "Content-Length"); value) {
        try {
            content_length = static_cast<std::size_t>(std::stoul(*value));
        }
        catch (...) {
            error = "invalid Content-Length header";
            return false;
        }
    }

    const std::size_t body_offset = header_end + 4;
    if (raw.size() > body_offset) {
        request.body = raw.substr(body_offset);
    }

    while (request.body.size() < content_length && raw.size() < kRequestLimit) {
        const ssize_t received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("recv() failed: ") + std::strerror(errno);
            return false;
        }
        if (received == 0) {
            break;
        }
        request.body.append(buffer.data(), static_cast<std::size_t>(received));
    }

    if (request.body.size() < content_length) {
        error = "request body incomplete";
        return false;
    }

    return true;
}

std::string ImageViewerServer::strip_query(std::string path) {
    const auto query = path.find('?');
    if (query != std::string::npos) {
        path.erase(query);
    }

    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    return path;
}

std::optional<ImageViewerServer::Route> ImageViewerServer::parse_route(std::string_view path) {
    const auto make_route = [&](Source source,
                                bool raw,
                                bool download,
                                bool delete_request,
                                std::optional<std::string> filename = std::nullopt) {
        return Route{source, raw, download, delete_request, std::move(filename)};
    };

    const auto match_prefix = [&](std::string_view prefix, Source source) -> std::optional<Route> {
        if (path == prefix) {
            return make_route(source, false, false, false);
        }
        if (path == std::string(prefix) + "/raw") {
            return make_route(source, true, false, false);
        }
        if (path.rfind(std::string(prefix) + "/raw/", 0) == 0) {
            return make_route(source, true, false, false, std::string(path.substr(prefix.size() + 5)));
        }
        if (path.rfind(std::string(prefix) + "/download/", 0) == 0) {
            return make_route(source, false, true, false, std::string(path.substr(prefix.size() + 10)));
        }
        if (path.rfind(std::string(prefix) + "/delete/", 0) == 0) {
            return make_route(source, false, false, true, std::string(path.substr(prefix.size() + 8)));
        }
        if (path.rfind(std::string(prefix) + "/", 0) == 0) {
            return make_route(source, false, false, false, std::string(path.substr(prefix.size() + 1)));
        }
        return std::nullopt;
    };

    if (const auto route = match_prefix("/camera", Source::Camera)) {
        return route;
    }
    if (const auto route = match_prefix("/generated", Source::Generated)) {
        return route;
    }
    if (const auto route = match_prefix("/uploads", Source::Uploads)) {
        return route;
    }
    return std::nullopt;
}

bool ImageViewerServer::send_all(int client_fd, std::string_view data) {
    return socket_send_all(client_fd, data);
}

bool ImageViewerServer::send_text(int client_fd,
                                  int status_code,
                                  std::string_view status_text,
                                  std::string_view content_type,
                                  std::string_view body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
        << "Pragma: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    const auto response = out.str();
    return socket_send_all(client_fd, response);
}

bool ImageViewerServer::send_file(int client_fd,
                                  const std::filesystem::path& path,
                                  std::string_view content_type,
                                  std::string& error,
                                  std::string_view extra_headers) {
    error.clear();

    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }

    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << size << "\r\n"
           << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
           << "Pragma: no-cache\r\n"
           << "Connection: close\r\n";
    if (!extra_headers.empty()) {
        header << extra_headers;
        if (extra_headers.back() != '\n') {
            header << "\r\n";
        }
    }
    header << "\r\n";
    const auto header_text = header.str();
    if (!socket_send_all(client_fd, header_text)) {
        error = "failed to send file header";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "failed to open image file";
        return false;
    }

    std::array<char, kBufferSize> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0) {
            if (!socket_send_all(client_fd, std::string_view(buffer.data(), static_cast<std::size_t>(count)))) {
                error = "failed to send file body";
                return false;
            }
        }
    }

    return true;
}

std::string ImageViewerServer::html_escape(std::string_view text) {
    return escape_html(text);
}

std::string ImageViewerServer::content_type_for_path(const std::filesystem::path& path) {
    const auto ext = lowercase(path.extension().string());
    if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    }
    if (ext == ".png") {
        return "image/png";
    }
    return "application/octet-stream";
}

bool ImageViewerServer::read_jpeg_dimensions(const std::filesystem::path& path,
                                             int& width,
                                             int& height,
                                             std::string& error) {
    error.clear();
    width = -1;
    height = -1;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "unable to open JPEG file";
        return false;
    }

    const auto read_byte = [&file]() -> std::optional<unsigned char> {
        char byte = 0;
        if (!file.get(byte)) {
            return std::nullopt;
        }
        return static_cast<unsigned char>(byte);
    };

    const auto read_u16_be = [&read_byte]() -> std::optional<std::uint16_t> {
        const auto high = read_byte();
        const auto low = read_byte();
        if (!high || !low) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(*high) << 8U) |
                                          static_cast<std::uint16_t>(*low));
    };

    const auto first = read_byte();
    const auto second = read_byte();
    if (!first || !second || *first != 0xFF || *second != 0xD8) {
        error = "not a JPEG file";
        return false;
    }

    while (true) {
        auto marker_prefix = read_byte();
        if (!marker_prefix) {
            error = "unexpected end of JPEG file";
            return false;
        }
        while (*marker_prefix != 0xFF) {
            marker_prefix = read_byte();
            if (!marker_prefix) {
                error = "unexpected end of JPEG file";
                return false;
            }
        }

        auto marker = read_byte();
        if (!marker) {
            error = "unexpected end of JPEG file";
            return false;
        }
        while (*marker == 0xFF) {
            marker = read_byte();
            if (!marker) {
                error = "unexpected end of JPEG file";
                return false;
            }
        }

        if (*marker == 0xD9 || *marker == 0xDA) {
            error = "JPEG dimensions not found";
            return false;
        }
        if (*marker >= 0xD0 && *marker <= 0xD7) {
            continue;
        }

        const auto length = read_u16_be();
        if (!length || *length < 2) {
            error = "invalid JPEG marker length";
            return false;
        }

        if (is_jpeg_sof_marker(*marker)) {
            (void) read_byte();
            const auto image_height = read_u16_be();
            const auto image_width = read_u16_be();
            if (!image_height || !image_width) {
                error = "unexpected end of JPEG file";
                return false;
            }
            height = static_cast<int>(*image_height);
            width = static_cast<int>(*image_width);
            return true;
        }

        file.ignore(static_cast<std::streamsize>(*length - 2));
        if (!file) {
            error = "unexpected end of JPEG file";
            return false;
        }
    }
}

bool ImageViewerServer::read_png_dimensions(const std::filesystem::path& path,
                                            int& width,
                                            int& height,
                                            std::string& error) {
    error.clear();
    width = -1;
    height = -1;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "unable to open PNG file";
        return false;
    }

    std::array<unsigned char, 24> header{};
    file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (file.gcount() != static_cast<std::streamsize>(header.size())) {
        error = "PNG header too small";
        return false;
    }

    constexpr std::array<unsigned char, 8> signature{{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}};
    if (!std::equal(signature.begin(), signature.end(), header.begin())) {
        error = "not a PNG file";
        return false;
    }

    if (!(header[12] == 'I' && header[13] == 'H' && header[14] == 'D' && header[15] == 'R')) {
        error = "PNG header missing IHDR chunk";
        return false;
    }

    width = static_cast<int>((static_cast<std::uint32_t>(header[16]) << 24U) |
                             (static_cast<std::uint32_t>(header[17]) << 16U) |
                             (static_cast<std::uint32_t>(header[18]) << 8U) |
                             static_cast<std::uint32_t>(header[19]));
    height = static_cast<int>((static_cast<std::uint32_t>(header[20]) << 24U) |
                              (static_cast<std::uint32_t>(header[21]) << 16U) |
                              (static_cast<std::uint32_t>(header[22]) << 8U) |
                              static_cast<std::uint32_t>(header[23]));
    return true;
}

std::string ImageViewerServer::format_bytes(uintmax_t size) {
    constexpr std::array<const char*, 5> units{{"B", "KB", "MB", "GB", "TB"}};
    double value = static_cast<double>(size);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

app::Settings ImageViewerServer::load_settings_snapshot() const {
    app::Settings settings;
    std::string error;
    if (std::filesystem::exists(settings_path_)) {
        (void) settings.load(settings_path_, error);
    }
    return settings;
}

std::string ImageViewerServer::build_settings_page(const app::Settings& settings,
                                                   const std::optional<LatestImage>& latest,
                                                   std::optional<std::string> message) const {
    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
         << "<title>DaliCam Home</title>"
         << "<style>"
         << ":root{color-scheme:dark;}"
         << "body{margin:0;font-family:system-ui,-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#101010;color:#eee;}"
         << "header{padding:16px 20px;background:#181818;border-bottom:1px solid #333;}"
         << "main{padding:16px 20px;max-width:980px;margin:0 auto;display:grid;gap:14px;}"
         << ".card{background:#151515;border:1px solid #2f2f2f;border-radius:10px;padding:14px;}"
         << ".grid{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,360px);gap:14px;align-items:start;}"
         << "@media (max-width: 860px){.grid{grid-template-columns:1fr;}}"
         << ".latest-view{display:grid;gap:10px;}"
         << ".latest-view img{display:block;width:100%;max-width:100%;max-height:56vh;object-fit:contain;background:#000;border-radius:8px;}"
         << ".meta{display:flex;flex-wrap:wrap;gap:12px;font-size:0.92rem;color:#bbb;}"
         << ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}"
         << "@media (max-width: 640px){.row{grid-template-columns:1fr;}}"
         << "label{display:block;font-weight:600;margin-bottom:6px;}"
         << "input,select,textarea{width:100%;box-sizing:border-box;background:#0d0d0d;color:#eee;border:1px solid #444;border-radius:8px;padding:10px;font:inherit;}"
         << "input[type=password]{letter-spacing:0.05em;}"
         << "textarea{min-height:96px;resize:vertical;}"
         << ".actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center;}"
         << "button,a.button{border:none;border-radius:8px;padding:10px 14px;font:inherit;text-decoration:none;display:inline-block;}"
         << "button{background:#2d7dff;color:#fff;cursor:pointer;}"
         << "button.danger{background:#cc4b4b;}"
         << "a.button{background:#2a2a2a;color:#fff;}"
         << ".message{padding:12px 14px;border-radius:10px;border:1px solid transparent;}"
         << ".message.ok{background:#112414;border-color:#2f6d3a;}"
         << ".message.error{background:#2a1414;border-color:#7a3333;}"
         << ".small{color:#bbb;font-size:0.92rem;}"
         << "code{background:#222;padding:2px 6px;border-radius:4px;}"
         << ".upload-status{min-height:1.2em;color:#9fd3ff;}"
         << ".generate-status{min-height:1.2em;color:#9fd3ff;}"
         << ".side-column{display:grid;gap:14px;}"
         << ".section-title{display:flex;justify-content:space-between;align-items:baseline;gap:8px;margin-bottom:10px;}"
         << "</style></head><body>"
         << "<header><h1>DaliCam Home</h1><div class=\"small\">Latest photo, uploads, and settings for the main app.</div></header>"
         << "<main>";

    if (message) {
        html << *message;
    }

    html << "<div class=\"grid\">";
    html << "<div class=\"card latest-view\">";
    html << "<div class=\"section-title\"><strong>Latest image</strong>";
    if (latest) {
        html << "<span class=\"small\">Source: " << escape_html(source_label(latest->source)) << "</span>";
    }
    html << "</div>";

    if (latest) {
        const auto raw_path = latest->source == Source::Camera ? "/camera/raw"
                             : latest->source == Source::Generated ? "/generated/raw"
                                                                   : "/uploads/raw";
        const auto view_path = latest->source == Source::Camera ? "/camera"
                              : latest->source == Source::Generated ? "/generated"
                                                                    : "/uploads";
        const auto cache_buster = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        html << "<img src=\"" << raw_path << "?t=" << cache_buster << "\" alt=\"Latest image\">"
             << "<div class=\"meta\">"
             << "<span>File: " << escape_html(latest->image.filename) << "</span>"
             << "<span>Size: " << escape_html(format_bytes(latest->image.size_bytes)) << "</span>";
        if (latest->image.width > 0 && latest->image.height > 0) {
            html << "<span>Dimensions: " << latest->image.width << " × " << latest->image.height << "</span>";
        }
        html << "</div>"
             << "<div class=\"actions\">"
             << "<a class=\"button\" href=\"" << view_path << "\">Open viewer</a>"
             << "<a class=\"button\" href=\"" << raw_path << "\">Open raw file</a>"
             << "</div>";
    } else {
        html << "<div class=\"small\">No image is available yet. Capture one, generate one, or upload a PNG/JPEG below.</div>";
    }

    html << "</div>";

    html << "<div class=\"side-column\">"
         << "<div class=\"card\">"
         << "<div class=\"section-title\"><strong>Upload image</strong><span class=\"small\">PNG or JPEG</span></div>"
         << "<form id=\"upload-form\">"
         << "<input id=\"upload-file\" type=\"file\" accept=\"image/png,image/jpeg\">"
         << "<div class=\"actions\" style=\"margin-top:10px\">"
         << "<button type=\"submit\">Upload</button>"
         << "<span id=\"upload-status\" class=\"upload-status\"></span>"
         << "</div></form>"
         << "<script>"
         << "(function(){const form=document.getElementById('upload-form');const input=document.getElementById('upload-file');const status=document.getElementById('upload-status');if(!form||!input||!status){return;}form.addEventListener('submit',async function(event){event.preventDefault();const file=input.files&&input.files[0];if(!file){status.textContent='Choose an image first.';return;}status.textContent='Uploading…';try{const response=await fetch('/upload',{method:'POST',headers:{'Content-Type':file.type||'application/octet-stream','X-Filename':file.name||'upload'},body:file});if(!response.ok){status.textContent='Upload failed ('+response.status+').';return;}window.location.reload();}catch(error){status.textContent='Upload failed.';}});})();"
         << "</script>"
         << "<div class=\"small\" style=\"margin-top:10px\">Uploaded files are stored separately so we can compare them with captured and generated images.</div>"
         << "</div>"
         << "<div class=\"card\">"
         << "<div class=\"section-title\"><strong>Generate image</strong><span class=\"small\">Press Enter or click Generate</span></div>"
         << "<form id=\"generate-form\" method=\"post\" action=\"/generate\" data-has-image=\"" << (latest ? "yes" : "no") << "\">"
         << "<input id=\"generate-prompt\" name=\"prompt\" type=\"text\" placeholder=\"Optional prompt — blank uses preset only\" autocomplete=\"off\" spellcheck=\"true\">";

    if (latest) {
        const auto latest_source_value = latest->source == Source::Camera ? "camera"
                                      : latest->source == Source::Generated ? "generated"
                                                                            : "uploads";
        html << "<input type=\"hidden\" name=\"source\" value=\"" << latest_source_value << "\">"
             << "<input type=\"hidden\" name=\"source_filename\" value=\"" << escape_html(latest->image.filename) << "\">";
    }

    html << "<div class=\"actions\" style=\"margin-top:10px\">"
         << "<button id=\"generate-button\" type=\"submit\">Generate image</button>"
         << "<span id=\"generate-status\" class=\"generate-status\"></span>"
         << "</div></form>"
         << "<div class=\"small\" style=\"margin-top:10px\">The latest image above will be used as the source when available. Leave the prompt empty to use the preset only.</div>"
         << "<div style=\"margin-top:12px\">"
         << "<form id=\"capture-form\" method=\"post\" action=\"/capture\">"
         << "<button id=\"capture-button\" type=\"submit\" class=\"danger\">Remote capture</button>"
         << "<span id=\"capture-status\" class=\"generate-status\" style=\"margin-left:10px\"></span>"
         << "</form>"
         << "<div class=\"small\" style=\"margin-top:10px\">Take a photo without touching the device.</div>"
         << "</div>"
         << "<script>"
         << "(function(){const form=document.getElementById('generate-form');const prompt=document.getElementById('generate-prompt');const button=document.getElementById('generate-button');const status=document.getElementById('generate-status');if(!form||!prompt||!button||!status){return;}form.addEventListener('submit',async function(event){event.preventDefault();const hasImage=form.dataset.hasImage==='yes';const text=(prompt.value||'').trim();const confirmText=hasImage?(text?'Generate from the latest image with this prompt?':'Generate from the latest image using the preset only?'):(text?'Generate a new image with this prompt?':'Generate a new image using the preset only?');if(!window.confirm(confirmText)){return;}button.disabled=true;status.textContent='Generating…';try{const body=new URLSearchParams(new FormData(form));const response=await fetch(form.action,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const html=await response.text();document.open();document.write(html);document.close();}catch(error){button.disabled=false;status.textContent='Generation failed.';}});})();"
         << "</script>"
         << "<script>"
         << "(function(){const form=document.getElementById('capture-form');const button=document.getElementById('capture-button');const status=document.getElementById('capture-status');if(!form||!button||!status){return;}form.addEventListener('submit',async function(event){event.preventDefault();if(!window.confirm('Capture a new photo now?')){return;}button.disabled=true;status.textContent='Capturing…';try{const response=await fetch(form.action,{method:'POST'});const html=await response.text();document.open();document.write(html);document.close();}catch(error){button.disabled=false;status.textContent='Capture failed.';}});})();"
         << "</script>"
         << "</div>"
         << "</div>";

    html << "</div>";

    html << "<div class=\"card\"><form method=\"post\" action=\"/settings\">"
         << "<div><label for=\"api_key\">Gemini API key</label>"
         << "<input id=\"api_key\" name=\"api_key\" type=\"password\" value=\"\" placeholder=\"Leave blank to keep the current key\" autocomplete=\"off\" spellcheck=\"false\"></div>"
         << "<div class=\"row\"><div><label for=\"gemini_model\">Model</label><select id=\"gemini_model\" name=\"gemini_model\">";

    for (const auto& model : app::Settings::kSupportedGeminiModels) {
        html << "<option value=\"" << escape_html(model) << "\"" << select_option(model, settings.gemini_model) << ">"
             << escape_html(model) << "</option>";
    }

    html << "</select></div><div><label for=\"default_personality\">Personality</label><select id=\"default_personality\" name=\"default_personality\">";
    for (const auto& preset : app::prompt_presets()) {
        html << "<option value=\"" << escape_html(preset.id) << "\"" << select_option(preset.id, settings.default_personality) << ">"
             << escape_html(preset.label) << "</option>";
    }

    html << "</select></div></div>"
         << "<div class=\"row\"><div><label for=\"allow_text_only_generation\">Allow text-only generation</label><select id=\"allow_text_only_generation\" name=\"allow_text_only_generation\">"
         << "<option value=\"yes\"" << select_option("yes", bool_option(settings.allow_text_only_generation)) << ">Yes</option>"
         << "<option value=\"no\"" << select_option("no", bool_option(settings.allow_text_only_generation)) << ">No</option>"
         << "</select></div><div><label for=\"save_source_images\">Save source images</label><select id=\"save_source_images\" name=\"save_source_images\">"
         << "<option value=\"yes\"" << select_option("yes", bool_option(settings.save_source_images)) << ">Yes</option>"
         << "<option value=\"no\"" << select_option("no", bool_option(settings.save_source_images)) << ">No</option>"
         << "</select></div></div>"
         << "<div class=\"row\"><div><label for=\"save_generated_images\">Save generated images</label><select id=\"save_generated_images\" name=\"save_generated_images\">"
         << "<option value=\"yes\"" << select_option("yes", bool_option(settings.save_generated_images)) << ">Yes</option>"
         << "<option value=\"no\"" << select_option("no", bool_option(settings.save_generated_images)) << ">No</option>"
         << "</select></div><div class=\"small\">These values are written to <code>" << escape_html(settings_path_.string()) << "</code>.</div></div>"
         << "<div class=\"actions\"><button type=\"submit\">Save settings</button>"
         << "<a class=\"button\" href=\"/camera\">Open camera gallery</a>"
         << "<a class=\"button\" href=\"/generated\">Open generated gallery</a>"
         << "<a class=\"button\" href=\"/uploads\">Open uploads gallery</a>"
         << "</div></form></div>"
         << "</main></body></html>";

    return html.str();
}

std::string ImageViewerServer::build_page(Source source,
                                           std::optional<std::string> selected_filename,
                                           std::optional<std::string> message) const {
    const auto title = source_label(source);
    const auto images = list_images(source);
    std::optional<ImageInfo> selected_image;
    if (selected_filename) {
        selected_image = image_by_name(source, *selected_filename);
    }
    if (!selected_image && !images.empty()) {
        selected_image = images.front();
    }

    const std::string gallery_base = source == Source::Camera ? "/camera"
                                    : source == Source::Generated ? "/generated"
                                                                  : "/uploads";
    const std::string raw_base = source == Source::Camera ? "/camera/raw"
                                : source == Source::Generated ? "/generated/raw"
                                                              : "/uploads/raw";
    const auto cache_buster = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<std::string> day_order;
    std::unordered_map<std::string, std::vector<const ImageInfo*>> grouped;
    for (const auto& image : images) {
        const auto day = format_day_label(image.modified_at);
        if (grouped.find(day) == grouped.end()) {
            day_order.push_back(day);
        }
        grouped[day].push_back(&image);
    }

    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
         << "<title>" << escape_html(title) << " Gallery</title>"
         << "<style>"
         << ":root{color-scheme:dark;}"
         << "body{margin:0;font-family:system-ui,-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#101010;color:#eee;}"
         << "header{display:flex;flex-wrap:wrap;gap:12px;justify-content:space-between;align-items:center;padding:12px 16px;background:#181818;border-bottom:1px solid #333;}"
         << ".meta{display:flex;flex-wrap:wrap;gap:12px;font-size:0.92rem;color:#bbb;}"
         << ".links{display:flex;flex-wrap:wrap;gap:8px;}"
         << ".links a{color:#fff;text-decoration:none;background:#2a2a2a;border-radius:8px;padding:8px 12px;}"
         << ".shell{padding:14px;display:grid;gap:14px;max-width:1200px;margin:0 auto;}"
         << ".card{background:#151515;border:1px solid #2f2f2f;border-radius:10px;padding:14px;}"
         << ".preview{display:grid;gap:12px;}"
         << ".preview img{display:block;width:100%;max-width:100%;max-height:56vh;object-fit:contain;background:#000;border-radius:8px;}"
         << ".thumb-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:10px;}"
         << ".thumb{display:grid;gap:8px;background:#101010;border:1px solid #333;border-radius:10px;padding:10px;color:#eee;}"
         << ".thumb:hover{border-color:#5a8cff;}"
         << ".thumb img{display:block;width:100%;height:140px;object-fit:cover;background:#000;border-radius:8px;}"
         << ".thumb-meta{font-size:0.88rem;color:#ddd;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
         << ".thumb-sub{font-size:0.8rem;color:#aaa;}"
         << ".thumb-actions{display:flex;flex-wrap:wrap;gap:8px;}"
         << ".thumb-actions form{margin:0;display:inline-block;}"
         << ".thumb-actions a,.thumb-actions button{padding:8px 10px;border-radius:8px;border:none;font:inherit;text-decoration:none;cursor:pointer;}"
         << ".thumb-actions a{background:#2a2a2a;color:#fff;display:inline-block;}"
         << ".thumb-actions button{background:#cc4b4b;color:#fff;}"
         << ".day-group{border:1px solid #2a2a2a;border-radius:10px;padding:10px;background:#141414;}"
         << ".section-title{display:flex;justify-content:space-between;align-items:baseline;gap:8px;margin-bottom:10px;}"
         << ".small{color:#bbb;font-size:0.92rem;}"
         << ".day-group + .day-group{margin-top:10px;}"
         << ".day-group summary{cursor:pointer;font-weight:700;list-style:none;}"
         << ".day-group summary::-webkit-details-marker{display:none;}"
         << ".actions{display:flex;flex-wrap:wrap;gap:8px;}"
         << ".actions a{color:#fff;text-decoration:none;background:#2a2a2a;border-radius:8px;padding:8px 12px;}"
         << ".notice{padding:16px;}"
         << "code{background:#222;padding:2px 6px;border-radius:4px;}"
         << "</style></head><body>"
         << "<header><div><strong>" << escape_html(title) << " Gallery</strong>"
         << "<div class=\"meta\">"
         << "<span>" << images.size() << " image" << (images.size() == 1 ? "" : "s") << "</span>";
    if (selected_image) {
        html << "<span>Selected: " << escape_html(selected_image->filename) << "</span>";
        if (selected_image->width > 0 && selected_image->height > 0) {
            html << "<span>Dimensions: " << selected_image->width << " × " << selected_image->height << "</span>";
        }
    }
    html << "</div></div><div class=\"links\">"
         << "<a href=\"/\">Settings</a>"
         << "<a href=\"/camera\">Camera</a>"
         << "<a href=\"/generated\">Generated</a>"
         << "<a href=\"/uploads\">Uploads</a>"
         << "</div></header>"
         << "<div class=\"shell\">";

    if (message) {
        html << *message;
    }

    if (selected_image) {
        const auto selected_raw = raw_base + "/" + selected_image->filename;
        html << "<section class=\"card preview\">"
             << "<div class=\"section-title\"><strong>Selected image</strong><span class=\"small\">Click any thumbnail to switch</span></div>"
             << "<img src=\"" << selected_raw << "?t=" << cache_buster << "\" alt=\"Selected image\">"
             << "<div class=\"meta\">"
             << "<span>File: " << escape_html(selected_image->filename) << "</span>"
             << "<span>Size: " << escape_html(format_bytes(selected_image->size_bytes)) << "</span>";
        if (selected_image->width > 0 && selected_image->height > 0) {
            html << "<span>Dimensions: " << selected_image->width << " × " << selected_image->height << "</span>";
        }
        html << "</div>"
             << "<div class=\"actions\">"
             << "<a href=\"" << selected_raw << "\">Open raw</a>"
             << "<a href=\"" << gallery_base << "\">Back to newest</a>"
             << "</div></section>";
    } else {
        html << "<section class=\"card notice\">No images were found in <code>" << escape_html(source_dir(source).string())
             << "</code>.</section>";
    }

    html << "<section class=\"card\">"
         << "<div class=\"section-title\"><strong>By day</strong><span class=\"small\">Most recent first</span></div>";

    if (images.empty()) {
        html << "<div class=\"small\">There are no images in this gallery yet.</div>";
    } else {
        for (const auto& day : day_order) {
            const auto& entries = grouped[day];
            html << "<details class=\"day-group\" open>"
                 << "<summary>" << escape_html(day) << " <span class=\"small\">(" << entries.size() << ")</span></summary>"
                 << "<div class=\"thumb-grid\">";
            for (const auto* entry : entries) {
                const std::string select_link = std::string("/settings?source=") +
                                                url_encode(source == Source::Camera ? "camera"
                                                           : source == Source::Generated ? "generated"
                                                                                         : "uploads") +
                                                "&file=" + url_encode(entry->filename);
                const std::string download_link = gallery_base + "/download/" + entry->filename;
                const std::string delete_link = gallery_base + "/delete/" + entry->filename;
                const std::string thumb_raw = raw_base + "/" + entry->filename;
                html << "<div class=\"thumb\">"
                     << "<img loading=\"lazy\" src=\"" << thumb_raw << "?t=" << cache_buster << "\" alt=\"" << escape_html(entry->filename) << "\">"
                     << "<span class=\"thumb-meta\">" << escape_html(entry->filename) << "</span>"
                     << "<span class=\"thumb-sub\">" << escape_html(format_time_label(entry->modified_at)) << " • "
                     << escape_html(format_bytes(entry->size_bytes)) << "</span>"
                     << "<div class=\"thumb-actions\">"
                     << "<a href=\"" << select_link << "\">Select for editing</a>"
                     << "<a href=\"" << download_link << "\">Download</a>"
                     << "<form method=\"post\" action=\"" << delete_link << "\" onsubmit=\"return confirm('Delete this image?');\">"
                     << "<button type=\"submit\">Delete</button>"
                     << "</form>"
                     << "</div></div>";
            }
            html << "</div></details>";
        }
    }

    html << "</section></div></body></html>";
    return html.str();
}

} // namespace platform::webui
