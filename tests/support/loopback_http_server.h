#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace agent::tests
{

class WinsockSession
{
  public:
    WinsockSession()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~WinsockSession()
    {
        WSACleanup();
    }
};

class TestSocket
{
  public:
    explicit TestSocket(SOCKET socket)
        : value(socket)
    {}

    ~TestSocket()
    {
        if (value != INVALID_SOCKET)
        {
            closesocket(value);
        }
    }

    TestSocket(const TestSocket&) = delete;
    TestSocket& operator=(const TestSocket&) = delete;

    SOCKET value;
};

/// One local HTTP listener. All waits are bounded and destruction stops its worker.
class LoopbackHttpServer
{
  public:
    using Handler = std::function<void(SOCKET, std::stop_token)>;

    explicit LoopbackHttpServer(Handler handler)
        : m_listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
        , m_handler(std::move(handler))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (m_listener.value == INVALID_SOCKET)
        {
            throw std::runtime_error("cannot create loopback test socket");
        }
        if (bind(m_listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            throw std::runtime_error("cannot bind loopback test server");
        }
        int size = sizeof(address);
        if (getsockname(m_listener.value, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        {
            throw std::runtime_error("cannot get loopback test server port");
        }
        if (listen(m_listener.value, 4) != 0)
        {
            throw std::runtime_error("cannot listen on loopback test server");
        }

        m_port = ntohs(address.sin_port);
        m_thread = std::jthread(
            [this](std::stop_token token)
            {
                Serve(token);
            });
    }

    ~LoopbackHttpServer()
    {
        m_thread.request_stop();
        m_thread.join();
    }

    std::string Url(const std::string& host = "127.0.0.1") const
    {
        return "http://" + host + ":" + std::to_string(m_port) + "/chat";
    }

    std::vector<std::string> Requests() const
    {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }

    static bool Send(SOCKET client, std::string_view data)
    {
        while (!data.empty())
        {
            const int count = send(client, data.data(), static_cast<int>(data.size()), 0);
            if (count <= 0)
            {
                return false;
            }
            data.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }

  private:
    static std::string ReadRequest(SOCKET client)
    {
        std::string request;
        char buffer[4096];
        while (request.size() < 65536)
        {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0)
            {
                break;
            }
            request.append(buffer, count);

            const auto header_end = request.find("\r\n\r\n");
            if (header_end == std::string::npos)
            {
                continue;
            }
            const auto length_header = request.find("Content-Length:");
            const std::size_t body_size =
                length_header == std::string::npos ? 0 : std::stoull(request.substr(length_header + 15));
            if (request.size() >= header_end + 4 + body_size)
            {
                break;
            }
        }
        return request;
    }

    void Serve(std::stop_token token)
    {
        while (!token.stop_requested())
        {
            fd_set readers;
            FD_ZERO(&readers);
            FD_SET(m_listener.value, &readers);
            timeval timeout{0, 20000};

            if (select(0, &readers, nullptr, nullptr, &timeout) <= 0)
            {
                continue;
            }
            TestSocket client(accept(m_listener.value, nullptr, nullptr));
            if (client.value == INVALID_SOCKET)
            {
                continue;
            }

            const DWORD timeout_ms = 2000;
            setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                       sizeof(timeout_ms));
            setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                       sizeof(timeout_ms));

            const std::string request = ReadRequest(client.value);
            {
                std::lock_guard lock(m_mutex);
                m_requests.push_back(request);
            }

            m_handler(client.value, token);
        }
    }

    // Winsock session and listener resources
    WinsockSession m_winsock;
    TestSocket m_listener;
    Handler m_handler;
    unsigned short m_port = 0;

    // Requests received by the worker thread
    mutable std::mutex m_mutex;
    std::vector<std::string> m_requests;

    // Background accept loop
    std::jthread m_thread;
};

} // namespace agent::tests
