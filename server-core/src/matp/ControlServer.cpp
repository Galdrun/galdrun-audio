#include "matp/ControlServer.h"

#include "net/Socket.h"
#include "util/Logger.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <iterator>
#include <list>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace multiroom {
namespace {

// How long select() waits before the loop comes round again. It is not a
// protocol timeout -- every one of those is the session's and is measured
// against the injected clock -- it is only how often the thread notices a stop
// request and gives each session a poll(). Twenty milliseconds is far finer
// than the five-second ping it services and coarse enough to cost nothing.
constexpr int kSelectTimeoutMs = 20;

// One read per socket per pass. A control message is a few hundred bytes and
// the largest is an AUDIO_CAPABILITIES list of a few kilobytes; a socket with
// more waiting is simply read again next pass, which is what keeps one busy
// peer from holding the loop.
constexpr size_t kReadChunkBytes = 8192;

// How long an ending connection is given to get its ERROR out. A peer that is
// not reading never takes it, and its slot is wanted by a room that is.
constexpr uint64_t kDrainDeadlineNs = 2'000'000'000ULL;

} // namespace

const char* errorName(ControlServerError error)
{
    switch (error)
    {
    case ControlServerError::SocketLibrary:
        return "SocketLibrary";
    case ControlServerError::BindFailed:
        return "BindFailed";
    case ControlServerError::Unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------

class ControlServer::Impl
{
public:
    Impl(const IClock& clock, ControlServerSettings settings)
        : clock_(clock), settings_(settings), registry_(settings.maxConnections)
    {
    }

    ~Impl()
    {
        // The thread is stopped before anything it touches is destroyed. The
        // listening socket is shut down as well as stop-requested, because a
        // thread sitting in select() would otherwise only notice at the next
        // timeout -- correct, but twenty milliseconds slower for no reason.
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

    [[nodiscard]] Status<ControlServerError> bind(uint16_t& portOut)
    {
        if (!library_.isReady())
            return fail(ControlServerError::SocketLibrary, library_.error());

        std::string error;
        listener_ = net::listenOnPort(settings_.port, settings_.backlog, error);
        if (listener_ == net::kInvalidSocket)
            return fail(ControlServerError::BindFailed, std::move(error));

        uint16_t bound = 0;
        if (!net::boundPort(listener_, bound))
        {
            net::closeSocket(listener_);
            listener_ = net::kInvalidSocket;
            return fail(ControlServerError::BindFailed,
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

    [[nodiscard]] uint64_t accepted() const noexcept { return accepted_.load(); }
    [[nodiscard]] uint64_t refused() const noexcept { return refused_.load(); }
    [[nodiscard]] uint64_t closed() const noexcept { return closed_.load(); }
    [[nodiscard]] uint64_t identified() const noexcept { return identified_.load(); }
    [[nodiscard]] size_t connections() const noexcept { return connectionCount_.load(); }
    [[nodiscard]] size_t clients() const noexcept { return clientCount_.load(); }
    [[nodiscard]] uint64_t commandsSent() const noexcept { return commandsSent_.load(); }
    [[nodiscard]] uint64_t startsSent() const noexcept { return startsSent_.load(); }

    // From a zone's pipeline thread. Nothing here touches a session: the
    // mailbox is the thread boundary and the loop is what empties it.
    [[nodiscard]] bool queueCommand(const RoomCommand& command)
    {
        const std::lock_guard<std::mutex> lock(mailboxMutex_);
        const size_t limit = transportCritical(command.kind) ? kMaxQueuedCritical
                                                             : kMaxQueuedCommands;
        if (mailbox_.size() >= limit)
            return false;
        mailbox_.push_back(command);
        return true;
    }

private:
    // What a zone has asked this thread to write. The number is what a zone
    // can produce between two passes of a loop that runs every twenty
    // milliseconds: a start, a pause or a stop per room, and the handful of
    // metadata messages a track boundary or a late join adds to them.
    static constexpr size_t kMaxQueuedCommands = 64;

    // The commands a room's timeline is wrong without. A burst across zones
    // can outrun the number above, and dropping one of these would leave a
    // room playing a line the server has stopped, so they are only refused
    // once the loop has plainly stopped emptying the mailbox.
    static constexpr size_t kMaxQueuedCritical = 4096;

    static bool transportCritical(RoomCommandKind kind)
    {
        switch (kind)
        {
        case RoomCommandKind::Start:
        case RoomCommandKind::Pause:
        case RoomCommandKind::Stop:
        case RoomCommandKind::Flush:
        case RoomCommandKind::TrackBoundary:
        case RoomCommandKind::Renegotiate:
        case RoomCommandKind::WakeStream:
            return true;
        default:
            return false;
        }
    }

    struct Connection
    {
        net::Socket socket = net::kInvalidSocket;
        std::shared_ptr<ClientSession> session;

        // The frame currently going out, and how much of it the socket has
        // taken. A non-blocking send writes what it can; the rest is resumed
        // rather than waited on, which is what keeps one slow room out of
        // every other room's way.
        std::string writing;
        size_t written = 0;

        // Registered under its client id, which only happens once HELLO has
        // named one. The id is kept here as well so that a connection can take
        // itself back out without asking a session that may have been
        // replaced.
        bool registered = false;
        ClientId registeredId;

        // The session is over; what is left in `writing` and in its queue is
        // the ERROR the peer is owed, and the socket closes once that is out.
        bool draining = false;
        uint64_t drainingSinceNs = 0;

        // A send failed: nothing more can be written, so draining is over.
        bool broken = false;
    };

    // One queued message, written into the session that owns that room. The
    // kind is the whole of the decision: which call it is, and nothing else.
    static Status<SessionError> write(ClientSession& session, const RoomCommand& command)
    {
        switch (command.kind)
        {
        case RoomCommandKind::Start:
            return session.startPlayback(command.start);
        case RoomCommandKind::Pause:
            return session.pausePlayback(command.at);
        case RoomCommandKind::Stop:
            return session.stopPlayback(command.at);
        case RoomCommandKind::Flush:
            return session.flushPlayback(command.at);
        case RoomCommandKind::Volume:
            return session.setVolume(command.volume);
        case RoomCommandKind::TrackInfo:
            return session.sendTrackInfo(command.info);
        case RoomCommandKind::TrackBoundary:
            return session.sendTrackBoundary(command.boundary);
        case RoomCommandKind::Renegotiate:
            return session.renegotiate();
        case RoomCommandKind::WakeStream:
            return session.wakeStream();
        case RoomCommandKind::SetAudioDevice:
            return session.setAudioDevice(command.audioDevice.deviceId);
        case RoomCommandKind::RescanAudio:
            return session.rescanAudio();
        case RoomCommandKind::SetInputBindings:
            return session.setInputBindings(command.inputBindings);
        case RoomCommandKind::SetBuffer:
            return session.setBuffer(command.buffer.bufferMs);
        case RoomCommandKind::SetVolumeControl:
            return session.setVolumeControl(command.volumeControl.volumeControl);
        case RoomCommandKind::SetRoomName:
            return session.setRoomName(command.roomName.name);
        case RoomCommandKind::SetDriftCorrection:
            return session.setDriftCorrection(command.driftCorrection.driftCorrection);
        case RoomCommandKind::DriftFeedback:
            return session.sendDriftFeedback(command.driftFeedback);
        }
        return fail(SessionError::UnexpectedMessage, "an unknown room command");
    }

    // The session a zone addressed the command to, where it names one. Empty
    // for the kinds the session fills in itself.
    static const std::string& addressedSession(const RoomCommand& command)
    {
        static const std::string none;
        switch (command.kind)
        {
        case RoomCommandKind::Start:
            return command.start.sessionId;
        case RoomCommandKind::Pause:
        case RoomCommandKind::Stop:
        case RoomCommandKind::Flush:
            return command.at.sessionId;
        case RoomCommandKind::Volume:
            return command.volume.sessionId;
        case RoomCommandKind::TrackInfo:
            return command.info.sessionId;
        case RoomCommandKind::TrackBoundary:
            return command.boundary.sessionId;
        case RoomCommandKind::DriftFeedback:
            return command.driftFeedback.sessionId;
        default:
            return none;
        }
    }

    // Whatever a zone queued since the last pass, written into the sessions
    // that own those rooms. Before select(), so that a start queued a
    // microsecond ago is on the wire this pass rather than the next.
    void deliverCommands()
    {
        std::vector<RoomCommand> pending;
        {
            const std::lock_guard<std::mutex> lock(mailboxMutex_);
            if (mailbox_.empty())
                return;
            pending.assign(std::make_move_iterator(mailbox_.begin()),
                           std::make_move_iterator(mailbox_.end()));
            mailbox_.clear();
        }

        for (const RoomCommand& entry : pending)
        {
            const std::shared_ptr<ClientSession> session = registry_.find(entry.client);
            if (session == nullptr)
            {
                // The room went away between the zone's decision and this
                // pass. Its output is being torn down by the same event, so
                // there is nothing left to tell it.
                MULTIROOM_DEBUG("control listener: no session for {}, {} dropped", entry.client.str(),
                          roomCommandKindName(entry.kind));
                continue;
            }

            // Section 43: decided for a session a reconnect has since
            // replaced. Its timeline is not the new stream's.
            if (const std::string& addressed = addressedSession(entry);
                !addressed.empty() && addressed != session->sessionId().toHex())
            {
                MULTIROOM_DEBUG("control listener: {} was for {}'s replaced session, dropped",
                          roomCommandKindName(entry.kind), entry.client.str());
                continue;
            }

            if (const Status<SessionError> sent = write(*session, entry); !sent)
            {
                MULTIROOM_WARN("control listener: {} could not be sent a {}: {}",
                         entry.client.str(), roomCommandKindName(entry.kind),
                         describe(sent.error()));
                continue;
            }

            commandsSent_.fetch_add(1);
            if (entry.kind == RoomCommandKind::Start)
                startsSent_.fetch_add(1);
        }
    }

    void loop(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            deliverCommands();

            fd_set readable;
            fd_set writable;
            FD_ZERO(&readable);
            FD_ZERO(&writable);

            FD_SET(listener_, &readable);
            net::Socket highest = listener_;

            for (Connection& connection : connections_)
            {
                // What an ending connection receives cannot change anything,
                // and a peer that has closed would wake select() every pass.
                if (!connection.draining)
                    FD_SET(connection.socket, &readable);
                if (hasPendingWrite(connection))
                    FD_SET(connection.socket, &writable);
                highest = std::max(highest, connection.socket);
            }

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = kSelectTimeoutMs * 1000;

            // Windows ignores the first argument and takes the count from the
            // sets themselves; POSIX wants the highest descriptor plus one.
#if defined(_WIN32)
            static_cast<void>(highest);
            const int ready = ::select(0, &readable, &writable, nullptr, &timeout);
#else
            const int ready =
                ::select(static_cast<int>(highest) + 1, &readable, &writable, nullptr,
                         &timeout);
#endif
            if (ready < 0 && !net::wouldBlock(net::lastError()))
            {
                MULTIROOM_ERROR("control listener: select failed: {}",
                          net::errorMessage(net::lastError()));
                break;
            }

            if (stop.stop_requested())
                break;

            if (ready > 0 && FD_ISSET(listener_, &readable))
                acceptPending();

            serviceConnections(readable, writable, ready > 0);
            reap();
        }
    }

    static bool hasPendingWrite(const Connection& connection)
    {
        return connection.written < connection.writing.size() ||
               connection.session->pendingOutbound() > 0;
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
                MULTIROOM_WARN("control listener: {}", error);
                return;
            }

            if (connections_.size() >= settings_.maxConnections)
            {
                // Bounded, like every other queue here. A peer that opens
                // sockets without speaking is refused at the door rather than
                // allowed to cost this process a session each.
                MULTIROOM_WARN("control listener: refusing a connection, already at {}",
                         settings_.maxConnections);
                net::closeSocket(accepted);
                refused_.fetch_add(1);
                continue;
            }

            Connection connection;
            connection.socket = accepted;
            connection.session = std::make_shared<ClientSession>(randomSessionId(), clock_,
                                                                 settings_.session);
            connection.session->setPeerAddress(net::peerAddress(accepted));
            connections_.push_back(std::move(connection));
            connectionCount_.store(connections_.size());
            accepted_.fetch_add(1);
        }
    }

    void serviceConnections(fd_set& readable, fd_set& writable, bool anyReady)
    {
        for (Connection& connection : connections_)
        {
            if (anyReady && FD_ISSET(connection.socket, &readable))
                readFrom(connection);

            // Time passing, whether or not anything arrived: the ping and the
            // silence timeout are what notice a half-open socket, and a
            // half-open socket is exactly one that never becomes readable.
            if (!connection.draining)
            {
                const Status<SessionError> polled = connection.session->poll();
                if (!polled)
                    beginDraining(connection, describe(polled.error()));
            }

            if (anyReady && FD_ISSET(connection.socket, &writable))
                writeTo(connection);
            else if (hasPendingWrite(connection))
                writeTo(connection); // a fresh frame that select() never saw
        }
    }

    void readFrom(Connection& connection)
    {
        char buffer[kReadChunkBytes];
        const int received =
            net::receive(connection.socket, buffer, static_cast<int>(sizeof(buffer)));

        if (received == 0)
        {
            // Orderly shutdown. Nothing to tell the peer: it has gone.
            connection.session->close(kClosedByPeer);
            beginDraining(connection, "closed by the client");
            return;
        }
        if (received < 0)
        {
            const int code = net::lastError();
            if (net::wouldBlock(code))
                return;
            connection.session->close(net::errorMessage(code));
            beginDraining(connection, net::errorMessage(code));
            return;
        }
        if (connection.draining)
            return; // whatever arrives now cannot change the outcome

        const Status<SessionError> handled = connection.session->receive(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer),
                                       static_cast<size_t>(received)));

        // Registration is here rather than in the session because a session
        // does not know there is a registry: it learns its own id from HELLO,
        // and this is what turns that into a room.
        registerIfNamed(connection);

        if (!handled)
            beginDraining(connection, describe(handled.error()));
    }

    void registerIfNamed(Connection& connection)
    {
        if (connection.registered || connection.session->clientId().empty())
            return;

        Result<std::shared_ptr<ClientSession>, RegistryError> displaced =
            registry_.add(connection.session);
        if (!displaced)
        {
            connection.session->close(describe(displaced.error()));
            beginDraining(connection, describe(displaced.error()));
            return;
        }

        connection.registered = true;
        connection.registeredId = connection.session->clientId();
        identified_.fetch_add(1);
        clientCount_.store(registry_.size());

        if (*displaced == nullptr)
            return;

        // Section 43: the same room on a new socket. The connection the old
        // session belonged to is finished -- it is a room that is gone whether
        // or not its TCP connection has noticed.
        for (Connection& other : connections_)
        {
            if (&other == &connection || other.session != *displaced)
                continue;
            other.registered = false; // the registry no longer points at it
            other.session->close(kReplacedByReconnect);
            beginDraining(other, "replaced by a reconnect");
        }
    }

    void writeTo(Connection& connection)
    {
        if (connection.broken)
            return;
        for (;;)
        {
            if (connection.written >= connection.writing.size())
            {
                connection.writing.clear();
                connection.written = 0;
                if (!connection.session->nextOutbound(connection.writing))
                    return;
            }

            const size_t remaining = connection.writing.size() - connection.written;
            const int sent = net::transmit(connection.socket,
                                           connection.writing.data() + connection.written,
                                           static_cast<int>(remaining));
            if (sent < 0)
            {
                const int code = net::lastError();
                if (net::wouldBlock(code))
                    return; // resumed next pass; nothing waits on it
                connection.session->close(net::errorMessage(code));
                beginDraining(connection, net::errorMessage(code));
                connection.broken = true;
                return;
            }
            connection.written += static_cast<size_t>(sent);
        }
    }

    void beginDraining(Connection& connection, const std::string& reason)
    {
        if (connection.draining)
            return;
        connection.draining = true;
        connection.drainingSinceNs = clock_.monotonicNowNs();
        MULTIROOM_INFO("control listener: connection for {} ending ({})",
                 connection.registeredId.empty() ? "an unidentified client"
                                                 : connection.registeredId.str().c_str(),
                 reason);
    }

    void reap()
    {
        for (auto it = connections_.begin(); it != connections_.end();)
        {
            if (!it->draining)
            {
                ++it;
                continue;
            }

            // One more attempt to get the ERROR out, for as long as the
            // deadline allows. A peer that is not reading does not get it, and
            // that is the end of the matter.
            writeTo(*it);
            const bool expired =
                clock_.monotonicNowNs() - it->drainingSinceNs >= kDrainDeadlineNs;
            if (!it->broken && !expired && hasPendingWrite(*it))
            {
                ++it;
                continue;
            }

            if (it->registered)
                registry_.remove(it->registeredId, it->session.get());

            net::closeSocket(it->socket);
            it = connections_.erase(it);
            closed_.fetch_add(1);
            connectionCount_.store(connections_.size());
            clientCount_.store(registry_.size());
        }
    }

    const IClock& clock_;
    ControlServerSettings settings_;

    net::Library library_;
    net::Socket listener_ = net::kInvalidSocket;

    // A list rather than a vector: registerIfNamed() walks the connections
    // while holding a reference into them, and a reallocation there would be a
    // dangling one.
    std::list<Connection> connections_;
    ClientRegistry registry_;

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> refused_{0};
    std::atomic<uint64_t> closed_{0};
    std::atomic<uint64_t> identified_{0};
    std::atomic<size_t> connectionCount_{0};
    std::atomic<size_t> clientCount_{0};
    std::atomic<uint64_t> commandsSent_{0};
    std::atomic<uint64_t> startsSent_{0};

    // The one place another thread reaches into this one, and the only lock
    // here: a zone's pipeline thread fills it and the loop empties it.
    mutable std::mutex mailboxMutex_;
    std::deque<RoomCommand> mailbox_;

    // Last, so that it is stopped before anything it uses is destroyed.
    std::jthread thread_;
};

// ---------------------------------------------------------------------------

ControlServer::ControlServer(std::unique_ptr<Impl> impl, uint16_t port)
    : impl_(std::move(impl)), port_(port)
{
}

ControlServer::~ControlServer() = default;

Result<std::unique_ptr<ControlServer>, ControlServerError>
ControlServer::start(const IClock& clock, ControlServerSettings settings)
{
    if (settings.maxConnections == 0 || settings.maxConnections >= FD_SETSIZE)
    {
        return fail(ControlServerError::Unsupported,
                    "maxConnections must be between 1 and " + std::to_string(FD_SETSIZE - 1) +
                        " on this platform, which is what select() can watch");
    }

    auto impl = std::make_unique<Impl>(clock, settings);

    uint16_t port = 0;
    const Status<ControlServerError> bound = impl->bind(port);
    if (!bound)
        return std::unexpected(bound.error());

    impl->run();

    MULTIROOM_INFO("control listener on port {}", port);
    return std::unique_ptr<ControlServer>(new ControlServer(std::move(impl), port));
}

uint64_t ControlServer::acceptedCount() const noexcept
{
    return impl_->accepted();
}

uint64_t ControlServer::refusedCount() const noexcept
{
    return impl_->refused();
}

uint64_t ControlServer::closedCount() const noexcept
{
    return impl_->closed();
}

uint64_t ControlServer::identifiedCount() const noexcept
{
    return impl_->identified();
}

size_t ControlServer::connectionCount() const noexcept
{
    return impl_->connections();
}

size_t ControlServer::clientCount() const noexcept
{
    return impl_->clients();
}

uint64_t ControlServer::commandsSentCount() const noexcept
{
    return impl_->commandsSent();
}

uint64_t ControlServer::startsSentCount() const noexcept
{
    return impl_->startsSent();
}

bool ControlServer::send(const RoomCommand& command)
{
    return impl_->queueCommand(command);
}

} // namespace multiroom
