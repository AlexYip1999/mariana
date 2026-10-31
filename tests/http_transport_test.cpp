#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "support/fake_http_transport.h"

namespace
{

agent::transport::HttpRequest MakeRequest()
{
    agent::transport::HttpRequest request;
    request.url = "https://example.test/chat";
    request.body = "test-body";
    request.headers = {{"Content-Type", "application/json"}};
    return request;
}

} // namespace

TEST_CASE("fake http transport records the request")
{
    agent::tests::FakeHttpTransport fake;
    fake.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = "ok";
            return response;
        });

    const auto response = fake.PostJson(MakeRequest());

    CHECK(response.Ok());
    CHECK(fake.RequestCount() == 1);
    CHECK(fake.LastRequest().url == "https://example.test/chat");
    CHECK(fake.LastRequest().body == "test-body");
    CHECK(fake.LastRequest().timeout_ms == 30000);
}

TEST_CASE("Ok() requires a 2xx status and no transport error")
{
    agent::transport::HttpResponse success;
    success.status = 204;
    CHECK(success.Ok());

    agent::transport::HttpResponse client_error;
    client_error.status = 401;
    CHECK_FALSE(client_error.Ok());

    agent::transport::HttpResponse transport_error;
    transport_error.status = 200;
    transport_error.error = "timeout";
    CHECK_FALSE(transport_error.Ok());
}
