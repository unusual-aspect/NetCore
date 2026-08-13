#include <gtest/gtest.h>

#include "AbstractNetSession.hpp"
#include "FrameCodec.hpp"
#include "NetDefaults.hpp"
#include "NetProtocol.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Deterministic malformed-input stress for FrameCodec.
// Not libFuzzer: keeps the production build free of a fuzzer runtime. The
// contract under test is: try_decode never throws, never treats a declared
// length above kMaxPayloadLen as "wait for more", and never reports success
// with consumed==0.

namespace {

void put_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void put_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void decodeOnce(std::span<const std::uint8_t> bytes) {
    std::size_t consumed = 0;
    bool fatal = false;
    std::optional<NetProtocol> decoded;
    EXPECT_NO_THROW(decoded = NetProtocol::decode(bytes, consumed, &fatal));
    if (decoded.has_value()) {
        EXPECT_FALSE(fatal);
        EXPECT_GE(consumed, kFrameHeaderSize);
        EXPECT_LE(consumed, bytes.size());
    } else {
        EXPECT_EQ(consumed, 0u);
        if (bytes.size() >= kFrameHeaderSize) {
            const auto magic =
                static_cast<std::uint16_t>((std::uint16_t(bytes[0]) << 8) | bytes[1]);
            const auto declared =
                (std::uint32_t(bytes[2]) << 24) | (std::uint32_t(bytes[3]) << 16) |
                (std::uint32_t(bytes[4]) << 8) | std::uint32_t(bytes[5]);
            if (magic == kFrameMagic && declared > kMaxPayloadLen) {
                EXPECT_TRUE(fatal) << "oversize declared length must be fatal without waiting";
            }
        }
    }
}

class ProbeSession : public AbstractNetSession {
public:
    std::vector<NetProtocol> received;
    std::vector<std::string> errors;

    bool push(std::span<const std::uint8_t> chunk) { return feed(chunk); }

protected:
    void parseNetProtocol(NetProtocol data) override { received.push_back(std::move(data)); }
    void onProtocolError(std::string_view reason) override { errors.emplace_back(reason); }
};

} // namespace

TEST(ProtocolStress, EmptyAndTinyBuffersNeverThrow) {
    decodeOnce({});
    decodeOnce(std::vector<std::uint8_t>{0xBE});
    decodeOnce(std::vector<std::uint8_t>{0xBE, 0xEF});
    decodeOnce(std::vector<std::uint8_t>(7, 0x00));
    decodeOnce(std::vector<std::uint8_t>(8, 0x00));
    decodeOnce(std::vector<std::uint8_t>(8, 0xFF));
}

TEST(ProtocolStress, OversizeLengthHeaderOnlyIsFatal) {
    std::vector<std::uint8_t> header;
    put_be16(header, kFrameMagic);
    put_be32(header, kMaxPayloadLen + 1);
    put_be16(header, kNetworkVersionWire);
    ASSERT_EQ(header.size(), kFrameHeaderSize);
    std::size_t consumed = 99;
    bool fatal = false;
    const auto decoded = NetProtocol::decode(header, consumed, &fatal);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_TRUE(fatal);
    EXPECT_EQ(consumed, 0u);
}

TEST(ProtocolStress, GarbageAndBeefPrefixedRandom) {
    std::mt19937 rng(20260813);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, 512);

    for (int i = 0; i < 400; ++i) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : bytes) {
            b = static_cast<std::uint8_t>(byte_dist(rng));
        }
        decodeOnce(bytes);
    }

    for (int i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> bytes;
        put_be16(bytes, kFrameMagic);
        put_be32(bytes, static_cast<std::uint32_t>(byte_dist(rng)) * 1024u);
        put_be16(bytes, static_cast<std::uint16_t>(byte_dist(rng) << 8 | byte_dist(rng)));
        const int extra = len_dist(rng);
        for (int n = 0; n < extra; ++n) {
            bytes.push_back(static_cast<std::uint8_t>(byte_dist(rng)));
        }
        decodeOnce(bytes);
    }
}

TEST(ProtocolStress, SessionFeedNeverThrowsOnGarbage) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(1, 128);

    for (int i = 0; i < 100; ++i) {
        ProbeSession session;
        std::vector<std::uint8_t> chunk(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : chunk) {
            b = static_cast<std::uint8_t>(byte_dist(rng));
        }
        EXPECT_NO_THROW({
            const bool ok = session.push(chunk);
            if (!ok) {
                EXPECT_FALSE(session.errors.empty());
            }
        });
    }
}

TEST(ProtocolStress, ValidFrameSurvivesLeadingSplitAndTrailingGarbage) {
    ProbeSession session;
    auto frame = NetProtocol(Opcode::Read, {}, 9).encode();
    const auto first = frame.size() / 2;
    EXPECT_TRUE(session.push(std::span(frame.data(), first)));
    EXPECT_TRUE(session.received.empty());

    std::vector<std::uint8_t> rest(frame.begin() + static_cast<std::ptrdiff_t>(first), frame.end());
    rest.insert(rest.end(), {0xDE, 0xAD, 0x00});
    EXPECT_TRUE(session.push(rest));
    ASSERT_EQ(session.received.size(), 1u);
    EXPECT_EQ(session.received[0].getMsgType(), Opcode::Read);
    EXPECT_EQ(session.received[0].seq(), 9u);
}
