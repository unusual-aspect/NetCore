#include "FrameCodec.hpp"
#include "Dbg.hpp"
#include "NetDefaults.hpp"
#include "WireEnvelope.hpp"

#include <rfl.hpp>
#include <rfl/json.hpp>

#include <cstdio>

void FrameCodec::writeBe16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void FrameCodec::writeBe32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t FrameCodec::readBe16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((std::uint16_t(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t FrameCodec::readBe32(const std::uint8_t* bytes) {
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
}

std::vector<std::uint8_t> FrameCodec::encode(const NetProtocol& message) {
    if (exceedsMaxMessage(message.getMsgData().size())) {
        DBG("Cannot encode frame — message body is " + std::to_string(message.getMsgData().size()) +
            " bytes, over the " + std::to_string(kMaxMessageBytes) + " byte limit.");
        return {};
    }

    WireEnvelope envelope;
    // Unknown opcode: keep the original Type string so it round-trips as None + invalid_type_.
    envelope.Type = message.getInvalidType().empty() ? message.getType() : message.getInvalidType();
    envelope.Seq = message.seq();
    envelope.Data = std::string(message.getMsgData());

    const std::string json = rfl::json::write(envelope);
    if (json.size() > kMaxPayloadLen) {
        DBG("Cannot encode frame — JSON envelope is " + std::to_string(json.size()) +
            " bytes, over the " + std::to_string(kMaxPayloadLen) + " byte wire limit.");
        return {};
    }

    // [0xBEEF][len][ver][json]
    std::vector<std::uint8_t> frame;
    frame.reserve(kFrameHeaderSize + json.size());
    writeBe16(frame, kFrameMagic);
    writeBe32(frame, static_cast<std::uint32_t>(json.size()));
    writeBe16(frame, versionToWire(message.getVersion()));
    frame.insert(frame.end(), json.begin(), json.end());
    return frame;
}

std::optional<NetProtocol> FrameCodec::try_decode(std::span<const std::uint8_t> frame, std::size_t& bytes_consumed, bool* fatal) {
    bytes_consumed = 0;
    if (fatal) {
        *fatal = false;
    }

    // Header not here yet — wait.
    if (frame.size() < kFrameHeaderSize) {
        return std::nullopt;
    }

    const auto magic = readBe16(frame.data());
    if (magic != kFrameMagic) {
        // Not our stream (or LE 0xEFBE) — kill the connection.
        char hex[8]{};
        std::snprintf(hex, sizeof(hex), "%04X", magic);
        DBG(std::string("Cannot parse this stream — header is 0x") + hex + ", expected 0xBEEF. Not our protocol, or the bytes are swapped/corrupt.");
        if (fatal) {
            *fatal = true;
        }
        return std::nullopt;
    }

    const auto payload_size = readBe32(frame.data() + 2);
    if (payload_size > kMaxPayloadLen) {
        // Declared length is a lie / attack — do not wait for it.
        DBG("Cannot parse this frame — body claims " + std::to_string(payload_size) +
            " bytes, over the " + std::to_string(kMaxPayloadLen) + " byte wire limit. Not waiting for the rest.");
        if (fatal) {
            *fatal = true;
        }
        return std::nullopt;
    }

    const auto version_wire = readBe16(frame.data() + 6);

    // Declared body not fully in the buffer yet — wait.
    if (frame.size() < kFrameHeaderSize + payload_size) {
        return std::nullopt;
    }

    const std::string_view body(
        reinterpret_cast<const char*>(frame.data() + kFrameHeaderSize), payload_size);
    const auto parsed = rfl::json::read<WireEnvelope>(body);
    if (!parsed) {
        // Header was fine, body is garbage.
        DBG("Cannot parse the frame body — bytes after 0xBEEF are not a valid JSON envelope (Type/Seq/Data).");
        if (fatal) {
            *fatal = true;
        }
        return std::nullopt;
    }

    const WireEnvelope& envelope = *parsed;
    std::string invalid_type;
    // Unknown Type → Opcode::None, invalid_type keeps the wire string.
    const Opcode opcode = NetProtocol::typeFromName(envelope.Type, &invalid_type);

    NetProtocol message(opcode, envelope.Data, envelope.Seq);
    message.version_ = versionFromWire(version_wire);
    message.invalid_type_ = std::move(invalid_type);

    bytes_consumed = kFrameHeaderSize + payload_size;
    return message;
}
