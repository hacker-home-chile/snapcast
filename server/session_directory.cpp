/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "session_directory.hpp"

#include "common/aixlog.hpp"

#include <algorithm>

static constexpr auto LOG_TAG = "SessionDir";


void SessionDirectory::add(const session_ptr& session)
{
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    sessions_.emplace_back(session);
    cleanup();
}


void SessionDirectory::remove(StreamSession* raw)
{
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [raw](const std::weak_ptr<StreamSession>& w)
    {
        auto s = w.lock();
        return s.get() == raw;
    }),
                    sessions_.end());
    cleanup();
}


SessionDirectory::session_ptr SessionDirectory::findByRawPointer(StreamSession* raw) const
{
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    for (const auto& w : sessions_)
    {
        if (auto s = w.lock())
            if (s.get() == raw)
                return s;
    }
    return nullptr;
}


SessionDirectory::session_ptr SessionDirectory::findByClientId(const std::string& clientId) const
{
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    for (const auto& w : sessions_)
    {
        if (auto s = w.lock())
            if (s->clientId == clientId)
                return s;
    }
    return nullptr;
}


size_t SessionDirectory::stopOthers(const std::string& clientId, StreamSession* keep)
{
    // Snapshot under lock; call stop() outside the iteration so the session's
    // stop() handler is free to call back into remove() without iterator
    // invalidation. The mutex is recursive, so we don't even need to drop it,
    // but iterating a snapshot keeps the invariant explicit.
    std::vector<session_ptr> targets;
    {
        std::lock_guard<std::recursive_mutex> mlock(mutex_);
        for (const auto& w : sessions_)
        {
            auto s = w.lock();
            if (!s || s.get() == keep)
                continue;
            if (s->clientId != clientId)
                continue;
            targets.push_back(std::move(s));
        }
    }
    for (const auto& s : targets)
    {
        LOG(INFO, LOG_TAG) << "stopping stale session for " << clientId << " superseded by new Hello\n";
        s->stop();
    }
    return targets.size();
}


void SessionDirectory::stopAll()
{
    auto targets = snapshot();
    for (const auto& s : targets)
        s->stop();
}


std::vector<SessionDirectory::session_ptr> SessionDirectory::snapshot() const
{
    std::vector<session_ptr> out;
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    out.reserve(sessions_.size());
    for (const auto& w : sessions_)
        if (auto s = w.lock())
            out.push_back(s);
    return out;
}


size_t SessionDirectory::cleanup()
{
    // Caller must already hold mutex_; cleanup() is only invoked from add() /
    // remove() which lock it themselves. We re-lock here too because it is
    // recursive and the public contract says "thread-safe".
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    auto new_end = std::remove_if(sessions_.begin(), sessions_.end(),
                                  [](const std::weak_ptr<StreamSession>& w) { return w.expired(); });
    auto count = static_cast<size_t>(std::distance(new_end, sessions_.end()));
    if (count > 0)
        sessions_.erase(new_end, sessions_.end());
    return count;
}


size_t SessionDirectory::size() const
{
    std::lock_guard<std::recursive_mutex> mlock(mutex_);
    return sessions_.size();
}
