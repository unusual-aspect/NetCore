#include <gtest/gtest.h>

#include "AbstractNetSession.hpp"
#include "Dbg.hpp"
#include "FrameCodec.hpp"
#include "NetDefaults.hpp"
#include "NetProtocol.hpp"
#include "ProtocolLayers.hpp"
#include "ProtocolStack.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

// Honest frame: len field matches body.size(), version in header.
std::vector<std::uint8_t> rawFrame(std::uint16_t magic, std::uint16_t version_wire, std::string_view body) {
    std::vector<std::uint8_t> frame;
    frame.reserve(kFrameHeaderSize + body.size());
    put_be16(frame, magic);
    put_be32(frame, static_cast<std::uint32_t>(body.size()));
    put_be16(frame, version_wire);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

std::vector<std::uint8_t> rawFrame(std::uint16_t magic, std::string_view body) {
    return rawFrame(magic, kNetworkVersionWire, body);
}

// Lie about length — used for oversize / incomplete-body cases.
std::vector<std::uint8_t> rawFrameLen(
    std::uint16_t magic, std::uint16_t version_wire, std::uint32_t declared_len, std::string_view body) {
    std::vector<std::uint8_t> frame;
    put_be16(frame, magic);
    put_be32(frame, declared_len);
    put_be16(frame, version_wire);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

std::vector<std::uint8_t> rawFrameLen(std::uint16_t magic, std::uint32_t declared_len, std::string_view body) {
    return rawFrameLen(magic, kNetworkVersionWire, declared_len, body);
}

std::string envelopeJson(std::string_view type, std::uint32_t seq, std::string_view data) {
    return std::string(R"({"Type":")") + std::string(type) +
           R"(","Seq":)" + std::to_string(seq) +
           R"(,"Data":")" + std::string(data) + R"("})";
}

std::optional<NetProtocol> decodeAll(const std::vector<std::uint8_t>& frame, bool* fatal = nullptr) {
    std::size_t consumed = 0;
    return NetProtocol::decode(frame, consumed, fatal);
}

// Session with no socket — drive feed() from tests.
class ProbeSession : public AbstractNetSession {
public:
    std::vector<NetProtocol> received;
    std::vector<std::string> errors;

    bool push(std::span<const std::uint8_t> chunk) { return feed(chunk); }

protected:
    void parseNetProtocol(NetProtocol data) override {
        received.push_back(std::move(data));
    }

    void onProtocolError(std::string_view reason) override {
        errors.emplace_back(reason);
    }
};

} // namespace

TEST(ProtocolRoundTrip, EncodeDecodeRead) {
    const NetProtocol original(Opcode::Read);
    std::size_t consumed = 0;
    bool fatal = true;
    const auto decoded = NetProtocol::decode(original.encode(), consumed, &fatal);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(consumed, original.encode().size());
    EXPECT_EQ(decoded->getMsgType(), Opcode::Read);
    EXPECT_EQ(decoded->getType(), "Read");
    EXPECT_TRUE(decoded->isEmpty());
    EXPECT_EQ(decoded->getVersion(), kNetworkVersion);
    EXPECT_TRUE(decoded->compatible());
    EXPECT_TRUE(decoded->getInvalidType().empty());
}

TEST(ProtocolRoundTrip, EncodeDecodeSetPayloadAndSeq) {
    const NetProtocol original(Opcode::Set, "hello world", 42);
    std::size_t consumed = 0;
    const auto decoded = NetProtocol::decode(original.encode(), consumed);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->getMsgType(), Opcode::Set);
    EXPECT_EQ(decoded->getMsgData(), "hello world");
    EXPECT_EQ(decoded->seq(), 42u);
}

TEST(ProtocolRoundTrip, AllOpcodesExceptNone) {
    const Opcode opcodes[] = {
        Opcode::Read, Opcode::Set, Opcode::Shutdown,
        Opcode::VersionSrv, Opcode::Ok, Opcode::Error, Opcode::LastMsgType
    };
    for (auto opcode : opcodes) {
        const NetProtocol original(opcode, "x", 7);
        std::size_t consumed = 0;
        bool fatal = false;
        const auto decoded = NetProtocol::decode(original.encode(), consumed, &fatal);
        ASSERT_TRUE(decoded.has_value()) << magic_enum::enum_name(opcode);
        EXPECT_FALSE(fatal);
        EXPECT_EQ(decoded->getMsgType(), opcode);
        EXPECT_EQ(decoded->getMsgData(), "x");
        EXPECT_EQ(decoded->seq(), 7u);
    }
}

TEST(ProtocolRoundTrip, EmptyPayloadOk) {
    const auto decoded = decodeAll(NetProtocol(Opcode::Ok).encode());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->isEmpty());
    EXPECT_EQ(decoded->getMsgType(), Opcode::Ok);
}

TEST(ProtocolRoundTrip, PayloadWithQuotesAndNewlines) {
    const std::string payload = "line1\n\"quoted\"\t\\end";
    const auto encoded = NetProtocol(Opcode::Set, payload, 1).encode();
    const auto decoded = decodeAll(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->getMsgData(), payload);
}

TEST(ProtocolInvalid, UnknownTypeIsNotFatal) {
    const auto frame = rawFrame(kFrameMagic, envelopeJson("NotARealOp", 3, "keep"));
    bool fatal = true;
    std::size_t consumed = 0;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(decoded->getMsgType(), Opcode::None);
    EXPECT_EQ(decoded->getInvalidType(), "NotARealOp");
    EXPECT_EQ(decoded->getMsgData(), "keep");
    EXPECT_EQ(decoded->seq(), 3u);
}

TEST(ProtocolInvalid, UnknownTypeRoundTrips) {
    const auto first = decodeAll(
        rawFrame(kFrameMagic, envelopeJson("FooBar", 0, "z")));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->getInvalidType(), "FooBar");

    const auto second = decodeAll(first->encode());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->getMsgType(), Opcode::None);
    EXPECT_EQ(second->getInvalidType(), "FooBar");
    EXPECT_EQ(second->getMsgData(), "z");
}

TEST(ProtocolInvalid, EmptyTypeIsNone) {
    bool fatal = true;
    const auto decoded = decodeAll(rawFrame(kFrameMagic, envelopeJson("", 0, "")), &fatal);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(decoded->getMsgType(), Opcode::None);
}

TEST(ProtocolInvalid, BadMagicIsFatal) {
    const auto frame = rawFrame(0xDEAD, envelopeJson("Read", 0, ""));
    bool fatal = false;
    std::size_t consumed = 0;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);

    EXPECT_FALSE(decoded.has_value());
    EXPECT_TRUE(fatal);
    EXPECT_EQ(consumed, 0u);
}

TEST(ProtocolInvalid, LittleEndianBeefIsFatal) {
    std::vector<std::uint8_t> frame{0xEF, 0xBE, 0, 0, 0, 0, 0, 0};
    bool fatal = false;
    EXPECT_FALSE(decodeAll(frame, &fatal).has_value());
    EXPECT_TRUE(fatal);
}

TEST(ProtocolInvalid, DeclaredLenOverMaxIsFatalWithoutBody) {
    const auto frame = rawFrameLen(kFrameMagic, kMaxPayloadLen + 1, {});
    ASSERT_EQ(frame.size(), kFrameHeaderSize);
    bool fatal = false;
    std::size_t consumed = 0;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_TRUE(fatal);
}

TEST(ProtocolInvalid, InvalidJsonIsFatal) {
    const auto frame = rawFrame(kFrameMagic, "{not json");
    bool fatal = false;
    EXPECT_FALSE(decodeAll(frame, &fatal).has_value());
    EXPECT_TRUE(fatal);
}

TEST(ProtocolInvalid, EmptyBodyIsFatal) {
    // Zero-length wire payload (no JSON). Not the same as Set with empty Data.
    const auto frame = rawFrame(kFrameMagic, {});
    bool fatal = false;
    EXPECT_FALSE(decodeAll(frame, &fatal).has_value());
    EXPECT_TRUE(fatal);
}

TEST(ProtocolInvalid, ZeroDeclaredPayloadLengthIsFatal) {
    const auto frame = rawFrameLen(kFrameMagic, 0, {});
    ASSERT_EQ(frame.size(), kFrameHeaderSize);
    bool fatal = false;
    std::size_t consumed = 99;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_TRUE(fatal);
    EXPECT_EQ(consumed, 0u);
}

TEST(ProtocolInvalid, MessageBodyMayContainBraces) {
    const std::string json = R"({"Type":"Set","Seq":1,"Data":"{{{{[[[["})";
    bool fatal = true;
    const auto decoded = decodeAll(rawFrame(kFrameMagic, json), &fatal);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(decoded->getMsgType(), Opcode::Set);
    EXPECT_EQ(decoded->getMsgData(), "{{{{[[[[");
}

TEST(DbgEvent, FormatIsStableAndRedactsByDefault) {
    const bool previous = netdbg::isVerbose();
    netdbg::setVerbose(true);
    const auto verbose = netdbg::event("READ", "127.0.0.1", "hello");
    EXPECT_NE(verbose.find("TIME="), std::string::npos);
    EXPECT_NE(verbose.find(" UTC"), std::string::npos);
    EXPECT_NE(verbose.find("IP=127.0.0.1"), std::string::npos);
    EXPECT_NE(verbose.find("OP=READ"), std::string::npos);
    EXPECT_NE(verbose.find("MSG=hello"), std::string::npos);

    netdbg::setVerbose(false);
    const auto redacted = netdbg::event("SET", "10.0.0.1", "secret");
    EXPECT_NE(redacted.find("IP=10.0.0.1"), std::string::npos);
    EXPECT_NE(redacted.find("OP=SET"), std::string::npos);
    EXPECT_NE(redacted.find("MSG=<redacted len=6>"), std::string::npos);
    EXPECT_EQ(redacted.find("secret"), std::string::npos);
    netdbg::setVerbose(previous);
}

TEST(ProtocolInvalid, TruncatedJsonIsFatal) {
    const auto frame = rawFrame(kFrameMagic, R"({"Type":"Read")");
    bool fatal = false;
    EXPECT_FALSE(decodeAll(frame, &fatal).has_value());
    EXPECT_TRUE(fatal);
}

TEST(ProtocolEdge, IncompleteHeaderWaits) {
    std::vector<std::uint8_t> frame{0xBE, 0xEF, 0, 0, 0, 0, 0x01};
    bool fatal = true;
    std::size_t consumed = 99;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(consumed, 0u);
}

TEST(ProtocolEdge, IncompleteBodyWaits) {
    const auto frame = rawFrameLen(kFrameMagic, 64, R"({"Type":")");
    bool fatal = true;
    std::size_t consumed = 99;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(consumed, 0u);
}

TEST(ProtocolEdge, EmptyBufferWaits) {
    bool fatal = true;
    std::size_t consumed = 99;
    const auto decoded = NetProtocol::decode({}, consumed, &fatal);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(consumed, 0u);
}

TEST(ProtocolEdge, ExtraBytesAfterFrame) {
    auto frame = NetProtocol(Opcode::Read, {}, 1).encode();
    const auto first_size = frame.size();
    frame.insert(frame.end(), {0x00, 0x01, 0x02});

    std::size_t consumed = 0;
    bool fatal = true;
    const auto decoded = NetProtocol::decode(frame, consumed, &fatal);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(consumed, first_size);
    EXPECT_EQ(decoded->getMsgType(), Opcode::Read);
}

TEST(ProtocolEdge, TwoFramesConcatenated) {
    auto first_frame = NetProtocol(Opcode::Set, "one", 1).encode();
    auto second_frame = NetProtocol(Opcode::Read, {}, 2).encode();
    first_frame.insert(first_frame.end(), second_frame.begin(), second_frame.end());

    std::size_t consumed = 0;
    const auto first = NetProtocol::decode(first_frame, consumed);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->getMsgType(), Opcode::Set);
    EXPECT_EQ(first->getMsgData(), "one");

    std::vector<std::uint8_t> remainder(
        first_frame.begin() + static_cast<std::ptrdiff_t>(consumed), first_frame.end());
    consumed = 0;
    const auto second = NetProtocol::decode(remainder, consumed);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->getMsgType(), Opcode::Read);
    EXPECT_EQ(second->seq(), 2u);
}

TEST(ProtocolEdge, VersionMismatchIsNotFatal) {
    const auto frame = rawFrame(kFrameMagic, versionToWire("2.0"), envelopeJson("Read", 0, ""));
    bool fatal = true;
    const auto decoded = decodeAll(frame, &fatal);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);
    EXPECT_EQ(decoded->getVersion(), "2.0");
    EXPECT_FALSE(decoded->compatible());
    EXPECT_EQ(decoded->getMsgType(), Opcode::Read);
}

TEST(ProtocolEdge, OldMinorStillCompatible) {
    const auto frame = rawFrame(kFrameMagic, versionToWire("1.9"), envelopeJson("Set", 0, "x"));
    const auto decoded = decodeAll(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->compatible());
    EXPECT_EQ(decoded->getMsgType(), Opcode::Set);
}

TEST(ProtocolEdge, WireVersionRoundTrip) {
    const auto frame = rawFrame(kFrameMagic, versionToWire("3.7"), envelopeJson("Read", 0, ""));
    const auto decoded = decodeAll(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->getVersion(), "3.7");
    EXPECT_EQ(decoded->getMsgType(), Opcode::Read);
}

TEST(ProtocolEdge, SeqRoundTripViaWire) {
    const auto frame = rawFrame(kFrameMagic, envelopeJson("Ok", 9, "hi"));
    const auto decoded = decodeAll(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->seq(), 9u);
    EXPECT_EQ(decoded->getMsgData(), "hi");
}

TEST(VersionMajor, ParsesKnownShapes) {
    EXPECT_EQ(versionMajor("1.0"), 1);
    EXPECT_EQ(versionMajor("1.9"), 1);
    EXPECT_EQ(versionMajor("2.5"), 2);
    EXPECT_EQ(versionMajor("10.1"), 10);
    EXPECT_EQ(versionMajor("3"), 3);
    EXPECT_EQ(versionMajor(""), 0);
    EXPECT_EQ(versionMajor("abc"), 0);
    EXPECT_EQ(versionMajor(".1"), 0);
}

TEST(VersionWire, RoundTripKnownShapes) {
    EXPECT_EQ(versionToWire("1.0"), 0x0100u);
    EXPECT_EQ(versionToWire("1.9"), 0x0109u);
    EXPECT_EQ(versionToWire("2.0"), 0x0200u);
    EXPECT_EQ(versionToWire("10.1"), 0x0A01u);
    EXPECT_EQ(versionFromWire(0x0100), "1.0");
    EXPECT_EQ(versionFromWire(0x0909), "9.9");
}

TEST(TypeFromName, KnownAndUnknown) {
    EXPECT_EQ(NetProtocol::typeFromName("Read"), Opcode::Read);
    EXPECT_EQ(NetProtocol::typeFromName("Set"), Opcode::Set);
    EXPECT_EQ(NetProtocol::typeFromName("Shutdown"), Opcode::Shutdown);

    std::string invalid_type;
    EXPECT_EQ(NetProtocol::typeFromName("nope", &invalid_type), Opcode::None);
    EXPECT_EQ(invalid_type, "nope");

    invalid_type.clear();
    EXPECT_EQ(NetProtocol::typeFromName("read", &invalid_type), Opcode::None);
    EXPECT_EQ(invalid_type, "read");
}

TEST(ProtocolStack, VxSwallowsBeforeV1) {
    int vx_hits = 0;
    int v1_hits = 0;
    ProtocolStack protocol_stack;
    protocol_stack.addLayer(0, [&](const NetProtocol& data) {
        if (data.getMsgType() == Opcode::Error) {
            ++vx_hits;
            return true;
        }
        return false;
    });
    protocol_stack.addLayer(1, [&](const NetProtocol&) {
        ++v1_hits;
        return true;
    });

    EXPECT_TRUE(protocol_stack.dispatch(NetProtocol(Opcode::Error, "boom")));
    EXPECT_EQ(vx_hits, 1);
    EXPECT_EQ(v1_hits, 0);

    EXPECT_TRUE(protocol_stack.dispatch(NetProtocol(Opcode::Read)));
    EXPECT_EQ(vx_hits, 1);
    EXPECT_EQ(v1_hits, 1);
}

TEST(ProtocolStack, V1SkippedWhenMajorBelowOne) {
    int v1_hits = 0;
    ProtocolStack protocol_stack;
    protocol_stack.addLayer(1, [&](const NetProtocol&) {
        ++v1_hits;
        return true;
    });

    const auto old = decodeAll(rawFrame(kFrameMagic, versionToWire("0.9"), envelopeJson("Read", 0, "")));
    ASSERT_TRUE(old.has_value());
    EXPECT_FALSE(protocol_stack.dispatch(*old));
    EXPECT_EQ(v1_hits, 0);
}

TEST(ProtocolStack, UnrecognizedReturnsFalse) {
    ProtocolStack protocol_stack;
    protocol_stack.addLayer(0, [](const NetProtocol&) { return false; });
    protocol_stack.addLayer(1, [](const NetProtocol&) { return false; });
    EXPECT_FALSE(protocol_stack.dispatch(NetProtocol(Opcode::Read)));
}

TEST(ProtocolStack, EmptyStackReturnsFalse) {
    ProtocolStack protocol_stack;
    EXPECT_FALSE(protocol_stack.dispatch(NetProtocol(Opcode::Read)));
}

TEST(ProtocolLayers, StoreFailureMapsToError) {
    std::vector<NetProtocol> sent;
    auto stack = protocol::makeServerStack({
        .peer = [] { return std::string{"127.0.0.1"}; },
        .send = [&](const NetProtocol& msg) {
            sent.push_back(msg);
            return true;
        },
        .onRead = [](auto done) { done(false, "disk full"); },
        .onSet =
            [](std::string_view, auto done) {
                done(false, "disk full");
            },
        .onShutdown = {},
    });

    EXPECT_TRUE(stack.dispatch(NetProtocol(Opcode::Read)));
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].getMsgType(), Opcode::Error);
    EXPECT_EQ(sent[0].getMsgData(), kStoreUnavailableMsg);

    sent.clear();
    EXPECT_TRUE(stack.dispatch(NetProtocol(Opcode::Set, "x")));
    ASSERT_EQ(sent.size(), 1u);
    // cppcheck-suppress containerOutOfBounds // cppcheck doesn't model the send lambda populating sent before the ASSERT_EQ guard runs.
    EXPECT_EQ(sent[0].getMsgType(), Opcode::Error);
    // cppcheck-suppress containerOutOfBounds // cppcheck doesn't model the send lambda populating sent before the ASSERT_EQ guard runs.
    EXPECT_EQ(sent[0].getMsgData(), kStoreUnavailableMsg);
}

TEST(ProtocolLayers, UnsupportedMajorGetsError) {
    std::vector<NetProtocol> sent;
    auto stack = protocol::makeServerStack({
        .peer = [] { return std::string{"127.0.0.1"}; },
        .send =
            [&](const NetProtocol& msg) {
                sent.push_back(msg);
                return true;
            },
        .onRead = [](auto done) { done(true, "should-not-run"); },
        .onSet = {},
        .onShutdown = {},
    });

    auto frame = rawFrame(kFrameMagic, versionToWire("2.0"), envelopeJson("Read", 7, ""));
    bool fatal = true;
    auto decoded = decodeAll(frame, &fatal);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(fatal);

    EXPECT_TRUE(stack.dispatch(*decoded));
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].getMsgType(), Opcode::Error);
    EXPECT_EQ(sent[0].getMsgData(), "unsupported protocol major");
    EXPECT_EQ(sent[0].seq(), 7u);
}

TEST(ProtocolLayers, ReadWithSqlParamsStillReturnsStoreBody) {
    std::vector<NetProtocol> sent;
    int reads = 0;
    auto stack = protocol::makeServerStack({
        .peer = [] { return std::string{"127.0.0.1"}; },
        .send =
            [&](const NetProtocol& msg) {
                sent.push_back(msg);
                return true;
            },
        .onRead =
            [&](auto done) {
                ++reads;
                done(true, "from-store");
            },
        .onSet = {},
        .onShutdown = {},
    });

    EXPECT_TRUE(stack.dispatch(NetProtocol(Opcode::Read, "'; DROP TABLE message;--", 4)));
    EXPECT_EQ(reads, 1);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].getMsgType(), Opcode::Ok);
    EXPECT_EQ(sent[0].getMsgData(), "from-store");
    EXPECT_EQ(sent[0].seq(), 4u);
}

TEST(ProtocolLayers, EmptySetIsIgnored) {
    std::vector<NetProtocol> sent;
    bool set_called = false;
    auto stack = protocol::makeServerStack({
        .peer = [] { return std::string{"127.0.0.1"}; },
        .send =
            [&](const NetProtocol& msg) {
                sent.push_back(msg);
                return true;
            },
        .onRead = {},
        .onSet =
            [&](std::string_view, auto done) {
                set_called = true;
                done(true, {});
            },
        .onShutdown = {},
    });

    EXPECT_TRUE(stack.dispatch(NetProtocol(Opcode::Set, {}, 9)));
    EXPECT_FALSE(set_called);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].getMsgType(), Opcode::Ok);
    EXPECT_TRUE(sent[0].getMsgData().empty());
    EXPECT_EQ(sent[0].seq(), 9u);
}

TEST(ProtocolLayers, OversizeSetMapsToError) {
    std::vector<NetProtocol> sent;
    bool set_called = false;
    auto stack = protocol::makeServerStack({
        .peer = [] { return std::string{"127.0.0.1"}; },
        .send =
            [&](const NetProtocol& msg) {
                sent.push_back(msg);
                return true;
            },
        .onRead = {},
        .onSet =
            [&](std::string_view, auto done) {
                set_called = true;
                done(true, {});
            },
        .onShutdown = {},
    });

    const std::string huge(kMaxMessageBytes + 1, 'Y');
    EXPECT_TRUE(stack.dispatch(NetProtocol(Opcode::Set, huge, 3)));
    EXPECT_FALSE(set_called);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].getMsgType(), Opcode::Error);
    EXPECT_EQ(sent[0].getMsgData(), "message too large");
    EXPECT_EQ(sent[0].seq(), 3u);
}

TEST(ProtocolLayers, MakeClientRequestRejectsOversizeSet) {
    const std::string huge(kMaxMessageBytes + 1, 'Z');
    EXPECT_FALSE(protocol::makeClientRequest(Opcode::Set, huge).has_value());
    EXPECT_TRUE(protocol::makeClientRequest(Opcode::Set, "ok").has_value());
    // Client may still send empty Data; the server ignores the insert.
    const auto empty_set = protocol::makeClientRequest(Opcode::Set, {});
    ASSERT_TRUE(empty_set.has_value());
    EXPECT_EQ(empty_set->getMsgType(), Opcode::Set);
    EXPECT_TRUE(empty_set->getMsgData().empty());
}

TEST(ProtocolStack, V1CappedAtProtoMajor) {
    int v1_hits = 0;
    ProtocolStack protocol_stack;
    protocol_stack.addLayer(1, [&](const NetProtocol&) {
        ++v1_hits;
        return true;
    }, kProtoMajor);

    auto high = decodeAll(rawFrame(kFrameMagic, versionToWire("2.0"), envelopeJson("Read", 0, "")));
    ASSERT_TRUE(high.has_value());
    EXPECT_FALSE(protocol_stack.dispatch(*high));
    EXPECT_EQ(v1_hits, 0);

    EXPECT_TRUE(protocol_stack.dispatch(NetProtocol(Opcode::Read)));
    EXPECT_EQ(v1_hits, 1);
}

TEST(SessionFeed, PartialThenComplete) {
    ProbeSession session;
    auto frame = NetProtocol(Opcode::Set, "abc", 5).encode();

    EXPECT_TRUE(session.push(std::span(frame.data(), 4)));
    EXPECT_TRUE(session.received.empty());
    EXPECT_TRUE(session.errors.empty());

    EXPECT_TRUE(session.push(std::span(frame.data() + 4, frame.size() - 4)));
    ASSERT_EQ(session.received.size(), 1u);
    EXPECT_EQ(session.received[0].getMsgType(), Opcode::Set);
    EXPECT_EQ(session.received[0].getMsgData(), "abc");
    EXPECT_EQ(session.received[0].seq(), 5u);
}

TEST(SessionFeed, TwoFramesOneChunk) {
    ProbeSession session;
    auto first_frame = NetProtocol(Opcode::Read).encode();
    auto second_frame = NetProtocol(Opcode::Shutdown).encode();
    first_frame.insert(first_frame.end(), second_frame.begin(), second_frame.end());

    EXPECT_TRUE(session.push(first_frame));
    ASSERT_EQ(session.received.size(), 2u);
    EXPECT_EQ(session.received[0].getMsgType(), Opcode::Read);
    EXPECT_EQ(session.received[1].getMsgType(), Opcode::Shutdown);
}

TEST(SessionFeed, BadMagicIsFatalAndClears) {
    ProbeSession session;
    auto invalid_frame = rawFrame(0x0000, "xxxx");
    EXPECT_FALSE(session.push(invalid_frame));
    EXPECT_TRUE(session.received.empty());
    ASSERT_EQ(session.errors.size(), 1u);
}

TEST(SessionFeed, UnknownTypeStillDispatched) {
    ProbeSession session;
    auto frame = rawFrame(kFrameMagic, envelopeJson("Ghost", 0, "n"));
    EXPECT_TRUE(session.push(frame));
    ASSERT_EQ(session.received.size(), 1u);
    EXPECT_EQ(session.received[0].getMsgType(), Opcode::None);
    EXPECT_EQ(session.received[0].getInvalidType(), "Ghost");
    EXPECT_TRUE(session.errors.empty());
}

TEST(SessionFeed, VersionMismatchStillDispatched) {
    ProbeSession session;
    auto frame = rawFrame(kFrameMagic, versionToWire("9.9"), envelopeJson("Ok", 0, "v"));
    EXPECT_TRUE(session.push(frame));
    ASSERT_EQ(session.received.size(), 1u);
    EXPECT_FALSE(session.received[0].compatible());
    EXPECT_EQ(session.received[0].getMsgType(), Opcode::Ok);
    EXPECT_TRUE(session.errors.empty());
}

TEST(SessionFeed, InvalidJsonIsFatal) {
    ProbeSession session;
    EXPECT_FALSE(session.push(rawFrame(kFrameMagic, "[[[") ));
    EXPECT_TRUE(session.received.empty());
    EXPECT_FALSE(session.errors.empty());
}

TEST(SessionFeed, ByteAtATime) {
    ProbeSession session;
    auto frame = NetProtocol(Opcode::VersionSrv, kNetworkVersion).encode();
    for (std::size_t index = 0; index < frame.size(); ++index) {
        const bool accepted = session.push(std::span(frame.data() + index, 1));
        EXPECT_TRUE(accepted);
        if (index + 1 < frame.size()) {
            EXPECT_TRUE(session.received.empty());
        }
    }
    ASSERT_EQ(session.received.size(), 1u);
    EXPECT_EQ(session.received[0].getMsgType(), Opcode::VersionSrv);
    EXPECT_EQ(session.received[0].getMsgData(), kNetworkVersion);
}
