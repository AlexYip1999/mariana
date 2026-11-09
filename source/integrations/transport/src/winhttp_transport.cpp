#include "agent/transport/winhttp_transport.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

bool ReadContentLength(HINTERNET request, std::size_t limit, std::optional<std::size_t>& expected,
                       HttpResponse& response)
{
    wchar_t text[64]{};
    DWORD size = sizeof(text);
    // Transfer-Encoding takes precedence over Content-Length.
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_TRANSFER_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX, text, &size,
                            WINHTTP_NO_HEADER_INDEX))
    {
        return true;
    }
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        return true;
    }
    size = sizeof(text);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &size,
                             WINHTTP_NO_HEADER_INDEX))
    {
        if (GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND)
        {
            return true;
        }
        response.error = "invalid HTTP Content-Length";
        return false;
    }
    const std::wstring_view value(text, size / sizeof(wchar_t));
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring_view::npos)
    {
        response.error = "invalid HTTP Content-Length";
        return false;
    }
    std::size_t length = 0;
    for (const wchar_t character : value)
    {
        const auto digit = static_cast<std::size_t>(character - L'0');
        if (length > limit / 10 || (length == limit / 10 && digit > limit % 10))
        {
            response.error = "HTTP response exceeds size limit";
            return false;
        }
        length = length * 10 + digit;
    }
    expected = length;
    return true;
}

struct AsyncState
{
    std::mutex mutex;
    std::condition_variable_any changed;
    bool completed = false;
    DWORD error = ERROR_SUCCESS;
    DWORD bytes_read = 0;
    std::array<char, 8192> buffer{};
    std::string request_body;
    std::wstring headers;

    void Reset()
    {
        std::lock_guard lock(mutex);
        completed = false;
        error = ERROR_SUCCESS;
        bytes_read = 0;
    }

    bool Wait(std::chrono::steady_clock::time_point deadline, std::stop_token stop_token, HttpResponse& response)
    {
        std::unique_lock lock(mutex);
        changed.wait_until(lock, stop_token, deadline,
                           [&]()
                           {
                               return completed;
                           });
        if (stop_token.stop_requested())
        {
            response.error = "HTTP request cancelled";
        }
        else if (!completed || std::chrono::steady_clock::now() >= deadline)
        {
            response.error = "HTTP request deadline exceeded";
        }
        else if (error != ERROR_SUCCESS)
        {
            response.error = "WinHTTP request failed: " + FormatWindowsError(error);
        }
        return response.error.empty();
    }
};

using AsyncContext = std::shared_ptr<AsyncState>;

void CALLBACK OnHttpStatus(HINTERNET, DWORD_PTR context_value, DWORD status, void* information, DWORD length)
{
    if (context_value == 0)
    {
        return;
    }
    auto* context = reinterpret_cast<AsyncContext*>(context_value);
    const auto state = *context;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
        // WinHTTP may call back after CloseHandle returns. Keep buffers alive until this final notification.
        delete context;
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        switch (status)
        {
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            {
                state->error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
                break;
            }
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            {
                state->bytes_read = length;
                break;
            }
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            {
                break;
            }
            default:
            {
                return;
            }
        }
        state->completed = true;
    }
    state->changed.notify_all();
}

} // namespace

HttpResponse WinHttpTransport::PostJson(const HttpRequest& request)
{
    HttpResponse response;
    if (request.timeout_ms <= 0 || request.max_response_bytes == 0)
    {
        response.error = "HTTP timeout and response size limit must be positive";
        return response;
    }
    if (request.stop_token.stop_requested())
    {
        response.error = "HTTP request cancelled";
        return response;
    }
    if (request.body.size() > (std::numeric_limits<DWORD>::max)())
    {
        response.error = "HTTP request body too large";
        return response;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeout_ms);

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
                                      WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
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

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(http_request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                          sizeof(redirect_policy)))
    {
        response.error = "cannot disable HTTP redirects: " + LastWindowsError();
        return response;
    }

    const auto state = std::make_shared<AsyncState>();
    auto context = std::make_unique<AsyncContext>(state);
    DWORD_PTR context_value = reinterpret_cast<DWORD_PTR>(context.get());
    if (!WinHttpSetOption(http_request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context_value, sizeof(context_value)))
    {
        response.error = "cannot set HTTP context: " + LastWindowsError();
        return response;
    }
    const DWORD callback_flags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES;
    if (WinHttpSetStatusCallback(http_request.value, OnHttpStatus, callback_flags, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
    {
        response.error = "cannot set HTTP callback: " + LastWindowsError();
        return response;
    }
    context.release(); // Released by the final HANDLE_CLOSING callback, including on early returns.

    for (const auto& header : request.headers)
    {
        state->headers += Utf8ToWide(header.first);
        state->headers += L": ";
        state->headers += Utf8ToWide(header.second);
        state->headers += L"\r\n";
    }

    state->request_body = request.body;
    const DWORD body_size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(http_request.value,
                            state->headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : state->headers.c_str(),
                            state->headers.empty() ? 0 : static_cast<DWORD>(-1),
                            body_size == 0 ? nullptr : state->request_body.data(), body_size, body_size, context_value))
    {
        response.error = "WinHttpSendRequest failed: " + LastWindowsError();
        return response;
    }
    if (!state->Wait(deadline, request.stop_token, response))
    {
        return response;
    }

    state->Reset();
    if (!WinHttpReceiveResponse(http_request.value, nullptr))
    {
        response.error = "WinHttpReceiveResponse failed: " + LastWindowsError();
        return response;
    }
    if (!state->Wait(deadline, request.stop_token, response))
    {
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
    std::optional<std::size_t> expected_length;
    const bool has_body = response.status != 204 && response.status != 304;
    if (has_body && !ReadContentLength(http_request.value, request.max_response_bytes, expected_length, response))
    {
        return response;
    }

    std::string body;
    for (;;)
    {
        state->Reset();
        if (!WinHttpReadData(http_request.value, state->buffer.data(), static_cast<DWORD>(state->buffer.size()),
                             nullptr))
        {
            response.error = "WinHttpReadData failed: " + LastWindowsError();
            return response;
        }
        if (!state->Wait(deadline, request.stop_token, response))
        {
            return response;
        }
        const DWORD read = state->bytes_read;
        if (read == 0)
        {
            break;
        }

        if (read > request.max_response_bytes - body.size())
        {
            response.error = "HTTP response exceeds size limit";
            return response;
        }
        body.append(state->buffer.data(), read);
    }

    if (expected_length && body.size() != *expected_length)
    {
        response.error = "HTTP response body incomplete";
        return response;
    }
    response.body = std::move(body);
    return response;
}

} // namespace agent::transport
