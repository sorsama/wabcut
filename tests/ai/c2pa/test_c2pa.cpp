// AI-15: Content Credentials - the local signing identity (CNG, the C2PA certificate profile), the keychain, the
// manifest of an export, and signing / verification / tamper detection with the same c2pa library. Signing runs on a
// tiny PNG made in memory (the library signs PNG like MP4), so this suite needs no media; the export suite signs a real
// MP4 export through ExportJob (tests/export/test_content_credentials.cpp).
#include "Der.h"
#include "TimelineFixture.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/c2pa/ContentCredentials.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <random>

using namespace wabcut;
using namespace wabcut::ai;
namespace der = wabcut::ai::c2pa::der;

namespace {

constexpr std::array<std::uint8_t, 74> kPng{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x29, 0xdc, 0x00, 0x00, 0x00, 0x11, 0x49,
    0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x38, 0x61, 0xa3, 0x81, 0x15, 0x31, 0x0c, 0x2d, 0x09, 0x00, 0x76, 0x7b, 0x4b,
    0x01, 0x66, 0xd4, 0x66, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

std::filesystem::path tempDir(const std::string& stem) {
    static std::mt19937_64 random{std::random_device{}()};
    auto dir = std::filesystem::temp_directory_path() / (stem + "-" + std::to_string(random()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream(file, std::ios::binary) << bytes;
}

std::filesystem::path writePng(const std::filesystem::path& dir, const std::string& name = "still.png") {
    const auto file = dir / name;
    writeAll(file, std::string_view(reinterpret_cast<const char*>(kPng.data()), kPng.size()));
    return file;
}

/// The base64 body of a PEM block (what would show up if the key were written somewhere).
std::string pemBody(std::string_view pem) {
    std::string body;
    std::size_t line = pem.find('\n') + 1;
    while (line < pem.size() && pem.substr(line, 5) != "-----") {
        const std::size_t next = pem.find('\n', line);
        body += pem.substr(line, std::min(next, pem.size()) - line);
        line = next + 1;
    }
    return body;
}

/// Every file below `dir` that contains `needle`.
std::vector<std::filesystem::path> filesContaining(const std::filesystem::path& dir, std::string_view needle) {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && readAll(e.path()).find(needle) != std::string::npos) {
            out.push_back(e.path());
        }
    }
    return out;
}

struct CertFields {
    std::vector<std::string> extensionOids;
    std::vector<std::string> ekus;
    bool ca = false;
    bool keyUsageCritical = false;
    std::uint8_t keyUsage = 0;
    std::string signatureAlgorithm;
    der::Bytes issuer;
    der::Bytes subject;
    der::Bytes subjectKeyId;
    der::Bytes authorityKeyId;
    int version = 0;
};

CertFields fields(const der::Bytes& certificate) {
    CertFields f;
    const auto cert = der::read(certificate);
    REQUIRE(cert);
    const auto parts = der::children(cert->content);
    REQUIRE(parts);
    REQUIRE(parts->size() == 3);
    f.signatureAlgorithm = der::oidText((*der::children((*parts)[1].content))[0].content);
    const auto tbs = der::children((*parts)[0].content);
    REQUIRE(tbs);
    REQUIRE((*tbs)[0].tag == der::tag::context(0));
    f.version = (*der::children((*tbs)[0].content))[0].content[0] + 1;
    f.issuer.assign((*tbs)[3].encoded.begin(), (*tbs)[3].encoded.end());
    f.subject.assign((*tbs)[5].encoded.begin(), (*tbs)[5].encoded.end());
    const auto extensions = der::children((*der::children(tbs->back().content))[0].content);
    REQUIRE(extensions);
    for (const der::Element& e : *extensions) {
        const auto ext = der::children(e.content);
        REQUIRE(ext);
        const std::string id = der::oidText((*ext)[0].content);
        f.extensionOids.push_back(id);
        const bool critical = ext->size() == 3;
        const auto value = (*ext)[critical ? 2 : 1].content;
        if (id == "2.5.29.19") {
            const auto bc = der::children(der::read(value)->content);
            f.ca = bc && !bc->empty() && (*bc)[0].tag == der::tag::Boolean && (*bc)[0].content[0] != 0;
        } else if (id == "2.5.29.15") {
            f.keyUsageCritical = critical;
            f.keyUsage = der::read(value)->content[1];
        } else if (id == "2.5.29.37") {
            for (const der::Element& o : *der::children(der::read(value)->content)) {
                f.ekus.push_back(der::oidText(o.content));
            }
        } else if (id == "2.5.29.14") {
            const auto v = der::read(value);
            f.subjectKeyId.assign(v->content.begin(), v->content.end());
        } else if (id == "2.5.29.35") {
            const auto v = (*der::children(der::read(value)->content))[0];
            f.authorityKeyId.assign(v.content.begin(), v.content.end());
        }
    }
    return f;
}

bool has(const std::vector<std::string>& list, std::string_view item) { return std::ranges::find(list, item) != list.end(); }

} // namespace

TEST_CASE("A local identity: a root and a C2PA-profile signing certificate it issues", "[ai][c2pa]") {
#ifndef _WIN32
    SKIP("the local identity is made with Windows CNG");
#endif
    auto identity = c2pa::generateIdentity();
    INFO((identity ? std::string() : identity.error().message));
    REQUIRE(identity);
    CHECK(identity->local);
    CHECK(identity->algorithm == "es256");
    CHECK(identity->subject.find("self-issued") != std::string::npos);
    const auto chain = der::pemBlocks(identity->certificateChainPem, "CERTIFICATE");
    const auto roots = der::pemBlocks(identity->rootPem, "CERTIFICATE");
    REQUIRE(chain.size() == 1); // the signing certificate only: the root is the anchor, not part of the chain
    REQUIRE(roots.size() == 1);
    const CertFields leaf = fields(chain[0]);
    const CertFields root = fields(roots[0]);
    // The C2PA certificate profile (c2pa-rs check_certificate_profile): v3, ECDSA with SHA-256, an end entity that is
    // not self-signed, digitalSignature only, an allowed EKU and no "any", an authority key identifier.
    CHECK(leaf.version == 3);
    CHECK(leaf.signatureAlgorithm == "1.2.840.10045.4.3.2");
    CHECK_FALSE(leaf.ca);
    CHECK(leaf.issuer == root.subject);
    CHECK(leaf.issuer != leaf.subject);
    CHECK(leaf.keyUsageCritical);
    CHECK(leaf.keyUsage == 0x80);
    CHECK(has(leaf.ekus, "1.3.6.1.5.5.7.3.36"));
    CHECK(has(leaf.ekus, "1.3.6.1.4.1.62558.2.1"));
    CHECK_FALSE(has(leaf.ekus, "2.5.29.37.0"));
    CHECK(leaf.authorityKeyId == root.subjectKeyId);
    CHECK(root.ca);
    CHECK(root.keyUsage == 0x06);
    CHECK(root.issuer == root.subject);
    // The key is PKCS#8 P-256 and belongs to the certificate.
    CHECK(identity->privateKeyPem.reveal().starts_with("-----BEGIN PRIVATE KEY-----\n"));
    auto again = c2pa::identityFromPem(identity->certificateChainPem, identity->privateKeyPem.clone());
    REQUIRE(again);
    CHECK(again->algorithm == "es256");
    // Two identities never share a key or a certificate.
    auto other = c2pa::generateIdentity();
    REQUIRE(other);
    CHECK(other->privateKeyPem.reveal() != identity->privateKeyPem.reveal());
    CHECK(other->rootPem != identity->rootPem);
}

TEST_CASE("User certificates that are mismatched or not PKCS 8 or expired are refused", "[ai][c2pa]") {
#ifndef _WIN32
    SKIP("the identities are made with Windows CNG");
#endif
    auto a = c2pa::generateIdentity();
    auto b = c2pa::generateIdentity();
    REQUIRE(a);
    REQUIRE(b);
    auto mismatched = c2pa::identityFromPem(a->certificateChainPem, b->privateKeyPem.clone());
    REQUIRE_FALSE(mismatched);
    CHECK(mismatched.error().message.find("does not belong") != std::string::npos);
    CHECK(mismatched.error().message.find("PRIVATE KEY-----") == std::string::npos);
    auto sec1 = c2pa::identityFromPem(a->certificateChainPem, Secret("-----BEGIN EC PRIVATE KEY-----\nAAAA\n-----END EC PRIVATE KEY-----\n"));
    REQUIRE_FALSE(sec1);
    CHECK(sec1.error().message.find("PKCS#8") != std::string::npos);
    CHECK_FALSE(c2pa::identityFromPem("no certificate here", a->privateKeyPem.clone()));
    auto old = c2pa::generateIdentity(std::chrono::system_clock::now() - std::chrono::days(400), std::chrono::days(30));
    REQUIRE(old);
    auto expired = c2pa::identityFromPem(old->certificateChainPem, old->privateKeyPem.clone());
    REQUIRE_FALSE(expired);
    CHECK(expired.error().message.find("expired") != std::string::npos);
}

TEST_CASE("The local identity keeps its key in the keychain and its certificates on disk and is reused", "[ai][c2pa]") {
#ifndef _WIN32
    SKIP("the local identity is made with Windows CNG");
#endif
    MemorySecretStore keys;
    const auto dir = tempDir("wabcut-c2pa-identity");
    CHECK_FALSE(c2pa::localIdentity(keys, dir, false)); // nothing yet, and not asked to make one
    auto first = c2pa::localIdentity(keys, dir);
    REQUIRE(first);
    CHECK(std::filesystem::exists(dir / "signing-certificate.pem"));
    CHECK(std::filesystem::exists(dir / "root-certificate.pem"));
    auto stored = keys.read(c2pa::kKeychainEntry);
    REQUIRE(stored);
    REQUIRE(*stored);
    CHECK((*stored)->reveal() == first->privateKeyPem.reveal());
    // The key is in the keychain only: no file holds it or its base64.
    const std::string body = pemBody(first->privateKeyPem.reveal());
    REQUIRE(body.size() > 100);
    CHECK(filesContaining(dir, "PRIVATE KEY").empty());
    CHECK(filesContaining(dir, body.substr(0, 40)).empty());
    // Reused while valid.
    auto second = c2pa::localIdentity(keys, dir);
    REQUIRE(second);
    CHECK(second->certificateChainPem == first->certificateChainPem);
    CHECK(second->rootPem == first->rootPem);
    // A key that no longer matches the certificate on disk (or a lost key) makes a new identity.
    REQUIRE(keys.write(c2pa::kKeychainEntry, Secret("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n")));
    auto third = c2pa::localIdentity(keys, dir);
    REQUIRE(third);
    CHECK(third->certificateChainPem != first->certificateChainPem);
    REQUIRE(c2pa::resetLocalIdentity(keys, dir));
    CHECK_FALSE(std::filesystem::exists(dir / "signing-certificate.pem"));
    CHECK_FALSE(*keys.read(c2pa::kKeychainEntry));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("Windows Credential Manager keeps the signing key for the app and the CLI", "[ai][c2pa][keychain]") {
    auto keychain = c2pa::osKeychain();
#ifndef _WIN32
    CHECK_FALSE(keychain);
    SKIP("Windows Credential Manager only");
#else
    REQUIRE(keychain);
    CHECK(keychain->persistent());
    CHECK(keychain->backendName() == "Windows Credential Manager");
    const std::string entry = "test-" + std::to_string(std::random_device{}());
    CHECK_FALSE(*keychain->read(entry));
    REQUIRE(keychain->write(entry, Secret("-----BEGIN PRIVATE KEY-----\nTESTONLY\n-----END PRIVATE KEY-----\n")));
    auto back = keychain->read(entry);
    REQUIRE(back);
    REQUIRE(*back);
    CHECK((*back)->reveal().find("TESTONLY") != std::string::npos);
    REQUIRE(keychain->remove(entry));
    CHECK_FALSE(*keychain->read(entry));
    CHECK(keychain->remove(entry)); // removing a missing entry succeeds
    CHECK_FALSE(keychain->write(entry, Secret(std::string(5000, 'x')))); // over the 2560-byte limit
#endif
}

TEST_CASE("Content Credentials modes and AI-generated material", "[ai][c2pa]") {
    for (auto mode : {c2pa::CredentialsMode::Off, c2pa::CredentialsMode::Ai, c2pa::CredentialsMode::Always}) {
        CHECK(c2pa::credentialsModeFromString(c2pa::toString(mode)) == mode);
    }
    CHECK(c2pa::credentialsModeFromString("on") == c2pa::CredentialsMode::Always);
    CHECK_FALSE(c2pa::credentialsModeFromString("sometimes"));
    wabcut::test::Timeline tl("V1 tgt |AaaaaaaaaaBbbbbbbbbb|\nA1 tgt |AaaaaaaaaaBbbbbbbbbb|");
    CHECK_FALSE(c2pa::containsGenerated(tl.state, tl.seq, std::nullopt));
    auto media = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media('B')));
    media->generated = GeneratedProvenance{"runway", "gen4.5", std::string(64, 'a'), "2026-10-11T12:00:00Z", "video", "extend-tail", {}};
    for (MediaPtr& m : tl.state.media) {
        if (m->id == media->id) {
            m = media;
        }
    }
    CHECK(c2pa::containsGenerated(tl.state, tl.seq, std::nullopt));
    CHECK_FALSE(c2pa::containsGenerated(tl.state, tl.seq, TimeRange::fromStartEnd(wabcut::test::f(0), wabcut::test::f(10))));
    CHECK(c2pa::formatForPath("a/b.MP4") == "video/mp4");
    CHECK(c2pa::formatForPath("b.mov") == "video/quicktime");
    CHECK(c2pa::formatForPath("b.mkv").empty());
}

TEST_CASE("The manifest of an export has actions and hashed ingredients and AI segments and redactions but no prompts or names", "[ai][c2pa]") {
    using wabcut::test::f;
    wabcut::test::Timeline tl("V1 tgt |AaaaaaaaaaBbbbbbbbbb..........|\nA1 tgt |AaaaaaaaaaBbbbbbbbbb..........|");
    const auto dir = tempDir("wabcut-c2pa-manifest");
    // Media A and B are real files (so they are hashed); B is AI-generated; a redaction blurs A's first 4 frames.
    std::map<char, std::string> sha;
    const MediaId mediaA = tl.media('A'); // before the loop renames the media
    for (char letter : {'A', 'B'}) {
        const auto file = dir / (std::string(1, letter) + ".mp4");
        const std::string bytes(4096 + letter, letter);
        writeAll(file, bytes);
        sha[letter] = Sha256::hex(bytes);
        auto media = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media(letter)));
        media->locator.path = pathToUtf8(file);
        media->name = std::string("Clip ") + letter;
        if (letter == 'B') {
            media->generated = GeneratedProvenance{"runway", "gen4.5", Sha256::hex("a private prompt about Jane"), "2026-10-11T12:00:00Z",
                                                   "video", "extend-tail", {}};
        }
        for (MediaPtr& m : tl.state.media) {
            if (m->id == media->id) {
                m = media;
            }
        }
    }
    Redaction r;
    r.id = tl.state.ids.allocate<RedactionId>();
    r.media = mediaA;
    r.name = "John Doe's face";
    r.source = TimeRange::fromStartEnd(f(0), f(4));
    r.style = RedactionStyle::Blur;
    r.audio = RedactionAudio::Bleep;
    tl.state.redactions.push_back(r);

    c2pa::ManifestOptions options;
    options.title = "cut.mp4";
    options.softwareVersion = "0.1.0";
    const Json m = c2pa::manifestDefinition(tl.state, tl.seq, options);
    INFO(m.dump(2));
    CHECK(m["claim_generator_info"][0]["name"] == "Wabcut");
    CHECK(m["claim_generator_info"][0]["version"] == "0.1.0");
    CHECK(m["format"] == "video/mp4");
    const Json& actions = m["assertions"][0]["data"]["actions"];
    REQUIRE(m["assertions"][0]["label"] == "c2pa.actions.v2");
    REQUIRE(actions.size() >= 4);
    CHECK(actions[0]["action"] == "c2pa.created");
    CHECK(actions[0]["digitalSourceType"] == "http://cv.iptc.org/newscodes/digitalsourcetype/compositeWithTrainedAlgorithmicMedia");
    CHECK(actions[0]["softwareAgent"]["version"] == "0.1.0");
    CHECK(actions[1]["action"] == "c2pa.edited");
    // The AI segment: where in the output (B plays 0.4-0.8 s) and by what, with the prompt's hash only.
    const auto ai = std::ranges::find_if(actions, [](const Json& a) {
        return a.value("digitalSourceType", std::string()).ends_with("/trainedAlgorithmicMedia");
    });
    REQUIRE(ai != actions.end());
    CHECK((*ai)["changes"][0]["region"][0]["time"]["start"] == "0.400");
    CHECK((*ai)["changes"][0]["region"][0]["time"]["end"] == "0.800");
    CHECK((*ai)["parameters"]["org.wabcut.promptSha256"] == Sha256::hex("a private prompt about Jane"));
    // The redaction: where and how, never its name.
    const auto red = std::ranges::find_if(actions, [](const Json& a) { return a["action"] == "org.wabcut.redacted"; });
    REQUIRE(red != actions.end());
    CHECK((*red)["parameters"]["style"] == "blur");
    CHECK((*red)["parameters"]["audio"] == "bleep");
    CHECK((*red)["changes"][0]["region"][0]["time"]["start"] == "0.000");
    CHECK((*red)["changes"][0]["region"][0]["time"]["end"] == "0.160");
    // Ingredients: every source with its SHA-256; the generated one as trainedAlgorithmicMedia.
    REQUIRE(m["ingredients"].size() == 2);
    for (const Json& ing : m["ingredients"]) {
        CHECK(ing["relationship"] == "componentOf");
        CHECK(ing["format"] == "video/mp4");
        CHECK_FALSE(ing.contains("thumbnail"));
        const char letter = ing["title"].get<std::string>().back();
        CHECK(ing["hash"] == "sha256:" + sha[letter]);
        CHECK(ing.contains("digital_source_type") == (letter == 'B'));
    }
    const std::string text = m.dump();
    CHECK(text.find("John Doe") == std::string::npos);
    CHECK(text.find("private prompt") == std::string::npos);
    CHECK(text.find("thumbnail") == std::string::npos);
    // An exported range without B: no AI segment, a plain composite.
    options.range = TimeRange::fromStartEnd(f(0), f(10));
    const Json firstHalf = c2pa::manifestDefinition(tl.state, tl.seq, options);
    CHECK(firstHalf["ingredients"].size() == 1);
    CHECK(firstHalf["assertions"][0]["data"]["actions"][0]["digitalSourceType"] == "http://cv.iptc.org/newscodes/digitalsourcetype/composite");
    // Neither a producer nor a training request unless asked for.
    CHECK(text.find("CreativeWork") == std::string::npos);
    CHECK(text.find("training-mining") == std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("The manifest names the producer and asks AI not to train when the settings say so (0.2)", "[ai][c2pa]") {
    wabcut::test::Timeline tl("V1 tgt |Aaaaaaaaaa|");
    c2pa::ManifestOptions options;
    options.hashSources = false;
    options.producer = "  Studio Phosphor  ";
    options.doNotTrain = true;
    const Json m = c2pa::manifestDefinition(tl.state, tl.seq, options);
    INFO(m.dump(2));
    const auto find = [&](std::string_view label) {
        return std::ranges::find_if(m["assertions"], [&](const Json& a) { return a.value("label", std::string()) == label; });
    };
    const auto work = find("stds.schema-org.CreativeWork");
    REQUIRE(work != m["assertions"].end());
    CHECK((*work)["data"]["@type"] == "CreativeWork");
    CHECK((*work)["data"]["author"][0]["@type"] == "Person");
    CHECK((*work)["data"]["author"][0]["name"] == "  Studio Phosphor  "); // as the host gives it (the app trims)
    const auto training = find("cawg.training-mining");
    REQUIRE(training != m["assertions"].end());
    for (const char* use : {"cawg.ai_inference", "cawg.ai_generative_training", "cawg.ai_training", "cawg.data_mining"}) {
        INFO(use);
        CHECK((*training)["data"]["entries"][use]["use"] == "notAllowed");
    }
    // A very long name is cut, not refused.
    options.producer = std::string(1000, 'x');
    const Json longName = c2pa::manifestDefinition(tl.state, tl.seq, options);
    const auto cut = std::ranges::find_if(longName["assertions"], [](const Json& a) { return a["label"] == "stds.schema-org.CreativeWork"; });
    REQUIRE(cut != longName["assertions"].end());
    CHECK((*cut)["data"]["author"][0]["name"].get<std::string>().size() == 256);
}

TEST_CASE("Signing and verification and trust and tamper detection with the same c2pa library", "[ai][c2pa][sign]") {
    if (!c2pa::available()) {
        SKIP("the c2pa library is not in this build");
    }
    CHECK(c2pa::sdkVersion().find("0.91") != std::string::npos);
    auto identity = c2pa::generateIdentity();
    REQUIRE(identity);
    const auto dir = tempDir("wabcut-c2pa-sign");
    const auto still = writePng(dir);
    const std::string before = readAll(still);

    auto unsignedRead = c2pa::verifyFile(still, identity->rootPem, "image/png");
    REQUIRE(unsignedRead);
    CHECK_FALSE(unsignedRead->hasManifest);

    wabcut::test::Timeline tl("V1 tgt |Aaaaaaaaaa|");
    c2pa::ManifestOptions options;
    options.title = "still.png";
    options.format = "image/png";
    options.softwareVersion = "0.1.0";
    const Json definition = c2pa::manifestDefinition(tl.state, tl.seq, options);
    auto signedOk = c2pa::signFile(still, definition, *identity, {}, "image/png");
    INFO((signedOk ? std::string() : signedOk.error().message));
    REQUIRE(signedOk);
    CHECK(signedOk->manifestBytes > 0);
    CHECK_FALSE(std::filesystem::exists(still.string() + ".c2pa-signing"));
    CHECK(readAll(still) != before);

    // Valid and trusted with the identity's root as the anchor.
    auto trusted = c2pa::verifyFile(still, identity->rootPem, "image/png");
    REQUIRE(trusted);
    INFO("codes: " << Json(trusted->codes).dump());
    CHECK(trusted->hasManifest);
    CHECK(trusted->valid);
    CHECK(trusted->trusted);
    CHECK(trusted->failures.empty());
    const Json& manifest = trusted->manifest;
    CHECK(manifest.dump().find("c2pa.actions") != std::string::npos);
    CHECK(manifest.dump().find("org.wabcut.sources") != std::string::npos);
    CHECK(manifest.dump().find("Wabcut") != std::string::npos);

    // Without the anchor: intact but the signer is unknown (what anyone else sees for a local identity).
    auto stranger = c2pa::verifyFile(still, {}, "image/png");
    REQUIRE(stranger);
    CHECK(stranger->valid);
    CHECK_FALSE(stranger->trusted);
    CHECK(has(stranger->failures, "signingCredential.untrusted"));

    // Tamper: one changed pixel byte fails the asset hash.
    std::string bytes = readAll(still);
    const std::size_t idat = bytes.find("IDAT");
    REQUIRE(idat != std::string::npos);
    bytes[idat + 8] = static_cast<char>(bytes[idat + 8] ^ 0x5A);
    const auto tampered = dir / "tampered.png";
    writeAll(tampered, bytes);
    auto broken = c2pa::verifyFile(tampered, identity->rootPem, "image/png");
    REQUIRE(broken);
    INFO("tampered codes: " << Json(broken->failures).dump());
    CHECK(broken->hasManifest);
    CHECK_FALSE(broken->valid);
    CHECK(std::ranges::any_of(broken->failures, [](const std::string& c) { return c.find("mismatch") != std::string::npos; }));

    // A failed signing leaves the file untouched, removes its temporary copy and never reveals the key.
    const auto other = writePng(dir, "other.png");
    const std::string otherBefore = readAll(other);
    auto wrongKey = c2pa::generateIdentity();
    REQUIRE(wrongKey);
    c2pa::SigningIdentity mismatched;
    mismatched.certificateChainPem = identity->certificateChainPem;
    mismatched.privateKeyPem = wrongKey->privateKeyPem.clone();
    mismatched.rootPem = identity->rootPem;
    auto refused = c2pa::signFile(other, definition, mismatched, {}, "image/png");
    REQUIRE_FALSE(refused);
    CHECK(readAll(other) == otherBefore);
    CHECK_FALSE(std::filesystem::exists(other.string() + ".c2pa-signing"));
    CHECK(refused.error().message.find("PRIVATE KEY") == std::string::npos);
    CHECK(refused.error().message.find(pemBody(wrongKey->privateKeyPem.reveal()).substr(0, 24)) == std::string::npos);
    // No file anywhere in the work directory holds a private key.
    CHECK(filesContaining(dir, "PRIVATE KEY").empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("The export finalizer signs or warns and leaves the file as it was", "[ai][c2pa][sign]") {
    if (!c2pa::available()) {
        SKIP("the c2pa library is not in this build");
    }
    const auto dir = tempDir("wabcut-c2pa-finalize");
    wabcut::test::Timeline tl("V1 tgt |Aaaaaaaaaa|");
    auto snapshot = std::make_shared<const ProjectState>(tl.state);
    c2pa::ManifestOptions options;
    options.format = "image/png";
    options.softwareVersion = "0.1.0";
    auto identity = c2pa::generateIdentity();
    REQUIRE(identity);
    const std::string root = identity->rootPem;
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto shared = std::make_shared<c2pa::SigningIdentity>(std::move(*identity));
    auto finalize = c2pa::exportFinalizer(snapshot, tl.seq, options, [shared]() -> std::expected<c2pa::SigningIdentity, AiError> {
        c2pa::SigningIdentity copy;
        copy.certificateChainPem = shared->certificateChainPem;
        copy.privateKeyPem = shared->privateKeyPem.clone();
        copy.rootPem = shared->rootPem;
        copy.algorithm = shared->algorithm;
        return copy;
    }, flag);
    const auto file = writePng(dir, "export.wabcut-partial-0001"); // an export's temporary name has no extension
    CHECK(finalize(file, {}).empty());
    CHECK(flag->load());
    auto v = c2pa::verifyFile(file, root, "image/png");
    REQUIRE(v);
    CHECK(v->valid);

    const auto second = writePng(dir, "second.wabcut-partial-0002");
    const std::string before = readAll(second);
    auto failing = c2pa::exportFinalizer(snapshot, tl.seq, options, []() -> std::expected<c2pa::SigningIdentity, AiError> {
        return std::unexpected(AiError(AiErrorCode::Unauthorized, "the keychain refused"));
    });
    const std::string warning = failing(second, {});
    CHECK(warning == "Content Credentials not signed: the keychain refused");
    CHECK(readAll(second) == before);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
