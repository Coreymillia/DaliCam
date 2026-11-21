/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "gemini_client.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

namespace app {
namespace {

constexpr std::string_view kGeminiEndpointPrefix =
    "https://generativelanguage.googleapis.com/v1beta/models/";
constexpr std::string_view kGeminiEndpointSuffix = ":generateContent?key=";
constexpr std::string_view kTestPrompt =
    "Create a simple test image for verifying Gemini image generation. "
    "Keep it clean, centered, and easy to recognize.";

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

std::string escape_json(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
            case '\\':
                escaped.append("\\\\");
                break;
            case '"':
                escaped.append("\\\"");
                break;
            case '\n':
                escaped.append("\\n");
                break;
            case '\r':
                escaped.append("\\r");
                break;
            case '\t':
                escaped.append("\\t");
                break;
            default:
                escaped.push_back(character);
                break;
        }
    }
    return escaped;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::string read_file_preview(const std::filesystem::path& path, std::size_t max_chars) {
    auto text = read_text_file(path);
    if (text.size() > max_chars) {
        text.resize(max_chars);
        text.append("...");
    }
    return text;
}

bool write_text_file(const std::filesystem::path& path, const std::string& content, std::string& error) {
    error.clear();

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create directory: " + filesystem_error.message();
            return false;
        }
    }

    std::ofstream output(path, std::ios::trunc);
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

bool write_binary_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& content,
                       std::string& error) {
    error.clear();

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create directory: " + filesystem_error.message();
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open image file for writing";
        return false;
    }

    output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    if (!output) {
        error = "failed while writing image file";
        return false;
    }

    return true;
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

bool should_use_response_modalities(std::string_view model) {
    return model.rfind("gemini-3", 0) == 0 || model.find("preview") != std::string_view::npos;
}

std::string encode_base64(const std::vector<std::uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((data.size() + 2) / 3) * 4);

    std::size_t index = 0;
    while (index + 3 <= data.size()) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
                                    (static_cast<std::uint32_t>(data[index + 1]) << 8) |
                                    static_cast<std::uint32_t>(data[index + 2]);
        output.push_back(kAlphabet[(value >> 18) & 0x3F]);
        output.push_back(kAlphabet[(value >> 12) & 0x3F]);
        output.push_back(kAlphabet[(value >> 6) & 0x3F]);
        output.push_back(kAlphabet[value & 0x3F]);
        index += 3;
    }

    const std::size_t remaining = data.size() - index;
    if (remaining == 1) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[index]) << 16;
        output.push_back(kAlphabet[(value >> 18) & 0x3F]);
        output.push_back(kAlphabet[(value >> 12) & 0x3F]);
        output.push_back('=');
        output.push_back('=');
    }
    else if (remaining == 2) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
                                    (static_cast<std::uint32_t>(data[index + 1]) << 8);
        output.push_back(kAlphabet[(value >> 18) & 0x3F]);
        output.push_back(kAlphabet[(value >> 12) & 0x3F]);
        output.push_back(kAlphabet[(value >> 6) & 0x3F]);
        output.push_back('=');
    }

    return output;
}

std::string infer_mime_type(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    return "image/jpeg";
}

std::string build_request_body(std::string_view prompt,
                               bool use_response_modalities,
                               std::string_view image_base64,
                               std::string_view image_mime_type) {
    std::string body;
    body.reserve(4096 + image_base64.size());
    body.append("{\n");
    body.append("  \"contents\": [\n");
    body.append("    {\n");
    body.append("      \"role\": \"user\",\n");
    body.append("      \"parts\": [\n");
    body.append("        {\n");
    body.append("          \"text\": \"");
    body.append(escape_json(prompt));
    body.append("\"\n");
    body.append("        }");
    if (!image_base64.empty()) {
        body.append(",\n");
        body.append("        {\n");
        body.append("          \"inlineData\": {\n");
        body.append("            \"mimeType\": \"");
        body.append(escape_json(image_mime_type.empty() ? "image/jpeg" : image_mime_type));
        body.append("\",\n");
        body.append("            \"data\": \"");
        body.append(escape_json(image_base64));
        body.append("\"\n");
        body.append("          }\n");
        body.append("        }");
    }
    body.append("\n      ]\n");
    body.append("    }\n");
    body.append("  ],\n");
    body.append("  \"generationConfig\": {\n");
    body.append("    \"imageConfig\": {\n");
    body.append("      \"aspectRatio\": \"1:1\"\n");
    body.append("    },\n");
    if (use_response_modalities) {
        body.append("    \"responseModalities\": [\"IMAGE\", \"TEXT\"]\n");
    }
    else {
        body.append("    \"responseMimeType\": \"image/png\"\n");
    }
    body.append("  }\n");
    body.append("}\n");
    return body;
}

std::optional<std::string> extract_base64_image_data(std::string_view response) {
    const auto extract_value_from = [&](std::string_view segment, std::string_view key) -> std::optional<std::string> {
        const std::string marker = '"' + std::string(key) + '"';
        const auto key_position = segment.find(marker);
        if (key_position == std::string_view::npos) {
            return std::nullopt;
        }

        auto value_start = segment.find(':', key_position + marker.size());
        if (value_start == std::string_view::npos) {
            return std::nullopt;
        }
        value_start = segment.find('"', value_start);
        if (value_start == std::string_view::npos) {
            return std::nullopt;
        }
        ++value_start;

        const auto value_end = segment.find('"', value_start);
        if (value_end == std::string_view::npos || value_end <= value_start) {
            return std::nullopt;
        }

        return std::string(segment.substr(value_start, value_end - value_start));
    };

    if (const auto image_bytes = extract_value_from(response, "imageBytes")) {
        return image_bytes;
    }

    if (const auto inline_data_position = response.find("\"inlineData\"");
        inline_data_position != std::string_view::npos) {
        const auto inline_segment = response.substr(inline_data_position);
        if (const auto inline_data = extract_value_from(inline_segment, "data")) {
            return inline_data;
        }
    }

    return extract_value_from(response, "data");
}

bool decode_base64(std::string_view encoded, std::vector<std::uint8_t>& output, std::string& error) {
    error.clear();
    output.clear();
    output.reserve(encoded.size() * 3 / 4);

    std::array<int, 256> decode_table{};
    decode_table.fill(-1);
    for (char c = 'A'; c <= 'Z'; ++c) {
        decode_table[static_cast<unsigned char>(c)] = c - 'A';
    }
    for (char c = 'a'; c <= 'z'; ++c) {
        decode_table[static_cast<unsigned char>(c)] = c - 'a' + 26;
    }
    for (char c = '0'; c <= '9'; ++c) {
        decode_table[static_cast<unsigned char>(c)] = c - '0' + 52;
    }
    decode_table[static_cast<unsigned char>('+')] = 62;
    decode_table[static_cast<unsigned char>('/')] = 63;

    int value = 0;
    int bits = -8;
    for (unsigned char character : encoded) {
        if (std::isspace(character)) {
            continue;
        }
        if (character == '=') {
            break;
        }

        const int decoded = decode_table[character];
        if (decoded < 0) {
            error = "invalid base64 data in Gemini response";
            output.clear();
            return false;
        }

        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<std::uint8_t>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }

    return !output.empty();
}

GeminiImageResult generate_image_request_impl(std::string_view api_key,
                                              std::string_view model,
                                              std::string_view prompt,
                                              const std::filesystem::path& output_path,
                                              const std::filesystem::path& temp_dir,
                                              const std::optional<std::filesystem::path>& source_image_path) {
    GeminiImageResult result;
    result.image_path = output_path;
    result.response_path = temp_dir / "gemini-response.json";
    result.error_path = temp_dir / "gemini-error.txt";

    if (api_key.empty()) {
        result.message = "Gemini API key is empty.";
        return result;
    }

    if (prompt.empty()) {
        result.message = "Gemini prompt is empty.";
        return result;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(temp_dir, filesystem_error);
    if (filesystem_error) {
        result.message = "could not create temp directory: " + filesystem_error.message();
        return result;
    }

    const auto output_parent = output_path.parent_path();
    if (!output_parent.empty()) {
        std::filesystem::create_directories(output_parent, filesystem_error);
        if (filesystem_error) {
            result.message = "could not create output directory: " + filesystem_error.message();
            return result;
        }
    }

    const auto url = std::string(kGeminiEndpointPrefix) + std::string(model) +
                     std::string(kGeminiEndpointSuffix) + std::string(api_key);

    const bool response_modalities_first = should_use_response_modalities(model);
    const bool attempt_order[] = {response_modalities_first, !response_modalities_first};

    std::string image_base64;
    std::string image_mime_type;
    if (source_image_path && !source_image_path->empty()) {
        const auto image_bytes = read_binary_file(*source_image_path);
        if (image_bytes.empty()) {
            result.message = "could not read source image for Gemini request";
            return result;
        }
        image_base64 = encode_base64(image_bytes);
        image_mime_type = infer_mime_type(*source_image_path);
    }

    std::string last_error;
    for (const bool use_response_modalities : attempt_order) {
        std::string request_error;
        const auto request_path = temp_dir / "gemini-request.json";
        if (!write_text_file(request_path,
                             build_request_body(prompt,
                                                use_response_modalities,
                                                image_base64,
                                                image_mime_type),
                             request_error)) {
            result.message = request_error;
            return result;
        }

        std::error_code remove_error;
        std::filesystem::remove(result.response_path, remove_error);
        std::filesystem::remove(result.error_path, remove_error);
        std::filesystem::remove(result.image_path, remove_error);

        const auto command =
            "curl --silent --show-error --fail --max-time 90 "
            "--header " + shell_quote("Content-Type: application/json") +
            " --data-binary @" + shell_quote(request_path.string()) +
            " --output " + shell_quote(result.response_path.string()) +
            " " + shell_quote(url) +
            " 2> " + shell_quote(result.error_path.string());

        const int exit_code = system_exit_code(command);
        if (exit_code != 0) {
            auto stderr_text = read_file_preview(result.error_path, 320);
            auto response_text = read_file_preview(result.response_path, 320);
            last_error.clear();
            if (!stderr_text.empty()) {
                last_error.append(stderr_text);
            }
            if (!response_text.empty()) {
                if (!last_error.empty()) {
                    last_error.append(" | ");
                }
                last_error.append(response_text);
            }
            if (last_error.empty()) {
                last_error = "curl exited with code " + std::to_string(exit_code);
            }
            continue;
        }

        std::error_code file_error;
        const auto response_size = std::filesystem::file_size(result.response_path, file_error);
        if (file_error || response_size == 0) {
            last_error = "Gemini request returned an empty response.";
            continue;
        }

        const auto response_text = read_text_file(result.response_path);
        if (response_text.empty()) {
            last_error = "Gemini response was empty.";
            continue;
        }

        const auto image_data = extract_base64_image_data(response_text);
        if (!image_data) {
            last_error = "Gemini response did not contain image data.";
            continue;
        }

        std::vector<std::uint8_t> decoded_image;
        std::string decode_error;
        if (!decode_base64(*image_data, decoded_image, decode_error)) {
            last_error = decode_error.empty() ? "failed to decode Gemini image data" : decode_error;
            continue;
        }

        std::string write_error;
        if (!write_binary_file(output_path, decoded_image, write_error)) {
            last_error = write_error;
            continue;
        }

        result.ok = true;
        result.message = "Gemini image saved.";
        return result;
    }

    result.message = last_error.empty() ? "Gemini image request failed." : last_error;
    return result;
}

} // namespace

GeminiImageResult generate_gemini_image_request(std::string_view api_key,
                                                std::string_view model,
                                                std::string_view prompt,
                                                const std::filesystem::path& output_path,
                                                const std::filesystem::path& temp_dir,
                                                const std::optional<std::filesystem::path>& source_image_path) {
    return generate_image_request_impl(api_key,
                                       model,
                                       prompt,
                                       output_path,
                                       temp_dir,
                                       source_image_path);
}

GeminiImageResult test_gemini_image_request(std::string_view api_key,
                                           std::string_view model,
                                           const std::filesystem::path& output_path,
                                           const std::filesystem::path& temp_dir) {
    return generate_image_request_impl(api_key, model, kTestPrompt, output_path, temp_dir, std::nullopt);
}

} // namespace app
