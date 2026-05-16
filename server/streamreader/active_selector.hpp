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

#include <memory>
#include <mutex>
#include <utility>


/// Thread-safe holder for the "currently active" element out of a wider set
/// (e.g. MetaStream's active child PcmStream). Pulled out so the
/// snapshot-then-apply pattern can be unit-tested in isolation and shared
/// between sites that need it.
///
/// The contract the pattern enforces:
///   1. `get()` returns a shared_ptr snapshot, so the caller can keep using
///      the value after a concurrent `set()` swaps it out — the underlying
///      object stays alive via the refcount.
///   2. `applyToActive(fn)` invokes @p fn on the snapshot OUTSIDE the
///      internal lock, so @p fn is free to take its own locks / call back
///      into other methods on this selector without deadlocking.
///   3. `set(nullptr)` followed by `applyToActive` is a no-op (returns
///      false), not a crash.
///
/// This is the pattern MetaStream::setVolume / setMute etc. should use —
/// today those methods read `active_stream_` under one mutex while
/// `switch_stream` updates it under a different mutex.
template <typename T>
class ActiveSelector
{
public:
    /// Replace the currently-active element (nullptr is allowed).
    void set(std::shared_ptr<T> value)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        active_ = std::move(value);
    }

    /// @return a snapshot of the active element (refcount bumped). Caller
    /// may safely use the returned shared_ptr after a concurrent set().
    std::shared_ptr<T> get() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return active_;
    }

    /// Apply @p fn to the currently-active element, if any.
    /// @return true if fn was invoked, false if active was nullptr.
    /// fn runs WITHOUT the internal lock held, so it can re-enter this
    /// selector safely.
    template <typename F>
    bool applyToActive(F&& fn)
    {
        auto snapshot = get();
        if (!snapshot)
            return false;
        std::forward<F>(fn)(*snapshot);
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<T> active_;
};
