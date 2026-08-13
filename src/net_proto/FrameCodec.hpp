#pragma once

#include "NetProtocol.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Wire layout: [0xBEEF u16 BE][payload_len u32 BE][version u16 BE][JSON WireEnvelope].
//
// Version is major.minor in one u16 (high byte = major). Peers can pick a parser
// before the envelope body. try_decode: nullopt + consumed=0 + fatal=false → need more bytes.
// fatal=true → bad magic, len > kMaxPayloadLen, envelope parse fail,
// or an unexpected exception from the codec (JSON today).
// Declared length is checked before waiting for / treating the body as complete.
// Unknown Type / version mismatch are not fatal; they come back as NetProtocol.

constexpr std::size_t kFrameHeaderSize = 8;  // 2 magic + 4 length + 2 version

class FrameCodec {
public:
    static std::vector<std::uint8_t> encode(const NetProtocol& message);
    static std::optional<NetProtocol> try_decode(std::span<const std::uint8_t> frame, std::size_t& bytes_consumed, bool* fatal = nullptr);

private:
    static void writeBe16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
    static void writeBe32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
    static std::uint16_t readBe16(const std::uint8_t* bytes);
    static std::uint32_t readBe32(const std::uint8_t* bytes);
};
