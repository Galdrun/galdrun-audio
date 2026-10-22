#include "TestSupport.h"

#include "control/ControlServer.h"
#include "discovery/NodeId.h"
#include "net/Socket.h" // not a public header; the test drives it directly

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>

using namespace multiroom;

namespace {

class CountingHandler : public IControlHandler
{
public:
    json::Value handle(const ControlRequest& request) override
    {
        calls.fetch_add(1);
        json::Value response = makeControlSuccess(request);
        response["echo"] = json::Value(request.name);
        return response;
    }

    std::atomic<int> calls{0};
};

// A blocking loopback client with a receive timeout, so a broken server shows
// up as a failing assertion rather than a hung test run.
class TestClient
{
public:
    explicit TestClient(uint16_t port)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == net::kInvalidSocket)
            return;

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            net::closeSocket(socket_);
            socket_ = net::kInvalidSocket;
            return;
        }

#if defined(_WIN32)
        DWORD timeout = 5000;
#else
        timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
#endif
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                     sizeof(timeout));
    }

    ~TestClient() { close(); }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    bool connected() const { return socket_ != net::kInvalidSocket; }

    bool send(const std::string& text)
    {
        size_t offset = 0;
        while (offset < text.size())
        {
            const int sent = net::transmit(socket_, text.data() + offset,
                                           static_cast<int>(text.size() - offset));
            if (sent <= 0)
                return false;
            offset += static_cast<size_t>(sent);
        }
        return true;
    }

    // Reads up to and including the next newline. Empty on timeout or close.
    std::string readLine()
    {
        for (;;)
        {
            const size_t newline = inbox_.find('\n');
            if (newline != std::string::npos)
            {
                std::string line = inbox_.substr(0, newline);
                inbox_.erase(0, newline + 1);
                return line;
            }

            char buffer[1024];
            const int received = net::receive(socket_, buffer, static_cast<int>(sizeof(buffer)));
            if (received <= 0)
                return std::string();
            inbox_.append(buffer, static_cast<size_t>(received));
        }
    }

    // Everything until the server closes the connection, as an HTTP
    // response ends.
    std::string readAll()
    {
        std::string text = inbox_;
        inbox_.clear();
        char buffer[1024];
        int received = 0;
        while ((received = net::receive(socket_, buffer, static_cast<int>(sizeof(buffer)))) > 0)
            text.append(buffer, static_cast<size_t>(received));
        return text;
    }

    std::string readBytes(size_t count)
    {
        while (inbox_.size() < count)
        {
            char buffer[1024];
            const int received = net::receive(socket_, buffer, sizeof(buffer));
            if (received <= 0)
                return std::string();
            inbox_.append(buffer, static_cast<size_t>(received));
        }
        const std::string text = inbox_.substr(0, count);
        inbox_.erase(0, count);
        return text;
    }

    std::string readChunk()
    {
        const std::string length = readLine();
        const unsigned long bytes = std::strtoul(length.c_str(), nullptr, 16);
        CHECK(bytes > 0 && bytes <= 128 * 1024);
        const std::string text = readBytes(bytes);
        CHECK_EQ(text.size(), static_cast<size_t>(bytes));
        CHECK_EQ(readBytes(2), std::string("\r\n"));
        return text;
    }

    std::string readInputChunk()
    {
        for (int i = 0; i < 20; ++i)
        {
            const std::string chunk = readChunk();
            if (chunk.find("event: system-usage") != 0 && chunk.find(": keepalive") != 0)
                return chunk;
        }
        CHECK(false);
        return std::string();
    }

    void close()
    {
        if (socket_ != net::kInvalidSocket)
        {
            net::closeSocket(socket_);
            socket_ = net::kInvalidSocket;
        }
    }

private:
    net::Socket socket_ = net::kInvalidSocket;
    std::string inbox_;
};

json::Value parseOrFail(const std::string& text)
{
    json::Value value;
    std::string error;
    CHECK(!text.empty());
    CHECK(json::parse(text, value, error));
    return value;
}

bool fieldBool(const json::Value& object, const char* key, bool fallback)
{
    const json::Value* value = object.find(key);
    return value != nullptr ? value->asBool(fallback) : fallback;
}

std::string fieldString(const json::Value& object, const char* key)
{
    const json::Value* value = object.find(key);
    return value != nullptr ? value->asString() : std::string();
}

} // namespace

MULTIROOM_TEST(control_server_answers_on_loopback)
{
    // Port 0: the OS picks a free one, so the test never collides with a
    // running client or with a parallel test run.
    net::Library library;
    CHECK(library.isReady());

    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));
    CHECK(server.isRunning());
    CHECK(server.boundPort() != 0);

    TestClient client(server.boundPort());
    CHECK(client.connected());
    CHECK(client.send("{\"command\":\"status\",\"id\":1}\n"));

    const json::Value response = parseOrFail(client.readLine());
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "echo"), std::string("status"));
    CHECK_EQ(response.find("id")->asInt(), static_cast<int64_t>(1));
    CHECK_EQ(handler.calls.load(), 1);

    server.stop();
    CHECK(!server.isRunning());
}

MULTIROOM_TEST(control_server_keeps_serving_after_a_bad_request)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));

    TestClient client(server.boundPort());
    CHECK(client.connected());

    // Garbage, an unknown command, a blank line and then a valid request, all
    // on one connection: the socket must survive every one of them.
    CHECK(client.send("not json\n"));
    CHECK_EQ(fieldBool(parseOrFail(client.readLine()), "ok", true), false);

    CHECK(client.send("{\"command\":\"reboot\"}\n"));
    const json::Value unknown = parseOrFail(client.readLine());
    CHECK_EQ(fieldBool(unknown, "ok", true), false);
    CHECK_EQ(fieldString(unknown, "error"), std::string(control_error::kUnknownCommand));

    CHECK(client.send("\r\n"));
    CHECK(client.send("{\"command\":\"getServer\"}\r\n"));
    CHECK_EQ(fieldBool(parseOrFail(client.readLine()), "ok", false), true);

    CHECK_EQ(handler.calls.load(), 1); // only the valid command reached it
    server.stop();
}

MULTIROOM_TEST(control_server_survives_a_client_that_disappears_mid_request)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));

    {
        TestClient client(server.boundPort());
        CHECK(client.connected());
        // Half a request, then gone without a newline and without reading.
        CHECK(client.send("{\"command\":\"sta"));
    }

    // The server thread must still be there for the next caller.
    TestClient second(server.boundPort());
    CHECK(second.connected());
    CHECK(second.send("{\"command\":\"status\"}\n"));
    CHECK_EQ(fieldBool(parseOrFail(second.readLine()), "ok", false), true);

    CHECK(server.isRunning());
    CHECK_EQ(server.acceptedConnections(), static_cast<uint64_t>(2));
    server.stop();
}

MULTIROOM_TEST(control_server_handles_several_requests_in_one_write)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));

    TestClient client(server.boundPort());
    CHECK(client.connected());
    // Pipelined, and split across a line boundary in the middle of a request
    // to make sure the reader buffers rather than assuming one read is one line.
    CHECK(client.send("{\"command\":\"status\",\"id\":1}\n{\"command\":\"getSer"));
    CHECK(client.send("ver\",\"id\":2}\n"));

    CHECK_EQ(parseOrFail(client.readLine()).find("id")->asInt(), static_cast<int64_t>(1));
    CHECK_EQ(parseOrFail(client.readLine()).find("id")->asInt(), static_cast<int64_t>(2));
    CHECK_EQ(handler.calls.load(), 2);

    server.stop();
}

MULTIROOM_TEST(control_server_reports_a_port_it_cannot_bind)
{
    net::Library library;
    CountingHandler handler;

    ControlServer first;
    std::string error;
    CHECK(first.start("127.0.0.1", 0, handler, error));

    // A second client on the same port is the common case for this failure,
    // and start() has to report it on the calling thread.
    ControlServer second;
    CHECK(!second.start("127.0.0.1", first.boundPort(), handler, error));
    CHECK(!error.empty());
    CHECK(!second.isRunning());

    CHECK(!first.start("127.0.0.1", 0, handler, error)); // already running
    first.stop();
}

MULTIROOM_TEST(control_server_rejects_an_unusable_bind_address)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;

    // Not a numeric address: the bind address is a literal by design, so this
    // must fail rather than trigger a DNS lookup at startup.
    CHECK(!server.start("not-an-address", 0, handler, error));
    CHECK(!error.empty());
    CHECK(!server.isRunning());
}

namespace {

std::string httpExchange(uint16_t port, const std::string& request)
{
    TestClient client(port);
    CHECK(client.connected());
    CHECK(client.send(request));
    return client.readAll();
}

std::string postApi(const std::string& host, const std::string& contentType,
                    const std::string& body)
{
    return "POST /api HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: " + contentType +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool startsWith(const std::string& text, const char* prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

} // namespace

MULTIROOM_TEST(control_server_serves_the_page_to_a_browser)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));

    const std::string page =
        httpExchange(server.boundPort(), "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    CHECK(startsWith(page, "HTTP/1.1 200 OK\r\n"));
    CHECK(page.find("Content-Type: text/html") != std::string::npos);
    CHECK(page.find("</html>") != std::string::npos);

    CHECK(startsWith(httpExchange(server.boundPort(),
                                  "GET /favicon.ico HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"),
                     "HTTP/1.1 404 "));
    CHECK_EQ(handler.calls.load(), 0);
    server.stop();
}

MULTIROOM_TEST(control_server_answers_control_requests_over_http)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));

    // Split mid-body: the server has to wait for Content-Length bytes.
    const std::string request =
        postApi("127.0.0.1:4720", "application/json", "{\"command\":\"status\",\"id\":3}");
    TestClient client(server.boundPort());
    CHECK(client.connected());
    CHECK(client.send(request.substr(0, request.size() - 5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(client.send(request.substr(request.size() - 5)));
    const std::string response = client.readAll();
    CHECK(startsWith(response, "HTTP/1.1 200 OK\r\n"));
    const json::Value body = parseOrFail(response.substr(response.find("\r\n\r\n") + 4));
    CHECK_EQ(fieldString(body, "echo"), std::string("status"));
    CHECK_EQ(body.find("id")->asInt(), static_cast<int64_t>(3));
    CHECK_EQ(handler.calls.load(), 1);
    server.stop();
}

MULTIROOM_TEST(control_server_refuses_cross_site_http_requests)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));

    const std::string body = "{\"command\":\"setServer\",\"params\":{\"server\":\"x:1\"}}";
    // A form post from another site cannot set a JSON content type...
    CHECK(startsWith(httpExchange(server.boundPort(), postApi("127.0.0.1", "text/plain", body)),
                     "HTTP/1.1 415 "));
    // ...and a rebound name is not this machine's.
    CHECK(startsWith(httpExchange(server.boundPort(),
                                  postApi("evil.example:4720", "application/json", body)),
                     "HTTP/1.1 403 "));
    // A name that only starts with this machine's is still the attacker's.
    const std::string own = discovery::hostName();
    CHECK(startsWith(httpExchange(server.boundPort(),
                                  postApi(own + ".evil.example", "application/json", body)),
                     "HTTP/1.1 403 "));
    CHECK(startsWith(httpExchange(server.boundPort(),
                                  postApi("[::1]:4720", "application/json", body)),
                     "HTTP/1.1 200 "));
    CHECK(startsWith(httpExchange(server.boundPort(),
                                  postApi(own + ".local:4720", "application/json", body)),
                     "HTTP/1.1 200 "));
    CHECK_EQ(handler.calls.load(), 2);
    server.stop();
}

namespace {
std::string openInputFeed(TestClient& client, const std::string& origin = std::string())
{
    CHECK(client.send("GET /api/input-events HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
                      (origin.empty() ? std::string() : "Origin: " + origin + "\r\n") + "\r\n"));
    std::string headers;
    for (int i = 0; i < 16; ++i)
    {
        const std::string line = client.readLine();
        CHECK(!line.empty());
        headers += line + "\n";
        if (line == "\r")
            return headers;
    }
    CHECK(false);
    return headers;
}

json::Value inputEventData(const std::string& chunk, const char* event)
{
    CHECK(chunk.find(std::string("event: ") + event + "\n") == 0);
    const size_t start = chunk.find("data: ");
    CHECK(start != std::string::npos);
    return parseOrFail(chunk.substr(start + 6));
}
}

MULTIROOM_TEST(control_server_pushes_input_edges_and_current_state_to_multiple_viewers)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    server.setInputButtons({"key\"1"});
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));
    TestClient first(server.boundPort());
    const std::string headers = openInputFeed(first, "http://127.0.0.1:80");
    CHECK(startsWith(headers, "HTTP/1.1 200 OK"));
    CHECK(headers.find("Content-Type: text/event-stream") != std::string::npos);
    CHECK(headers.find("Access-Control-Allow-Origin: http://127.0.0.1:80") != std::string::npos);
    CHECK(first.readInputChunk().find("retry: ") == 0);
    const json::Value initial = inputEventData(first.readInputChunk(), "input-snapshot");
    CHECK_EQ(initial.size(), static_cast<size_t>(1));
    CHECK_EQ(fieldString(initial.at(0), "id"), std::string("key\"1"));
    CHECK(!fieldBool(initial.at(0), "pressed", true));

    server.publishInputState("key\"1", true);
    CHECK(fieldBool(inputEventData(first.readInputChunk(), "input-state"), "pressed", false));
    TestClient second(server.boundPort());
    CHECK(startsWith(openInputFeed(second), "HTTP/1.1 200 OK"));
    second.readInputChunk(); // retry interval
    CHECK(fieldBool(inputEventData(second.readInputChunk(), "input-snapshot").at(0), "pressed", false));

    server.publishInputState("unknown", true);
    server.publishInputState("key\"1", true); // duplicate level emits nothing
    server.publishInputState("key\"1", false);
    CHECK(!fieldBool(inputEventData(first.readInputChunk(), "input-state"), "pressed", true));
    CHECK(!fieldBool(inputEventData(second.readInputChunk(), "input-state"), "pressed", true));
    CHECK(startsWith(httpExchange(server.boundPort(),
                                  postApi("127.0.0.1", "application/json", "{\"command\":\"status\"}")),
                     "HTTP/1.1 200 OK"));
    CHECK_EQ(handler.calls.load(), 1);
    CHECK(first.send("{\"command\":\"status\"}\n"));
    CHECK(first.readAll().empty()); // feed cannot be reused to dispatch commands
    CHECK_EQ(handler.calls.load(), 1);
    server.stop();
}

MULTIROOM_TEST(control_server_limits_input_viewers_and_rejects_unrelated_origins)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));
    TestClient foreign(server.boundPort());
    CHECK(startsWith(openInputFeed(foreign, "http://evil.example"), "HTTP/1.1 403"));
    std::vector<std::unique_ptr<TestClient>> viewers;
    for (int i = 0; i < 4; ++i)
    {
        viewers.emplace_back(new TestClient(server.boundPort()));
        CHECK(startsWith(openInputFeed(*viewers.back()), "HTTP/1.1 200 OK"));
        viewers.back()->readInputChunk();
        viewers.back()->readInputChunk();
    }
    TestClient excess(server.boundPort());
    CHECK(startsWith(openInputFeed(excess), "HTTP/1.1 503"));
    viewers[0]->close();
    TestClient replacement(server.boundPort());
    CHECK(startsWith(openInputFeed(replacement), "HTTP/1.1 200 OK"));
    replacement.readInputChunk();
    replacement.readInputChunk();
    CHECK_EQ(handler.calls.load(), 0);
    server.stop();
}

MULTIROOM_TEST(control_server_pushes_system_usage_without_dispatching_commands)
{
    net::Library library;
    CountingHandler handler;
    ControlServer server;
    std::string error;
    CHECK(server.start("127.0.0.1", 0, handler, error));
    TestClient client(server.boundPort());
    CHECK(startsWith(openInputFeed(client), "HTTP/1.1 200 OK"));
    client.readChunk(); // retry
    client.readChunk(); // button snapshot
    for (int sample = 0; sample < 2; ++sample)
    {
        const json::Value usage = inputEventData(client.readChunk(), "system-usage");
        const json::Value* ram = usage.find("ramPercent");
        CHECK(ram != nullptr && ram->isNumber());
        CHECK(ram->asDouble() >= 0.0 && ram->asDouble() <= 100.0);
        const json::Value* total = usage.find("ramTotalBytes");
        const json::Value* used = usage.find("ramUsedBytes");
        CHECK(total != nullptr && total->isNumber() && total->asDouble() > 0.0);
        CHECK(used != nullptr && used->isNumber() && used->asDouble() >= 0.0 && used->asDouble() <= total->asDouble());
        CHECK(std::abs(ram->asDouble() - 100.0 * used->asDouble() / total->asDouble()) < 0.001);
        CHECK(usage.find("wifiRssiDbm") != nullptr);
        CHECK(!fieldString(usage, "cpuModel").empty());
        CHECK(usage.find("cpuMhz") != nullptr);
        if (sample == 1)
        {
            const json::Value* cpu = usage.find("cpuPercent");
            CHECK(cpu != nullptr && cpu->isNumber());
            CHECK(cpu->asDouble() >= 0.0 && cpu->asDouble() <= 100.0);
        }
    }
    CHECK_EQ(handler.calls.load(), 0);
    server.stop();
}
