#include <wabcut/ai/Sha256.h>

#include <bit>
#include <cstring>

namespace wabcut::ai {

namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

} // namespace

Sha256::Sha256()
    : m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const std::uint8_t* p) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
                                         (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = m_state;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(std::span<const std::byte> data) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t n = data.size();
    m_length += n;
    if (m_buffered > 0) {
        const std::size_t take = std::min(n, 64 - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, p, take);
        m_buffered += take;
        p += take;
        n -= take;
        if (m_buffered < 64) {
            return;
        }
        block(m_buffer.data());
        m_buffered = 0;
    }
    for (; n >= 64; n -= 64, p += 64) {
        block(p);
    }
    std::memcpy(m_buffer.data(), p, n);
    m_buffered = n;
}

void Sha256::update(std::string_view data) { update(std::as_bytes(std::span(data.data(), data.size()))); }

std::array<std::uint8_t, 32> Sha256::finish() {
    const std::uint64_t bits = m_length * 8;
    const std::uint8_t one = 0x80;
    update(std::as_bytes(std::span(&one, 1)));
    const std::uint8_t zero = 0;
    while (m_buffered != 56) {
        update(std::as_bytes(std::span(&zero, 1)));
    }
    std::array<std::uint8_t, 8> length{};
    for (int i = 0; i < 8; ++i) {
        length[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    update(std::as_bytes(std::span(length)));
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[4 * i] = static_cast<std::uint8_t>(m_state[i] >> 24);
        digest[4 * i + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        digest[4 * i + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        digest[4 * i + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    return digest;
}

std::string Sha256::finishHex() {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : finish()) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0x0F];
    }
    return out;
}

std::string Sha256::hex(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.finishHex();
}

} // namespace wabcut::ai
