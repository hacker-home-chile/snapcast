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

#pragma once

// local headers
#include "stream_session.hpp"

// standard headers
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>


/// Holds the live StreamSession registry. Extracted from StreamServer so the
/// add/remove/lookup/stop-stale logic can be exercised without dragging in the
/// TCP accept path, UDP fan-out, or the global Config singleton.
///
/// Thread-safety: every public method takes the internal recursive mutex.
/// Recursive because a session's stop() typically calls back into remove()
/// from within an iteration here.
class SessionDirectory
{
public:
    using session_ptr = std::shared_ptr<StreamSession>;

    /// Store @p session as a weak ref; drop any expired entries.
    void add(const session_ptr& session);

    /// Remove the entry whose raw pointer matches @p raw, if any.
    void remove(StreamSession* raw);

    /// First live session whose raw pointer matches @p raw, else nullptr.
    session_ptr findByRawPointer(StreamSession* raw) const;

    /// First live session whose clientId matches, else nullptr.
    session_ptr findByClientId(const std::string& clientId) const;

    /// Stop every live session with @p clientId except @p keep.
    /// The session's own stop() is expected to drive onDisconnect → remove().
    /// @return number of stop() calls issued.
    size_t stopOthers(const std::string& clientId, StreamSession* keep);

    /// Stop every live session.
    void stopAll();

    /// Snapshot of live sessions for lock-free iteration by callers.
    std::vector<session_ptr> snapshot() const;

    /// Drop expired weak refs. Called automatically by add()/remove().
    /// @return number of expired entries removed.
    size_t cleanup();

    /// Number of stored weak refs (live or not).
    size_t size() const;

private:
    mutable std::recursive_mutex mutex_;
    std::vector<std::weak_ptr<StreamSession>> sessions_;
};
