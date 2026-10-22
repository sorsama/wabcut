#pragma once

// A minimal DER writer and reader for the local signing identity (X.509 v3 certificates, PKCS#8 keys, PEM). Only what
// Identity.cpp needs: no BER, no indefinite lengths, lengths below 2^32.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::c2pa::der {

using Bytes = std::vector<std::uint8_t>;

namespace tag {
inline constexpr std::uint8_t Boolean = 0x01, Integer = 0x02, BitString = 0x03, OctetString = 0x04, Null = 0x05, Oid = 0x06,
                              Utf8String = 0x0C, PrintableString = 0x13, UtcTime = 0x17, GeneralizedTime = 0x18,
                              Sequence = 0x30, Set = 0x31;
constexpr std::uint8_t context(int n, bool constructed = true) { return static_cast<std::uint8_t>((constructed ? 0xA0 : 0x80) | n); }
} // namespace tag

Bytes tlv(std::uint8_t t, std::span<const std::uint8_t> content);
Bytes sequence(std::initializer_list<Bytes> items);
Bytes sequence(const std::vector<Bytes>& items);
Bytes set(std::initializer_list<Bytes> items);
/// An unsigned big-endian integer (leading zeros dropped, a zero byte added when the top bit is set).
Bytes integer(std::span<const std::uint8_t> bigEndian);
Bytes integer(std::int64_t value);
Bytes oid(std::string_view dotted);
Bytes octetString(std::span<const std::uint8_t> bytes);
Bytes bitString(std::span<const std::uint8_t> bytes, std::uint8_t unusedBits = 0);
Bytes boolean(bool value);
Bytes utf8(std::string_view text);
/// UTCTime before 2050, GeneralizedTime from then on (RFC 5280 4.1.2.5); `unixSeconds` UTC.
Bytes time(std::int64_t unixSeconds);
Bytes explicitTag(int n, const Bytes& content);

/// One element: its tag, its content, and the whole encoding.
struct Element {
    std::uint8_t tag = 0;
    std::span<const std::uint8_t> content;
    std::span<const std::uint8_t> encoded;
};
/// The element at the start of `data`, or nullopt when it is not well-formed DER.
std::optional<Element> read(std::span<const std::uint8_t> data);
/// The elements of a constructed element's content, in order (nullopt when one is malformed).
std::optional<std::vector<Element>> children(std::span<const std::uint8_t> content);
/// "1.2.840.10045.2.1" from an OID's content.
std::string oidText(std::span<const std::uint8_t> content);
/// Seconds since 1970 (UTC) of a UTCTime / GeneralizedTime element; nullopt when malformed.
std::optional<std::int64_t> timeValue(const Element& e);

std::string base64(std::span<const std::uint8_t> bytes);
std::optional<Bytes> unbase64(std::string_view text);
/// "-----BEGIN <label>-----\n<base64, 64 per line>\n-----END <label>-----\n"
std::string pem(std::string_view label, std::span<const std::uint8_t> der);
/// The DER of every PEM block with `label` in `text`, in order.
std::vector<Bytes> pemBlocks(std::string_view text, std::string_view label);

/// Overwrites `bytes` with zeros in a way the compiler keeps.
void wipe(std::span<std::uint8_t> bytes);
void wipe(std::string& text);

} // namespace wabcut::ai::c2pa::der
