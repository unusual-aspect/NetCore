#pragma once

#include <cstdint>
#include <string_view>

#include <magic_enum/magic_enum.hpp>

// Wire "Type" is magic_enum::enum_name(Opcode). kNetworkVersion is "major.minor".
// Bump major when this enum changes; bump minor when an existing opcode's Data layout changes.
constexpr std::string_view kNetworkVersion = "1.0";
constexpr int kProtoMajor = 1;

constexpr std::uint16_t kFrameMagic = 0xBEEF;
// JSON envelope on the wire (message body ≤ kMaxMessageBytes plus Type/Seq overhead).
constexpr std::uint32_t kMaxPayloadLen = static_cast<std::uint32_t>(1024u * 1024u + 256u * 1024u);

enum class Opcode : std::uint16_t {
    None = 0,
    Read,
    Set,
    Shutdown,
    VersionSrv,
    Ok,
    Error,
    LastMsgType  // sentinel; Vx consumes, never a payload
};

constexpr int versionMajor(std::string_view version) {
    // "1.0" → 1, "10.2" → 10, "abc" / "" → 0.
    const auto dot_pos = version.find('.');
    const auto major_text = version.substr(
        0, dot_pos == std::string_view::npos ? version.size() : dot_pos);
    int major_number = 0;
    for (char digit : major_text) {
        if (digit < '0' || digit > '9') {
            break;
        }
        major_number = major_number * 10 + (digit - '0');
    }
    return major_number;
}

constexpr int versionMinor(std::string_view version) {
    const auto dot_pos = version.find('.');
    if (dot_pos == std::string_view::npos || dot_pos + 1 >= version.size()) {
        return 0;
    }
    int minor_number = 0;
    for (std::size_t index = dot_pos + 1; index < version.size(); ++index) {
        const char digit = version[index];
        if (digit < '0' || digit > '9') {
            break;
        }
        minor_number = minor_number * 10 + (digit - '0');
        if (minor_number > 255) {
            return 255;
        }
    }
    return minor_number;
}

// Wire header u16: high byte = major, low byte = minor ("1.0" → 0x0100).
constexpr std::uint16_t versionToWire(std::string_view version) {
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(versionMajor(version)) << 8) |
        static_cast<unsigned>(versionMinor(version) & 0xFF));
}

inline std::string versionFromWire(std::uint16_t wire) {
    return std::to_string((wire >> 8) & 0xFF) + "." + std::to_string(wire & 0xFF);
}

constexpr std::uint16_t kNetworkVersionWire = versionToWire(kNetworkVersion);
