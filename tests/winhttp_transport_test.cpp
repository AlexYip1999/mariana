#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#if defined(_WIN32)

#include "agent/transport/winhttp_transport.h"

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
