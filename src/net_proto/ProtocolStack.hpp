#pragma once

#include "NetProtocol.hpp"

#include <functional>
#include <utility>
#include <vector>

// Ordered parse layers, keyed by peer protocol major.
//
// Usual stack: addLayer(0, Vx) then addLayer(1, V1, kProtoMajor). Later majors
// add as addLayer(2, V2, 2). Layers with max_major < 0 have no upper bound.
// dispatch() walks in add order; first true stops the walk.

class ProtocolStack {
public:
    using ParseFn = std::function<bool(const NetProtocol& data)>;

    void addLayer(int min_major, ParseFn parse, int max_major = -1) {
        parse_layers_.emplace_back(min_major, max_major, std::move(parse));
    }

    bool dispatch(const NetProtocol& data) const {
        const int protocol_major = versionMajor(data.getVersion());
        bool consumed = false;
        for (const auto& [min_major, max_major, parse] : parse_layers_) {
            if (consumed) {
                break;
            }
            const bool in_range =
                (min_major == 0) ||
                (protocol_major >= min_major && (max_major < 0 || protocol_major <= max_major));
            if (in_range) {
                consumed = parse(data);
            }
        }
        return consumed;
    }

private:
    struct Layer {
        int min_major;
        int max_major;
        ParseFn parse;
        Layer(int min_major_in, int max_major_in, ParseFn parse_in)
            : min_major(min_major_in), max_major(max_major_in), parse(std::move(parse_in)) {}
    };
    std::vector<Layer> parse_layers_;
};
