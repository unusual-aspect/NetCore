#include "NetProtocol.hpp"
#include "FrameCodec.hpp"

#include <magic_enum/magic_enum.hpp>

NetProtocol::NetProtocol(Opcode type, std::string_view message, std::uint32_t seq): type_(type), version_(kNetworkVersion), seq_(seq), payload_(message) {}

std::string NetProtocol::getType() const {
    return std::string(magic_enum::enum_name(type_));
}

bool NetProtocol::compatible() const {
    // Major only. 1.9 is fine, 2.0 is not.
    return versionMajor(version_) == kProtoMajor;
}

Opcode NetProtocol::typeFromName(std::string_view name, std::string* invalid_type) {
    if (const auto opcode = magic_enum::enum_cast<Opcode>(name); opcode.has_value()) {
        return *opcode;
    }
    // Keep the raw Type so encode() can send it back unchanged.
    if (invalid_type) {
        *invalid_type = std::string(name);
    }
    return Opcode::None;
}

std::vector<std::uint8_t> NetProtocol::encode() const {
    return FrameCodec::encode(*this);
}

std::optional<NetProtocol> NetProtocol::decode(std::span<const std::uint8_t> frame, std::size_t& consumed, bool* fatal) {
    // consumed / fatal filled by the framer.
    return FrameCodec::try_decode(frame, consumed, fatal);
}
