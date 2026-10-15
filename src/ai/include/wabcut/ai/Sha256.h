#pragma once

// SHA-256 (FIPS 180-4), streaming. Used to verify downloaded model weights and to hash analysis parameters.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::ai {

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::byte> data);
    void update(std::string_view data);
    /// The digest; the object must not be updated afterwards.
    std::array<std::uint8_t, 32> finish();
    /// finish() as 64 lowercase hex digits.
    std::string finishHex();

    static std::string hex(std::string_view data);

private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> m_state;
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffered = 0;
    std::uint64_t m_length = 0;
};

} // namespace wabcut::ai
