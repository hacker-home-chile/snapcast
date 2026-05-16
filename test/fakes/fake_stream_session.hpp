/***
    udp-music: test fake for StreamSession.

    Subclasses the abstract StreamSession base so unit tests can drive
    session lifecycle (start/stop/onDisconnect) without sockets, SSL, or
    the StreamSessionTcp/Ws machinery.

    Behavior:
      - start()/stop()/sendAsync() are no-ops that just bump counters.
      - stop() does NOT auto-trigger onDisconnect — tests that want to
        observe the post-stop cleanup should call simulateDisconnect()
        explicitly. This keeps tests focused on the call sequence under
        test rather than the (production-only) async socket teardown.
***/

#pragma once

#include "server/stream_session.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <string>
#include <utility>


class FakeStreamSession : public StreamSession
{
public:
    FakeStreamSession(const boost::asio::any_io_executor& executor,
                      const ServerSettings& settings,
                      StreamMessageReceiver* receiver,
                      std::string id)
        : StreamSession(executor, settings, receiver)
    {
        clientId = std::move(id);
    }

    std::string getIP() override
    {
        return "127.0.0.1";
    }

    void start() override
    {
        started_.fetch_add(1, std::memory_order_relaxed);
    }

    void stop() override
    {
        stopped_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Drive the disconnect callback as if the underlying socket had closed.
    /// In production this is invoked by the transport layer once stop() has
    /// torn the socket down; tests call it explicitly to control ordering.
    void simulateDisconnect()
    {
        if (messageReceiver_ != nullptr)
            messageReceiver_->onDisconnect(this);
    }

    int started() const
    {
        return started_.load(std::memory_order_relaxed);
    }
    int stopped() const
    {
        return stopped_.load(std::memory_order_relaxed);
    }
    int sendCalls() const
    {
        return send_calls_.load(std::memory_order_relaxed);
    }

protected:
    void sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler) override
    {
        send_calls_.fetch_add(1, std::memory_order_relaxed);
        // Invoke the handler synchronously so StreamSession::sendNext drains
        // its internal queue — otherwise messages_ piles up across tests.
        if (handler)
            handler({}, buffer.message().data.size());
    }

private:
    std::atomic<int> started_{0};
    std::atomic<int> stopped_{0};
    std::atomic<int> send_calls_{0};
};
