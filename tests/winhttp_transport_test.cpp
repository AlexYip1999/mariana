#include "support/loopback_http_server.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#if defined(_WIN32)

#include "agent/transport/winhttp_transport.h"

using agent::tests::LoopbackHttpServer;

TEST_CASE("winhttp posts JSON and reads complete responses")
{
    LoopbackHttpServer server(
        [](SOCKET client, std::stop_token)
        {
            LoopbackHttpServer::Send(client, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
        });

    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = server.Url();
    request.body = R"({"prompt":"hello"})";
    request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer test-only"}};
    request.max_response_bytes = 2;

    const auto response = transport.PostJson(request);

    INFO(response.error);
    CHECK(response.Ok());
    CHECK(response.body == "{}");

    const auto requests = server.Requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].find(request.body) != std::string::npos);
    CHECK(requests[0].find("Bearer test-only") != std::string::npos);
}

TEST_CASE("winhttp never forwards credentials or bodies through redirects")
{
    LoopbackHttpServer target(
        [](SOCKET client, std::stop_token)
        {
            LoopbackHttpServer::Send(client, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        });

    const int statuses[] = {302, 307, 308};
    for (const int status : statuses)
    {
        LoopbackHttpServer source(
            [&](SOCKET client, std::stop_token)
            {
                LoopbackHttpServer::Send(client, "HTTP/1.1 " + std::to_string(status) +
                                                     " Redirect\r\nLocation: " + target.Url("localhost") +
                                                     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            });

        agent::transport::WinHttpTransport transport;
        agent::transport::HttpRequest request;
        request.url = source.Url();
        request.headers = {{"Authorization", "Bearer test-only"}};
        request.body = "private prompt";

        const auto response = transport.PostJson(request);

        CHECK(response.status == status);
        CHECK_FALSE(response.Ok());
        CHECK(target.Requests().empty());
    }
}

TEST_CASE("winhttp bounds chunked and error response bodies")
{
    const std::string replies[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\n12345\r\n0\r\n\r\n",
        "HTTP/1.1 500 Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\n12345",
    };

    for (const std::string& reply : replies)
    {
        LoopbackHttpServer server(
            [&](SOCKET client, std::stop_token)
            {
                LoopbackHttpServer::Send(client, reply);
            });

        agent::transport::WinHttpTransport transport;
        agent::transport::HttpRequest request;
        request.url = server.Url();
        request.max_response_bytes = 4;

        const auto response = transport.PostJson(request);
        CHECK_FALSE(response.Ok());
        CHECK(response.error.find("size limit") != std::string::npos);
        CHECK(response.body.empty());
    }
}

TEST_CASE("winhttp enforces total deadline on a continuously trickling response")
{
    LoopbackHttpServer server(
        [](SOCKET client, std::stop_token token)
        {
            LoopbackHttpServer::Send(client, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
            for (int index = 0; index < 100 && !token.stop_requested(); ++index)
            {
                if (!LoopbackHttpServer::Send(client, "x"))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });

    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = server.Url();
    request.timeout_ms = 200;

    const auto started = std::chrono::steady_clock::now();
    const auto response = transport.PostJson(request);

    CHECK(response.error.find("deadline") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("winhttp reports a connection interrupted before the body is complete")
{
    LoopbackHttpServer server(
        [](SOCKET client, std::stop_token)
        {
            LoopbackHttpServer::Send(client, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\nx");
        });

    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = server.Url();

    const auto response = transport.PostJson(request);
    CHECK_FALSE(response.Ok());
    CHECK_FALSE(response.error.empty());
    CHECK(response.body.empty());
}

TEST_CASE("winhttp cancels a request waiting for response headers")
{
    LoopbackHttpServer server(
        [](SOCKET, std::stop_token token)
        {
            while (!token.stop_requested())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = server.Url();
    request.timeout_ms = 5000;
    std::stop_source stop;
    request.stop_token = stop.get_token();
    std::jthread cancel(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });

    const auto started = std::chrono::steady_clock::now();
    const auto response = transport.PostJson(request);

    CHECK(response.error.find("cancelled") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("winhttp rejects invalid limits and pre-cancelled requests")
{
    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = "http://127.0.0.1/";
    const int timeouts[] = {0, -1};
    for (const int timeout : timeouts)
    {
        request.timeout_ms = timeout;
        CHECK(transport.PostJson(request).error.find("positive") != std::string::npos);
    }

    request.timeout_ms = 1000;
    std::stop_source stop;
    stop.request_stop();
    request.stop_token = stop.get_token();
    CHECK(transport.PostJson(request).error.find("cancelled") != std::string::npos);
}

TEST_CASE("winhttp transport rejects an invalid URL")
{
    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = "not-a-url";

    const agent::transport::HttpResponse response = transport.PostJson(request);

    CHECK_FALSE(response.Ok());
    CHECK_FALSE(response.error.empty());
}

TEST_CASE("winhttp transport tolerates an empty body")
{
    agent::transport::WinHttpTransport transport;
    agent::transport::HttpRequest request;
    request.url = "not-a-url";
    request.body.clear();

    const agent::transport::HttpResponse response = transport.PostJson(request);

    CHECK_FALSE(response.Ok());
    CHECK_FALSE(response.error.empty());
}

#endif
