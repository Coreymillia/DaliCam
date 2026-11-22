/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "openai_client.h"

#include <array>
#include <cctype>
#include <cstdint>
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

constexpr std::string_view kOpenAIImagesEndpoint = "https://api.openai.com/v1/images/";

std::string normalize_openai_model_id(std::string_view model) {
    return std::string(model);
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

bool write_text_file(const std::filesystem::path& path, const std::string& content) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return false;
        }
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return false;
    }

    output << content;
    return static_cast<bool>(output);
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

std::string build_openai_responses_request_body(std::string_view prompt,
                                                const std::optional<std::filesystem::path>& source_image_path) {
    std::string body;
    body.reserve(4096);
    body.append("{\n");
    body.append("  \"model\": \"gpt-image-1\",\n");
    body.append("  \"input\": [\n");
    body.append("    {\n");
    body.append("      \"role\": \"user\",\n");
    body.append("      \"content\": [\n");
    body.append("        {\n");
    body.append("          \"type\": \"input_text\",\n");
    body.append("          \"text\": \"");
    body.append(escape_json(prompt));
    body.append("\"\n");
    body.append("        }");

    if (source_image_path && std::filesystem::is_regular_file(*source_image_path)) {
        const auto image_bytes = read_binary_file(*source_image_path);
        const auto image_base64 = encode_base64(image_bytes);
        body.append(",\n");
        body.append("        {\n");
        body.append("          \"type\": \"input_image\",\n");
        body.append("          \"image_url\": \"data:");
        body.append(escape_json(infer_mime_type(*source_image_path)));
        body.append(";base64,");
        body.append(escape_json(image_base64));
        body.append("\",\n");
        body.append("          \"detail\": \"auto\"\n");
        body.append("        }");
    }

    body.append("\n      ]\n");
    body.append("    }\n");
    body.append("  ],\n");
    body.append("  \"tools\": [\n");
    body.append("    {\n");
    body.append("      \"type\": \"image_generation\"\n");
    body.append("    }\n");
    body.append("  ]\n");
    body.append("}\n");
    return body;
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
            error = "invalid base64 data in OpenAI response";
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

    if (const auto image_call_position = response.find("\"image_generation_call\""); image_call_position != std::string_view::npos) {
        const auto image_call_segment = response.substr(image_call_position);
        if (const auto result = extract_value_from(image_call_segment, "result")) {
            return result;
        }
    }

    if (const auto data_position = response.find("\"data\""); data_position != std::string_view::npos) {
        const auto data_segment = response.substr(data_position);
        if (const auto b64 = extract_value_from(data_segment, "b64_json")) {
            return b64;
        }
        if (const auto inline_b64 = extract_value_from(data_segment, "imageBytes")) {
            return inline_b64;
        }
        if (const auto result = extract_value_from(data_segment, "result")) {
            return result;
        }
    }

    if (const auto b64 = extract_value_from(response, "b64_json")) {
        return b64;
    }
    if (const auto image_bytes = extract_value_from(response, "imageBytes")) {
        return image_bytes;
    }
    if (const auto result = extract_value_from(response, "result")) {
        return result;
    }

    return extract_value_from(response, "data");
}

} // namespace

ImageGenerationResult generate_openai_image_request(std::string_view api_key,
                                                    std::string_view model,
                                                    std::string_view prompt,
                                                    const std::filesystem::path& output_path,
                                                    const std::filesystem::path& temp_dir,
                                                    const std::optional<std::filesystem::path>& source_image_path) {
    ImageGenerationResult result;
    result.image_path = output_path;
    result.response_path = temp_dir / "openai-response.json";
    result.error_path = temp_dir / "openai-error.txt";

    if (api_key.empty()) {
        result.message = "OpenAI API key is empty.";
        return result;
    }

    if (prompt.empty()) {
        result.message = "OpenAI prompt is empty.";
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

    std::error_code remove_error;
    std::filesystem::remove(result.response_path, remove_error);
    std::filesystem::remove(result.error_path, remove_error);
    std::filesystem::remove(result.image_path, remove_error);

    const auto http_code_path = temp_dir / "openai-http-code.txt";
    std::filesystem::remove(http_code_path, remove_error);

    const auto api_model = normalize_openai_model_id(model);
    const bool modern_model = api_model.rfind("dall-e", 0) != 0;

    std::string command = "curl --silent --show-error --max-time 120 ";
    command += "--header " + shell_quote(std::string("Authorization: Bearer ") + std::string(api_key));
    command += " --output " + shell_quote(result.response_path.string());
    command += " --write-out " + shell_quote("%{http_code}");
    command += " --form " + shell_quote(std::string("model=") + api_model);
    command += " --form " + shell_quote(std::string("prompt=") + std::string(prompt));

    if (modern_model) {
        command += " --form " + shell_quote("output_format=png");
        command += " --form " + shell_quote("quality=high");
        command += " --form " + shell_quote("background=auto");
        if (!source_image_path || source_image_path->empty()) {
            command += " --form " + shell_quote("size=1024x1024");
        }
    }
    else {
        command += " --form " + shell_quote("size=1024x1024");
        command += " --form " + shell_quote("n=1");
        command += " --form " + shell_quote("response_format=b64_json");
    }

    if (source_image_path && !source_image_path->empty()) {
        const auto image_bytes = read_binary_file(*source_image_path);
        if (image_bytes.empty()) {
            result.message = "could not read source image for OpenAI request";
            return result;
        }
        command += " --form " + shell_quote(std::string("image=@") + source_image_path->string());
    }

    command += " " + shell_quote(std::string(kOpenAIImagesEndpoint) + (source_image_path ? "edits" : "generations"));
    command += " > " + shell_quote(http_code_path.string());
    command += " 2> " + shell_quote(result.error_path.string());

    const int exit_code = system_exit_code(command);
    if (exit_code != 0) {
        auto stderr_text = read_file_preview(result.error_path, 320);
        auto response_text = read_file_preview(result.response_path, 320);
        auto http_code_text = read_file_preview(http_code_path, 32);
        std::string last_error;
        if (!stderr_text.empty()) {
            last_error.append(stderr_text);
        }
        if (!http_code_text.empty()) {
            if (!last_error.empty()) {
                last_error.append(" | ");
            }
            last_error.append("HTTP ").append(http_code_text);
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
        result.message = last_error;
        return result;
    }

    const auto http_code = read_file_preview(http_code_path, 32);
    if (!http_code.empty() && (http_code.front() != '2')) {
        auto response_text = read_file_preview(result.response_path, 320);
        auto stderr_text = read_file_preview(result.error_path, 320);
        std::string last_error = "HTTP ";
        last_error.append(http_code);
        if (!stderr_text.empty()) {
            last_error.append(" | ").append(stderr_text);
        }
        if (!response_text.empty()) {
            last_error.append(" | ").append(response_text);
        }
        result.message = last_error;
        return result;
    }

    std::error_code file_error;
    const auto response_size = std::filesystem::file_size(result.response_path, file_error);
    if (file_error || response_size == 0) {
        result.message = "OpenAI request returned an empty response.";
        return result;
    }

    const auto response_text = read_text_file(result.response_path);
    if (response_text.empty()) {
        result.message = "OpenAI response was empty.";
        return result;
    }

    const auto image_data = extract_base64_image_data(response_text);
    if (!image_data) {
        result.message = "OpenAI response did not contain image data.";
        return result;
    }

    std::vector<std::uint8_t> decoded_image;
    std::string decode_error;
    if (!decode_base64(*image_data, decoded_image, decode_error)) {
        result.message = decode_error.empty() ? "failed to decode OpenAI image data" : decode_error;
        return result;
    }

    std::string write_error;
    if (!write_binary_file(output_path, decoded_image, write_error)) {
        result.message = write_error;
        return result;
    }

    result.ok = true;
    result.message = "OpenAI image saved.";
    return result;
}

} // namespace app
