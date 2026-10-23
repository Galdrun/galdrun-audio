#include "matp/AudioServer.h"

#include "matp/Protocol.h"
#include "matp/SocketPacketSink.h"
#include "net/Socket.h"
#include "util/Logger.h"

#include <algorithm>
#include <atomic>
#include <list>
#include <optional>
#include <thread>
#include <utility>

namespace multiroom {
namespace {

// How long select() waits before the loop comes round again. Like the control
// listener's, it is not a protocol timeout -- it is how often the thread
// notices a stop request and looks at the attach deadline.
constexpr int kSelectTimeoutMs = 20;

// One read per socket per pass. STREAM_ATTACH is a couple of hundred bytes and
// is the only thing a client sends here.
constexpr size_t kReadChunkBytes = 4096;

// What a connection may send before it has said which room it is. A
// STREAM_ATTACH is the first thing on the wire, so anything past this is a
// peer filling this process's memory rather than attaching.
constexpr size_t kMaxUnattachedBytes = 64 * 1024;

} // namespace

const char* errorName(AudioServerError error)
{
    switch (error)
    {
    case AudioServerError::SocketLibrary:
        return "SocketLibrary";
    case AudioServerError::BindFailed:
        return "BindFailed";
    case AudioServerError::Unsupported:
        return "Unsupported";
    case AudioServerError::NoHandler:
        return "NoHandler";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------

class AudioServer::Impl
{
public:
    Impl(const IClock& clock, AudioServerSettings settings)
        : clock_(clock), settings_(std::move(settings))
    {
    }

    ~Impl()
    {
        thread_.request_stop();
        net::shutdownSocket(listener_);
        if (thread_.joinable())
            thread_.join();

        for (Connection& connection : connections_)
            net::closeSocket(connection.socket);
        connections_.clear();
        net::closeSocket(listener_);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] Status<AudioServerError> bind(uint16_t& portOut)
    {
        if (!library_.isReady())
            return fail(AudioServerError::SocketLibrary, library_.error());

        std::string error;
        listener_ = net::listenOnPort(settings_.port, settings_.backlog, error);
        if (listener_ == net::kInvalidSocket)
            return fail(AudioServerError::BindFailed, std::move(error));

        uint16_t bound = 0;
        if (!net::boundPort(listener_, bound))
        {
            net::closeSocket(listener_);
            listener_ = net::kInvalidSocket;
            return fail(AudioServerError::BindFailed,
                        "could not read back the bound port: " +
                            net::errorMessage(net::lastError()));
        }

        portOut = bound;
        return {};
    }

    void run()
    {
        thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
    }

    [[nodiscard]] bool listening() const noexcept { return listening_.load(); }
    [[nodiscard]] uint64_t accepted() const noexcept { return accepted_.load(); }
    [[nodiscard]] uint64_t refused() const noexcept { return refused_.load(); }
    [[nodiscard]] uint64_t attached() const noexcept { return attached_.load(); }
    [[nodiscard]] uint64_t rejected() const noexcept { return rejected_.load(); }
    [[nodiscard]] uint64_t failed() const noexcept { return failed_.load(); }
    [[nodiscard]] size_t connections() const noexcept { return connectionCount_.load(); }
    [[nodiscard]] size_t waiting() const noexcept { return waiting_.load(); }

private:
    struct Connection
    {
        net::Socket socket = net::kInvalidSocket;
        matp::FrameReader reader;
        uint64_t acceptedAtNs = 0;
        size_t received = 0;

        // Handed to a room, or finished with. Either way this listener stops
        // watching it; `close` says whether the descriptor is still ours.
        bool done = false;
        bool close = false;

        // An attach the probe answered Later: kept, and asked about again on
        // every pass (phase 35).
        std::optional<StreamAttachment> waiting;
    };

    void loop(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            net::Socket highest = listener_;

            for (Connection& connection : connections_)
            {
                FD_SET(connection.socket, &readable);
                highest = std::max(highest, connection.socket);
            }

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = kSelectTimeoutMs * 1000;

            const int ready = net::waitReady(highest, &readable, nullptr, nullptr, timeout);
            if (ready < 0 && !net::wouldBlock(net::lastError()))
            {
                MULTIROOM_ERROR("audio listener: select failed: {}",
                          net::errorMessage(net::lastError()));
                listening_.store(false);
                break;
            }

            if (stop.stop_requested())
                break;

            if (ready > 0 && FD_ISSET(listener_, &readable))
                acceptPending();

            for (Connection& connection : connections_)
            {
                if (ready > 0 && FD_ISSET(connection.socket, &readable))
                    readFrom(connection);
                if (connection.done)
                    continue;
                if (connection.waiting)
                    handOver(connection, *connection.waiting);
                else
                    checkDeadline(connection);
            }
            reap();
        }
    }

    void acceptPending()
    {
        for (;;)
        {
            bool empty = false;
            std::string error;
            const net::Socket accepted = net::acceptOne(listener_, empty, error);
            if (empty)
                return;
            if (accepted == net::kInvalidSocket)
            {
                MULTIROOM_WARN("audio listener: {}", error);
                return;
            }

            if (connections_.size() >= settings_.maxConnections)
            {
                MULTIROOM_WARN("audio listener: refusing a connection, already at {} unattached",
                         settings_.maxConnections);
                net::closeSocket(accepted);
                refused_.fetch_add(1);
                continue;
            }

            Connection connection;
            connection.socket = accepted;
            connection.acceptedAtNs = clock_.monotonicNowNs();
            connections_.push_back(std::move(connection));
            connectionCount_.store(connections_.size());
            accepted_.fetch_add(1);
        }
    }

    void readFrom(Connection& connection)
    {
        char buffer[kReadChunkBytes];
        const int received =
            net::receive(connection.socket, buffer, static_cast<int>(sizeof(buffer)));
        if (received == 0)
        {
            finish(connection, connection.waiting
                                   ? "the client closed the connection while it waited for a zone"
                                   : "the client closed the connection before attaching");
            return;
        }
        if (received < 0)
        {
            const int code = net::lastError();
            if (net::wouldBlock(code))
                return;
            finish(connection, net::errorMessage(code));
            return;
        }

        connection.received += static_cast<size_t>(received);
        if (connection.received > kMaxUnattachedBytes)
        {
            finish(connection, "sent " + std::to_string(connection.received) +
                                   " bytes without attaching");
            return;
        }

        connection.reader.append(buffer, static_cast<size_t>(received));

        for (;;)
        {
            matp::Message message;
            std::string error;
            const matp::FrameReader::Result result = connection.reader.next(message, error);
            if (result == matp::FrameReader::Result::Incomplete)
                return;
            if (result == matp::FrameReader::Result::Invalid)
            {
                finish(connection, std::move(error));
                return;
            }
            if (message.type != matp::MessageType::StreamAttach)
            {
                // The audio connection carries one message and then audio in
                // the other direction. Anything else here is a client talking
                // on the wrong socket, and guessing which it meant is worse
                // than saying so.
                finish(connection, message.typeName + " arrived on the audio port");
                return;
            }

            attach(connection, message);
            return;
        }
    }

    void attach(Connection& connection, const matp::Message& message)
    {
        std::string sessionText;
        std::string clientText;
        std::string error;
        if (!matp::parseStreamAttach(message.payload, sessionText, clientText, error))
        {
            finish(connection, std::move(error));
            return;
        }

        const std::optional<SessionId> session = SessionId::fromHex(sessionText);
        if (!session || !session->valid())
        {
            finish(connection, "streamAttach names session \"" + sessionText +
                                   "\", which is not a session id");
            return;
        }

        StreamAttachment attachment;
        attachment.session = *session;
        attachment.client = ClientId(clientText);

        // A second attach on a waiting connection replaces the first.
        handOver(connection, attachment);
    }

    // By value: the attachment a waiting connection is asked about again is
    // `connection.waiting` itself, which is reset below.
    void handOver(Connection& connection, StreamAttachment attachment)
    {
        const AttachReadiness readiness =
            settings_.readiness ? settings_.readiness(attachment) : AttachReadiness::Now;
        if (readiness == AttachReadiness::Later)
        {
            if (!connection.waiting || connection.waiting->session != attachment.session)
            {
                MULTIROOM_INFO("audio listener: {} waits for a zone to hold session {}",
                         attachment.client.str(), attachment.session.toHex());
            }
            if (!connection.waiting)
                waiting_.fetch_add(1);
            connection.waiting = attachment;
            return;
        }

        const bool waited = connection.waiting.has_value();
        if (waited)
        {
            connection.waiting.reset();
            waiting_.fetch_sub(1);
        }

        if (readiness == AttachReadiness::Never)
        {
            connection.done = true;
            connection.close = true;
            rejected_.fetch_add(1);
            MULTIROOM_WARN("audio listener: no room holds session {} for {}",
                     attachment.session.toHex(), attachment.client.str());
            return;
        }

        // The handover. The descriptor goes with the sink, so the connection
        // is done here whether or not the handler took it: what it did not
        // take, the sink closes when it is destroyed at the end of the call.
        auto sink = std::make_unique<SocketPacketSink>(connection.socket);
        connection.socket = net::kInvalidSocket;
        connection.done = true;
        connection.close = false;

        if (settings_.onAttach(attachment, std::move(sink)))
        {
            attached_.fetch_add(1);
            MULTIROOM_INFO("audio listener: {} attached to session {}{}", attachment.client.str(),
                     attachment.session.toHex(), waited ? " after waiting" : "");
            return;
        }

        rejected_.fetch_add(1);
        MULTIROOM_WARN("audio listener: no room holds session {} for {}", attachment.session.toHex(),
                 attachment.client.str());
    }

    void checkDeadline(Connection& connection)
    {
        const uint64_t waitedNs = clock_.monotonicNowNs() - connection.acceptedAtNs;
        if (waitedNs < settings_.attachTimeoutNs)
            return;
        finish(connection, "said nothing for " + std::to_string(waitedNs / 1000000ULL) + " ms");
    }

    void finish(Connection& connection, std::string reason)
    {
        if (connection.done)
            return;
        connection.done = true;
        connection.close = true;
        if (connection.waiting)
        {
            // Not a failure: a client that prepares a new stream opens a new
            // connection for it and closes the one that was waiting.
            connection.waiting.reset();
            waiting_.fetch_sub(1);
            MULTIROOM_INFO("audio listener: closing a waiting connection ({})", reason);
            return;
        }
        failed_.fetch_add(1);
        MULTIROOM_INFO("audio listener: closing an unattached connection ({})", reason);
    }

    void reap()
    {
        for (auto it = connections_.begin(); it != connections_.end();)
        {
            if (!it->done)
            {
                ++it;
                continue;
            }
            if (it->close)
                net::closeSocket(it->socket);
            it = connections_.erase(it);
            connectionCount_.store(connections_.size());
        }
    }

    const IClock& clock_;
    AudioServerSettings settings_;

    net::Library library_;
    net::Socket listener_ = net::kInvalidSocket;

    std::list<Connection> connections_;

    std::atomic<bool> listening_{true};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> refused_{0};
    std::atomic<uint64_t> attached_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<size_t> connectionCount_{0};
    std::atomic<size_t> waiting_{0};

    // Last, so that it is stopped before anything it uses is destroyed.
    std::jthread thread_;
};

// ---------------------------------------------------------------------------

AudioServer::AudioServer(std::unique_ptr<Impl> impl, uint16_t port)
    : impl_(std::move(impl)), port_(port)
{
}

AudioServer::~AudioServer() = default;

Result<std::unique_ptr<AudioServer>, AudioServerError>
AudioServer::start(const IClock& clock, AudioServerSettings settings)
{
    if (settings.maxConnections == 0 || settings.maxConnections >= FD_SETSIZE)
    {
        return fail(AudioServerError::Unsupported,
                    "maxConnections must be between 1 and " +
                        std::to_string(FD_SETSIZE - 1) +
                        " on this platform, which is what select() can watch");
    }
    if (!settings.onAttach)
    {
        // A component that is not implemented yet fails explicitly: a listener
        // with nowhere to put a connection would accept rooms and drop them
        // without saying why.
        return fail(AudioServerError::NoHandler,
                    "an audio listener needs somewhere to hand an attached connection");
    }
    if (settings.attachTimeoutNs == 0)
    {
        return fail(AudioServerError::NoHandler,
                    "attachTimeoutNs of zero would close every connection before it spoke");
    }

    auto impl = std::make_unique<Impl>(clock, std::move(settings));

    uint16_t port = 0;
    const Status<AudioServerError> bound = impl->bind(port);
    if (!bound)
        return std::unexpected(bound.error());

    impl->run();

    MULTIROOM_INFO("audio listener on port {}", port);
    return std::unique_ptr<AudioServer>(new AudioServer(std::move(impl), port));
}

bool AudioServer::listening() const noexcept
{
    return impl_->listening();
}

uint64_t AudioServer::acceptedCount() const noexcept
{
    return impl_->accepted();
}

uint64_t AudioServer::refusedCount() const noexcept
{
    return impl_->refused();
}

uint64_t AudioServer::attachedCount() const noexcept
{
    return impl_->attached();
}

uint64_t AudioServer::rejectedCount() const noexcept
{
    return impl_->rejected();
}

uint64_t AudioServer::failedCount() const noexcept
{
    return impl_->failed();
}

size_t AudioServer::connectionCount() const noexcept
{
    return impl_->connections();
}

size_t AudioServer::waitingCount() const noexcept
{
    return impl_->waiting();
}

} // namespace multiroom
