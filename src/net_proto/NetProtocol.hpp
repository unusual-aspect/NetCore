#pragma once

#include "Opcode.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// In-memory message. encode() → FrameCodec wire bytes; decode() is the reverse.
//
// Unknown Type string becomes Opcode::None and is kept in invalid_type_ so
// encode() can echo it. decode() still succeeds in that case.
// compatible() is major-only. feed() / ProtocolStack do not drop on mismatch;
// Vx/V1 decide what to do with the opcode.

class NetProtocol {
public:
    NetProtocol() = default;
    explicit NetProtocol(Opcode type, std::string_view message = {}, std::uint32_t seq = 0);

    Opcode getMsgType() const { return type_; }
    std::string getType() const;
    std::string_view getMsgData() const { return payload_; }
    std::string getVersion() const { return version_; }
    std::string getInvalidType() const { return invalid_type_; }
    std::uint32_t seq() const { return seq_; }
    bool isEmpty() const { return payload_.empty(); }
    bool compatible() const;

    static Opcode typeFromName(std::string_view name, std::string* invalid_type = nullptr);

    std::vector<std::uint8_t> encode() const;
    static std::optional<NetProtocol> decode(std::span<const std::uint8_t> frame, std::size_t& consumed, bool* fatal = nullptr);

private:
    friend class FrameCodec;

    Opcode type_ = Opcode::None;
    std::string version_{kNetworkVersion};
    std::uint32_t seq_ = 0;
    std::string payload_;
    std::string invalid_type_;  // original Type when type_ is None
};
