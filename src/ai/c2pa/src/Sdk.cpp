// Signing and verification with c2pa-rs's C library (AI-15). Its header declares unscoped C enums (Start, End, Create,
// Empty, ...), so it is included here only. Every context refuses network access twice over: the settings allow no
// network host and an HTTP resolver that refuses every request replaces the SDK's own (no timestamp authority, no OCSP,
// no remote manifests).
#include <wabcut/ai/c2pa/ContentCredentials.h>

#include "Der.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(WABCUT_HAVE_C2PA) && WABCUT_HAVE_C2PA
#include <c2pa.h>
// c2pa.h (cbindgen) puts its deprecation attributes after `extern`, where MSVC's [[deprecated]] attaches them to the
// struct type the function returns: every use of C2paBuilder, C2paReader and C2paSigner then warns C4996 with some
// other function's message, although this file calls only the context API that replaces the deprecated functions.
#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif
#endif

namespace wabcut::ai::c2pa {

namespace {

AiError failure(AiErrorCode code, std::string message) { return AiError(code, std::move(message)); }

/// The extended key usages a signing certificate may carry (c2pa-rs 0.91 clears its built-in list when a context
/// sets trust settings, so it is given explicitly): emailProtection, documentSigning, timeStamping, OCSPSigning,
/// Microsoft's C2PA signing, C2PA claim signing.
constexpr const char* kEkus =
    "1.3.6.1.5.5.7.3.4\n1.3.6.1.5.5.7.3.36\n1.3.6.1.5.5.7.3.8\n1.3.6.1.5.5.7.3.9\n1.3.6.1.4.1.311.76.59.1.9\n1.3.6.1.4.1.62558.2.1";

std::string settingsJson(bool signing, const std::string& anchors) {
    Json s{{"version", 1},
           {"verify",
            {{"verify_after_sign", true},
             {"verify_trust", !signing},
             {"verify_timestamp_trust", false},
             {"remote_manifest_fetch", false},
             {"ocsp_fetch", false}}},
           {"core", {{"allowed_network_hosts", Json::array()}}},
           {"builder", {{"thumbnail", {{"enabled", false}}}}},
           {"trust", {{"trust_config", kEkus}}}};
    if (!anchors.empty()) {
        s["trust"]["anchors"] = Json::array({Json{{"trust_anchors", anchors}, {"trust_kind", "manifest"}, {"trust_config", kEkus}}});
    }
    return s.dump();
}

} // namespace

std::string formatForPath(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == ".mp4" || ext == ".m4v") {
        return "video/mp4";
    }
    if (ext == ".mov") {
        return "video/quicktime";
    }
    return {};
}

#if defined(WABCUT_HAVE_C2PA) && WABCUT_HAVE_C2PA

namespace {

/// c2pa_c.dll is delay-loaded: load it explicitly (from the executable's directory or System32 only) so a missing
/// DLL means "unavailable" instead of a delay-load exception.
bool loadLibrary() {
#ifdef _WIN32
    static const bool loaded = LoadLibraryExW(L"c2pa_c.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
    return loaded;
#else
    return true;
#endif
}

struct Free {
    void operator()(const void* p) const {
        if (p) {
            c2pa_free(p);
        }
    }
};
template <class T> using Owned = std::unique_ptr<T, Free>;

std::string lastError() {
    char* text = c2pa_error();
    std::string out = text ? text : "unknown error";
    if (text) {
        c2pa_free(text);
    }
    return out;
}

/// A FILE*-backed C2paStream.
class FileStream {
public:
    FileStream(const std::filesystem::path& path, const wchar_t* mode) {
#ifdef _WIN32
        _wfopen_s(&m_file, path.c_str(), mode);
#else
        m_file = std::fopen(path.c_str(), mode[0] == L'r' ? "rb" : "w+b");
#endif
        if (m_file) {
            m_stream = c2pa_create_stream(reinterpret_cast<StreamContext*>(this), &read, &seek, &write, &flush);
        }
    }
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;
    ~FileStream() { close(); }
    C2paStream* get() const { return m_stream; }
    void close() {
        if (m_stream) {
            c2pa_release_stream(m_stream);
            m_stream = nullptr;
        }
        if (m_file) {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

private:
    static FileStream& self(StreamContext* c) { return *reinterpret_cast<FileStream*>(c); }
    static intptr_t read(StreamContext* c, uint8_t* data, intptr_t len) {
        const std::size_t n = std::fread(data, 1, static_cast<std::size_t>(len), self(c).m_file);
        return n == 0 && std::ferror(self(c).m_file) ? -1 : static_cast<intptr_t>(n);
    }
    static intptr_t seek(StreamContext* c, intptr_t offset, C2paSeekMode mode) {
        const int whence = mode == C2paSeekMode::Start ? SEEK_SET : mode == C2paSeekMode::Current ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
        if (_fseeki64(self(c).m_file, offset, whence) != 0) {
            return -1;
        }
        return static_cast<intptr_t>(_ftelli64(self(c).m_file));
#else
        if (fseeko(self(c).m_file, offset, whence) != 0) {
            return -1;
        }
        return static_cast<intptr_t>(ftello(self(c).m_file));
#endif
    }
    static intptr_t write(StreamContext* c, const uint8_t* data, intptr_t len) {
        const std::size_t n = std::fwrite(data, 1, static_cast<std::size_t>(len), self(c).m_file);
        return n == static_cast<std::size_t>(len) ? static_cast<intptr_t>(n) : -1;
    }
    static intptr_t flush(StreamContext* c) { return std::fflush(self(c).m_file) == 0 ? 0 : -1; }

    std::FILE* m_file = nullptr;
    C2paStream* m_stream = nullptr;
};

/// A read-only C2paStream over bytes in memory (the manifest's thumbnail).
class MemoryStream {
public:
    explicit MemoryStream(const std::string& bytes) : m_bytes(bytes) {
        m_stream = c2pa_create_stream(reinterpret_cast<StreamContext*>(this), &read, &seek, &write, &flush);
    }
    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;
    ~MemoryStream() {
        if (m_stream) {
            c2pa_release_stream(m_stream);
        }
    }
    C2paStream* get() const { return m_stream; }

private:
    static MemoryStream& self(StreamContext* c) { return *reinterpret_cast<MemoryStream*>(c); }
    static intptr_t read(StreamContext* c, uint8_t* data, intptr_t len) {
        MemoryStream& m = self(c);
        const std::size_t n = std::min(static_cast<std::size_t>(std::max<intptr_t>(len, 0)), m.m_bytes.size() - m.m_position);
        std::memcpy(data, m.m_bytes.data() + m.m_position, n);
        m.m_position += n;
        return static_cast<intptr_t>(n);
    }
    static intptr_t seek(StreamContext* c, intptr_t offset, C2paSeekMode mode) {
        MemoryStream& m = self(c);
        const intptr_t base = mode == C2paSeekMode::Start ? 0
                              : mode == C2paSeekMode::Current ? static_cast<intptr_t>(m.m_position)
                                                              : static_cast<intptr_t>(m.m_bytes.size());
        const intptr_t to = base + offset;
        if (to < 0 || to > static_cast<intptr_t>(m.m_bytes.size())) {
            return -1;
        }
        m.m_position = static_cast<std::size_t>(to);
        return to;
    }
    static intptr_t write(StreamContext*, const uint8_t*, intptr_t) { return -1; }
    static intptr_t flush(StreamContext*) { return 0; }

    const std::string& m_bytes;
    std::size_t m_position = 0;
    C2paStream* m_stream = nullptr;
};

int refuseNetwork(void*, const C2paHttpRequest*, C2paHttpResponse*) {
    c2pa_error_set_last("network access is disabled for Content Credentials in Wabcut");
    return 1;
}

std::expected<Owned<C2paContext>, AiError> makeContext(const std::string& settings) {
    C2paContextBuilder* builder = c2pa_context_builder_new();
    if (!builder) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa: " + lastError()));
    }
    Owned<C2paSettings> s(c2pa_settings_new());
    if (!s || c2pa_settings_update_from_string(s.get(), settings.c_str(), "json") != 0 ||
        c2pa_context_builder_set_settings(builder, s.get()) != 0) {
        const std::string error = lastError();
        c2pa_free(builder);
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa settings: " + error));
    }
    C2paHttpResolver* resolver = c2pa_http_resolver_create(nullptr, &refuseNetwork);
    if (!resolver || c2pa_context_builder_set_http_resolver(builder, resolver) != 0) {
        const std::string error = lastError();
        c2pa_free(builder);
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa network guard: " + error));
    }
    Owned<C2paContext> context(c2pa_context_builder_build(builder));
    if (!context) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa context: " + lastError()));
    }
    return context;
}

std::vector<std::string> codes(const Json& list) {
    std::vector<std::string> out;
    if (list.is_array()) {
        for (const Json& item : list) {
            if (item.is_object() && item.contains("code") && item["code"].is_string()) {
                out.push_back(item["code"].get<std::string>());
            }
        }
    }
    return out;
}

} // namespace

std::string sdkVersion() {
    if (!loadLibrary()) {
        return {};
    }
    char* v = c2pa_version();
    std::string out = v ? v : WABCUT_C2PA_VERSION;
    if (v) {
        c2pa_free(v);
    }
    return out;
}

bool available() { return loadLibrary(); }

std::expected<SignResult, AiError> signFile(const std::filesystem::path& file, const Json& definition, const SigningIdentity& identity,
                                            std::stop_token stop, std::string format, const std::string& thumbnailJpeg) {
    if (!loadLibrary()) {
        return std::unexpected(failure(AiErrorCode::Unsupported, "c2pa_c.dll is missing next to the program"));
    }
    if (format.empty()) {
        format = formatForPath(file);
    }
    if (format.empty()) {
        return std::unexpected(failure(AiErrorCode::Unsupported, "Content Credentials are signed into MP4 and MOV files only"));
    }
    auto context = makeContext(settingsJson(true, identity.rootPem));
    if (!context) {
        return std::unexpected(context.error());
    }
    std::stop_callback cancel(stop, [c = context->get()] { c2pa_context_cancel(c); });
    Json signedDefinition = definition;
    constexpr const char* kThumbnail = "thumbnail.jpg";
    if (!thumbnailJpeg.empty()) {
        signedDefinition["thumbnail"] = Json{{"format", "image/jpeg"}, {"identifier", kThumbnail}};
    }
    Owned<C2paBuilder> builder(c2pa_builder_from_context(context->get()));
    if (builder) {
        builder.reset(c2pa_builder_with_definition(builder.release(), signedDefinition.dump().c_str()));
    }
    if (!builder) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "c2pa manifest: " + lastError()));
    }
    if (!thumbnailJpeg.empty()) {
        MemoryStream thumbnail(thumbnailJpeg);
        if (!thumbnail.get() || c2pa_builder_add_resource(builder.get(), kThumbnail, thumbnail.get()) != 0) {
            return std::unexpected(failure(AiErrorCode::InvalidRequest, "c2pa thumbnail: " + lastError()));
        }
    }
    Owned<C2paSigner> signer;
    {
        std::string key(identity.privateKeyPem.reveal());
        const C2paSignerInfo info{identity.algorithm.c_str(), identity.certificateChainPem.c_str(), key.c_str(), nullptr};
        signer.reset(c2pa_signer_from_info(&info));
        der::wipe(key);
    }
    if (!signer) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "c2pa signer: " + lastError()));
    }
    auto signedCopy = file;
    signedCopy += ".c2pa-signing";
    std::error_code ec;
    std::int64_t size = -1;
    std::string error;
    {
        FileStream source(file, L"rb");
        FileStream dest(signedCopy, L"w+b");
        if (!source.get() || !dest.get()) {
            dest.close();
            std::filesystem::remove(signedCopy, ec);
            return std::unexpected(failure(AiErrorCode::ProviderError, "could not open the export for signing"));
        }
        const unsigned char* manifest = nullptr;
        size = c2pa_builder_sign(builder.get(), format.c_str(), source.get(), dest.get(), signer.get(), &manifest);
        if (size < 0) {
            error = lastError();
        }
        if (manifest) {
            c2pa_free(manifest);
        }
    }
    if (size < 0 || stop.stop_requested()) {
        std::filesystem::remove(signedCopy, ec);
        if (stop.stop_requested()) {
            return std::unexpected(failure(AiErrorCode::Cancelled, "signing cancelled"));
        }
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa could not sign the export: " + error));
    }
    // The signed copy must verify (signature, asset hash, certificate profile) before it replaces the export.
    auto check = verifyFile(signedCopy, identity.rootPem, format);
    if (!check || !check->hasManifest || !check->valid) {
        std::filesystem::remove(signedCopy, ec);
        std::string why = !check ? check.error().message : !check->hasManifest ? std::string("no manifest") : std::string();
        if (check) {
            for (const std::string& f : check->failures) {
                why += (why.empty() ? "" : ", ") + f;
            }
        }
        return std::unexpected(failure(AiErrorCode::ProviderError, "the signed export did not verify (" + why + ")"));
    }
    std::filesystem::rename(signedCopy, file, ec);
    if (ec) {
        std::filesystem::remove(signedCopy, ec);
        return std::unexpected(failure(AiErrorCode::ProviderError, "could not replace the export with its signed copy"));
    }
    SignResult result;
    result.manifestBytes = size;
    result.activeManifest = check->manifest.value("label", std::string());
    return result;
}

std::expected<Verification, AiError> verifyFile(const std::filesystem::path& file, const std::string& trustAnchorsPem, std::string format) {
    if (!loadLibrary()) {
        return std::unexpected(failure(AiErrorCode::Unsupported, "c2pa_c.dll is missing next to the program"));
    }
    if (format.empty()) {
        format = formatForPath(file);
    }
    if (format.empty()) {
        return std::unexpected(failure(AiErrorCode::Unsupported, "only MP4 and MOV files are read"));
    }
    auto context = makeContext(settingsJson(false, trustAnchorsPem));
    if (!context) {
        return std::unexpected(context.error());
    }
    FileStream source(file, L"rb");
    if (!source.get()) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "could not open the file"));
    }
    Owned<C2paReader> reader(c2pa_reader_from_context(context->get()));
    if (reader) {
        reader.reset(c2pa_reader_with_stream(reader.release(), format.c_str(), source.get()));
    }
    Verification v;
    if (!reader) {
        const std::string error = lastError();
        if (error.find("ManifestNotFound") != std::string::npos || error.find("JumbfNotFound") != std::string::npos ||
            error.find("no JUMBF") != std::string::npos || error.find("not found") != std::string::npos) {
            return v; // no Content Credentials in the file
        }
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa could not read the file: " + error));
    }
    char* text = c2pa_reader_json(reader.get());
    if (!text) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa reader: " + lastError()));
    }
    const Json j = Json::parse(text, nullptr, false);
    c2pa_free(text);
    if (j.is_discarded()) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "c2pa reader returned invalid JSON"));
    }
    v.hasManifest = j.contains("active_manifest");
    const std::string active = j.value("active_manifest", std::string());
    if (const auto manifests = j.find("manifests"); manifests != j.end() && manifests->contains(active)) {
        v.manifest = (*manifests)[active];
        v.manifest["label"] = active;
        const Json& info = v.manifest.value("signature_info", Json::object());
        v.signer = info.value("common_name", info.value("issuer", std::string()));
    }
    const Json results = j.value("validation_results", Json::object()).value("activeManifest", Json::object());
    for (const std::string& c : codes(results.value("success", Json::array()))) {
        v.codes.push_back(c);
    }
    for (const std::string& c : codes(results.value("informational", Json::array()))) {
        v.codes.push_back(c);
    }
    for (const std::string& c : codes(results.value("failure", Json::array()))) {
        v.codes.push_back(c);
        v.failures.push_back(c);
    }
    for (const std::string& c : codes(j.value("validation_status", Json::array()))) {
        if (std::ranges::find(v.codes, c) == v.codes.end()) {
            v.codes.push_back(c);
            v.failures.push_back(c);
        }
    }
    const std::string state = j.value("validation_state", std::string());
    v.trusted = state == "Trusted";
    // "Untrusted" is about who signed, not whether the file is intact.
    v.valid = v.hasManifest && std::ranges::all_of(v.failures, [](const std::string& c) { return c == "signingCredential.untrusted"; });
    return v;
}

#else

std::string sdkVersion() { return {}; }
bool available() { return false; }

std::expected<SignResult, AiError> signFile(const std::filesystem::path&, const Json&, const SigningIdentity&, std::stop_token, std::string,
                                            const std::string&) {
    return std::unexpected(failure(AiErrorCode::Unsupported, "this build signs no Content Credentials (WABCUT_WITH_C2PA=OFF)"));
}

std::expected<Verification, AiError> verifyFile(const std::filesystem::path&, const std::string&, std::string) {
    return std::unexpected(failure(AiErrorCode::Unsupported, "this build reads no Content Credentials (WABCUT_WITH_C2PA=OFF)"));
}

#endif

} // namespace wabcut::ai::c2pa
