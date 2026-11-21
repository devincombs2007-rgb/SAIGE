#include "saige/gfx_assets/resource_uri.hpp"

#include <algorithm>
#include <cctype>

namespace saige::gfx {

std::optional<std::vector<uint8_t>> decode_base64(std::string_view text)
{
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int v[4];
        int padding = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                // padding is only legal in the last quad: "xx==" or "xxx="
                if (!last || k < 2) {
                    return std::nullopt;
                }
                ++padding;
                v[k] = 0;
            } else {
                if (padding > 0) {
                    return std::nullopt;  // data after padding
                }
                v[k] = value_of(c);
                if (v[k] < 0) {
                    return std::nullopt;
                }
            }
        }
        const uint32_t triple = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                                (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
        if (padding < 2) {
            out.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            out.push_back(static_cast<uint8_t>(triple & 0xFF));
        }
    }
    return out;
}

bool is_data_uri(std::string_view uri)
{
    if (uri.size() < 5) {
        return false;
    }
    constexpr std::string_view prefix = "data:";
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(uri[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

std::optional<DataUri> decode_data_uri(std::string_view uri)
{
    if (!is_data_uri(uri)) {
        return std::nullopt;
    }
    const std::size_t comma = uri.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view header = uri.substr(5, comma - 5);  // "<mime>;base64"
    constexpr std::string_view marker = ";base64";
    if (header.size() < marker.size() || header.substr(header.size() - marker.size()) != marker) {
        return std::nullopt;  // only base64 payloads are accepted
    }
    DataUri result;
    result.mime_type = std::string(header.substr(0, header.size() - marker.size()));
    std::optional<std::vector<uint8_t>> bytes = decode_base64(uri.substr(comma + 1));
    if (!bytes) {
        return std::nullopt;
    }
    result.bytes = std::move(*bytes);
    return result;
}

ResolvedUri resolve_relative_uri(std::string_view uri)
{
    auto reject = [](const std::string& why) {
        ResolvedUri r;
        r.error = why;
        return r;
    };
    if (uri.empty()) {
        return reject("empty URI");
    }
    if (uri.size() > 1024) {
        return reject("URI is longer than 1024 bytes");
    }
    // Percent-decode exactly once.
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] != '%') {
            decoded.push_back(uri[i]);
            continue;
        }
        if (i + 2 >= uri.size()) {  // a '%' needs two more characters
            return reject("truncated percent escape");
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hex(uri[i + 1]);
        const int lo = hex(uri[i + 2]);
        if (hi < 0 || lo < 0) {
            return reject("invalid percent escape");
        }
        decoded.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    for (const char c : decoded) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return reject("control characters are not allowed in resource paths");
        }
        if (c == '\\') {
            return reject("backslashes are not allowed in resource paths");
        }
        if (c == ':') {
            return reject("drive letters and URI schemes are not allowed in resource paths");
        }
    }
    if (decoded.front() == '/') {
        return reject("absolute paths are not allowed");
    }
    std::string normalized;
    std::size_t start = 0;
    while (start <= decoded.size()) {
        const std::size_t slash = decoded.find('/', start);
        const std::size_t end = slash == std::string::npos ? decoded.size() : slash;
        const std::string_view segment(decoded.data() + start, end - start);
        if (segment.empty()) {
            return reject("empty path segment");
        }
        if (segment == "..") {
            return reject("'..' path segments are not allowed (the asset may not reference outside its directory)");
        }
        if (segment != ".") {
            if (!normalized.empty()) {
                normalized.push_back('/');
            }
            normalized.append(segment);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    if (normalized.empty()) {
        return reject("the path names no file");
    }
    ResolvedUri ok;
    ok.ok = true;
    ok.path = std::move(normalized);
    return ok;
}

} // namespace saige::gfx
