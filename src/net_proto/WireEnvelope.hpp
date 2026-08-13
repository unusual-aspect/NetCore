#pragma once

#include <cstdint>
#include <string>

// JSON payload after the 0xBEEF header. Version lives in the frame header (Opcode.hpp).
// Field names are the wire keys (reflectcpp).

struct WireEnvelope {
    std::string Type;       // magic_enum name of Opcode
    std::uint32_t Seq = 0;  // echoed on Ok so a reply can match the request
    std::string Data;       // payload / message body
};
