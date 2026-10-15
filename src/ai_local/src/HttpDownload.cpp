#include <wabcut/ai_local/HttpDownload.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>
#endif

namespace wabcut::ai_local {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
};

std::unexpected<Error> netError(const char* what) {
    return std::unexpected(Error{ErrorCode::IoError, std::string(what) + " (WinHTTP error " + std::to_string(GetLastError()) + ")"});
}

class WinHttpTransport final : public ai::IDownloadTransport {
public:
    std::expected<Response, Error> get(const std::string& url, std::uint64_t offset,
                                       const std::function<bool(const Response&)>& onResponse,
                                       const std::function<bool(std::span<const std::byte>)>& onData,
                                       std::stop_token stop) override {
        const std::wstring wurl = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256];
        wchar_t path[2048];
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "not an HTTPS URL: " + url});
        }
        Handle session{WinHttpOpen(L"Wabcut", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0)};
        if (session.h == nullptr) {
            return netError("cannot open an HTTP session");
        }
        Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
        if (connection.h == nullptr) {
            return netError("cannot connect");
        }
        Handle request{WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (request.h == nullptr) {
            return netError("cannot create the request");
        }
        DWORD noCookies = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof(noCookies));
        std::wstring headers;
        if (offset > 0) {
            headers = L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
        }
        if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr)) {
            return netError("the download request failed");
        }
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        Response response;
        response.status = static_cast<int>(status);
        wchar_t lengthText[32] = {};
        DWORD lengthSize = sizeof(lengthText);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
                                &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
            const std::uint64_t length = std::wcstoull(lengthText, nullptr, 10);
            response.totalSize = status == 206 ? length + offset : length;
        }
        if (onResponse && !onResponse(response)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "download stopped (HTTP " + std::to_string(status) + ")"});
        }
        std::vector<std::byte> buffer(1 << 16);
        for (;;) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::Cancelled, "download cancelled"});
            }
            DWORD got = 0;
            if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
                return netError("the download was interrupted");
            }
            if (got == 0) {
                break;
            }
            if (!onData(std::span<const std::byte>(buffer.data(), got))) {
                return std::unexpected(Error{ErrorCode::Cancelled, "download stopped"});
            }
        }
        return response;
    }
};

} // namespace

std::unique_ptr<ai::IDownloadTransport> makeHttpDownloadTransport() { return std::make_unique<WinHttpTransport>(); }

#else

// TODO(ai): a libcurl or Qt (src/ai_net) transport for macOS and Linux.
std::unique_ptr<ai::IDownloadTransport> makeHttpDownloadTransport() { return nullptr; }

#endif

} // namespace wabcut::ai_local
