#include "agent/transport/winhttp_transport.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <string_view>
#include <utility>

namespace agent::transport
{

namespace
{

struct WinHttpHandle
{
    HINTERNET value = nullptr;

    WinHttpHandle() = default;

    explicit WinHttpHandle(HINTERNET handle)
        : value(handle)
    {}

    ~WinHttpHandle()
    {
        if (value != nullptr)
        {
            WinHttpCloseHandle(value);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::string FormatWindowsError(DWORD error_code)
{
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error_code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr)
    {
        return "WinHTTP error " + std::to_string(error_code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
    {
        message.pop_back();
    }
    return WideToUtf8(message);
}

std::string LastWindowsError()
{
    return FormatWindowsError(GetLastError());
}

} // namespace

HttpResponse WinHttpTransport::PostJson(const HttpRequest& request)
{
    HttpResponse response;

    const std::wstring url = Utf8ToWide(request.url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components))
    {
        response.error = "invalid URL: " + LastWindowsError();
        return response;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo != nullptr)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    WinHttpHandle session(WinHttpOpen(L"agent-core/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.value == nullptr)
    {
        response.error = "WinHttpOpen failed: " + LastWindowsError();
        return response;
    }

    if (!WinHttpSetTimeouts(session.value, request.timeout_ms, request.timeout_ms, request.timeout_ms,
                            request.timeout_ms))
    {
        response.error = "WinHttpSetTimeouts failed: " + LastWindowsError();
        return response;
    }

    WinHttpHandle connection(
        WinHttpConnect(session.value, host.c_str(), static_cast<INTERNET_PORT>(components.nPort), 0));
    if (connection.value == nullptr)
    {
        response.error = "WinHttpConnect failed: " + LastWindowsError();
        return response;
    }

    const wchar_t* accept_types[] = {L"application/json", nullptr};
    WinHttpHandle http_request(WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                  accept_types, secure ? WINHTTP_FLAG_SECURE : 0));
    if (http_request.value == nullptr)
    {
        response.error = "WinHttpOpenRequest failed: " + LastWindowsError();
        return response;
    }

    std::wstring header_block;
    for (const auto& header : request.headers)
    {
        header_block += Utf8ToWide(header.first);
        header_block += L": ";
        header_block += Utf8ToWide(header.second);
        header_block += L"\r\n";
    }

    const DWORD body_size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(http_request.value,
                            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
                            header_block.empty() ? 0 : static_cast<DWORD>(-1),
                            body_size == 0 ? nullptr : const_cast<char*>(request.body.data()), body_size, body_size, 0))
    {
        response.error = "WinHttpSendRequest failed: " + LastWindowsError();
        return response;
    }

    if (!WinHttpReceiveResponse(http_request.value, nullptr))
    {
        response.error = "WinHttpReceiveResponse failed: " + LastWindowsError();
        return response;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(http_request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX))
    {
        response.error = "WinHttpQueryHeaders failed: " + LastWindowsError();
        return response;
    }
    response.status = static_cast<int>(status_code);

    std::string body;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http_request.value, &available))
        {
            response.error = "WinHttpQueryDataAvailable failed: " + LastWindowsError();
            return response;
        }
        if (available == 0)
        {
            break;
        }

        const size_t old_size = body.size();
        body.resize(old_size + available);

        DWORD read = 0;
        if (!WinHttpReadData(http_request.value, body.data() + old_size, available, &read))
        {
            response.error = "WinHttpReadData failed: " + LastWindowsError();
            return response;
        }

        body.resize(old_size + read);
        if (read == 0)
        {
            break;
        }
    }

    response.body = std::move(body);
    return response;
}

} // namespace agent::transport
