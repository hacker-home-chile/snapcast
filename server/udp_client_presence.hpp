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

#include <string>


/// Read-only "is this client receiving audio over UDP?" lookup used by
/// StreamServer's TCP fan-out to skip clients that are already getting their
/// audio over the UDP transport. Pulled out into its own interface so the
/// fan-out can be unit-tested with a tiny fake instead of dragging in the
/// real UdpAudioServer (which owns a socket and a strand).
class UdpClientPresence
{
public:
    virtual ~UdpClientPresence() = default;

    /// True if @p client_id is currently registered for UDP audio delivery.
    virtual bool hasClient(const std::string& client_id) const = 0;
};


/// Returns true when @p clientId is currently receiving its audio over UDP
/// and therefore should be skipped in the TCP WireChunk fan-out. Nullsafe:
/// returns false when @p udp is nullptr (i.e. the server was built/run
/// without the UDP transport — stock TCP-only behavior).
inline bool isUdpRegistered(const UdpClientPresence* udp, const std::string& clientId)
{
    return udp != nullptr && udp->hasClient(clientId);
}
