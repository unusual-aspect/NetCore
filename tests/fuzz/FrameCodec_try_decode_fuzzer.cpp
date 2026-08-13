#include "FrameCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::size_t consumed = 0;
    bool fatal = false;
    (void)FrameCodec::try_decode(std::span<const std::uint8_t>(data, size), consumed, &fatal);
    return 0;
}
