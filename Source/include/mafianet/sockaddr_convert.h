/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file sockaddr_convert.h
/// \brief Conversions between raw sockaddrs and SystemAddress at the socket
/// boundary.
///
/// A dual-stack socket (AF_INET6 with IPV6_V6ONLY=0, opened for
/// SocketDescriptor::socketFamily == AF_UNSPEC) sees IPv4 peers as IPv4-mapped
/// IPv6 addresses (::ffff:a.b.c.d) and must be addressed that way. These
/// helpers keep that a property of the socket: everything above them only
/// ever sees the canonical AF_INET form, so an IPv4 peer compares equal
/// however it reached us. Both the scalar recvfrom/sendto paths and the Linux
/// recvmmsg/sendmmsg paths go through them.
///
/// Portable and compiled everywhere, so they are unit tested on every platform.

#ifndef __MAFIANET_SOCKADDR_CONVERT_H
#define __MAFIANET_SOCKADDR_CONVERT_H

#include "mafianet/export.h"
#include "mafianet/types.h"

namespace MafiaNet
{

/// Convert a raw sockaddr (IPv4 or IPv6) into a SystemAddress, reproducing the
/// byte-order handling of the scalar recvfrom path in RNS2_Berkley. \a out
/// receives the address; its port is stored in network order in the address
/// union and mirrored (host order) into debugPort. An IPv4-mapped IPv6 source
/// (::ffff:a.b.c.d, what a dual-stack socket reports for an IPv4 peer) is
/// returned as the plain AF_INET address.
///
/// Returns false for an address family other than AF_INET or AF_INET6 (such as
/// AF_UNSPEC), in which case \a out is set to UNASSIGNED_SYSTEM_ADDRESS rather
/// than left holding whatever the recycled recv struct contained. Callers must
/// not dispatch a datagram whose address failed to decode.
RAK_DLL_EXPORT bool SockaddrToSystemAddress(const sockaddr_storage &from, SystemAddress *out);

/// Convert \a to into the sockaddr to pass to sendto/sendmmsg on a socket.
/// Returns the sockaddr length to pass along with \a out, or 0 when the socket
/// cannot reach \a to at all (the caller must not send).
///
/// When \a socketIsDualStack, an IPv4 target becomes ::ffff:a.b.c.d (length
/// sizeof(sockaddr_in6)) and an IPv6 target is passed through.
///
/// Otherwise \a socketFamily, the socket's own family, filters targets of the
/// other family: an IPv6 target on an AF_INET socket, or an IPv4 target on an
/// IPv6-only socket, returns 0. AF_UNSPEC means the family is unknown and
/// nothing is filtered. A target that is neither AF_INET nor AF_INET6 always
/// returns 0.
RAK_DLL_EXPORT socklen_t SystemAddressToSockaddr(const SystemAddress &to, bool socketIsDualStack, sockaddr_storage *out, int socketFamily);

/// A setsockopt level and option name that sets the TTL (IPv4) or hop limit
/// (IPv6) of the unicast datagrams a socket sends.
struct HopLimitOption
{
	int level;
	int name;
};

/// The most options HopLimitOptionsFor ever returns.
static const int MAX_HOP_LIMIT_OPTIONS = 2;

/// The options to try, in order, to set the TTL of a datagram to a target of
/// \a targetFamily (AF_INET or AF_INET6) on a socket of \a socketFamily, or
/// on a dual-stack socket when \a socketIsDualStack (its family argument is
/// then ignored). Use the first one whose getsockopt and setsockopt both
/// succeed. Writes them to \a out and returns how many, 0 when the socket
/// cannot reach the target at all. AF_UNSPEC as \a socketFamily means the
/// socket's family is unknown: the target's decides.
///
/// - IPv4 socket: IPPROTO_IP / IP_TTL.
/// - IPv6 target (IPv6-only or dual-stack socket): IPPROTO_IPV6 / IPV6_UNICAST_HOPS.
/// - IPv4 target on a dual-stack socket: IPPROTO_IP / IP_TTL first (Linux,
///   and Windows dual-mode sockets, apply it to the socket's IPv4 traffic and
///   ignore the hop limit for it -- measured on Linux), then IPPROTO_IPV6 /
///   IPV6_UNICAST_HOPS (macOS rejects IP_TTL on an AF_INET6 socket with
///   EINVAL and applies the hop limit to IPv4-mapped datagrams -- measured on macOS).
RAK_DLL_EXPORT int HopLimitOptionsFor(int socketFamily, bool socketIsDualStack, int targetFamily, HopLimitOption out[MAX_HOP_LIMIT_OPTIONS]);

/// The value to put back after a datagram was sent with a temporary TTL, given
/// the value \a saved that getsockopt returned before. Normally \a saved
/// itself. A negative \a saved (IPV6_UNICAST_HOPS left at -1, "the kernel
/// default", as macOS reports it) is put back as is on an IPv6-only socket,
/// but not on a dual-stack one: on macOS, explicitly setting -1 makes the
/// socket send its IPv4-mapped datagrams with TTL 255 from then on. There the
/// IPv4 default \a ipv4DefaultTtl is restored instead (64 when it is not
/// positive, the Linux and macOS default).
RAK_DLL_EXPORT int HopLimitToRestore(int saved, bool socketIsDualStack, int ipv4DefaultTtl);

} // namespace MafiaNet

#endif // __MAFIANET_SOCKADDR_CONVERT_H
