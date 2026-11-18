/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#ifndef __RAKNET_DEFINES_H
#define __RAKNET_DEFINES_H

#ifdef _RETAIL
// retail builds imply release configurations
#define _RELEASE
#endif

// If you want to change these defines, put them in RakNetDefinesOverrides so your changes are not lost when updating RakNet
// The user should not edit this file
#include "mafianet/define_overrides.h"

/// Define __GET_TIME_64BIT to have MafiaNet::TimeMS use a 64, rather than 32 bit value.  A 32 bit value will overflow after about 5 weeks.
/// However, this doubles the bandwidth use for sending times, so don't do it unless you have a reason to.
/// This must be the same on all systems, or they won't connect
#ifndef __GET_TIME_64BIT
#define __GET_TIME_64BIT 1
#endif

/// Define RAKNET_COMPATIBILITY to enable API compatibility with RakNet.
/// This allows you to keep existing code which was compatible with RakNet 4.082 unmodified and
/// use MafiaNet as an in-place replacement for the RakNet library without having to modify any
/// of your code.
// #define RAKNET_COMPATIBILITY

/// Define __BITSTREAM_NATIVE_END to NOT support endian swapping in the BitStream class.  This is faster and is what you should use
/// unless you actually plan to have different endianness systems connect to each other
/// Enabled by default.
// #define __BITSTREAM_NATIVE_END

/// Threshold at which to do a malloc / free rather than pushing data onto a fixed stack for the bitstream class
/// Arbitrary size, just picking something likely to be larger than most packets
#ifndef BITSTREAM_STACK_ALLOCATION_SIZE
#define BITSTREAM_STACK_ALLOCATION_SIZE 256
#endif

// Redefine if you want to disable or change the target for debug RAKNET_DEBUG_PRINTF
#ifndef RAKNET_DEBUG_PRINTF
#define RAKNET_DEBUG_PRINTF printf
#endif

#ifndef RAKNET_DEBUG_TPRINTF
#define RAKNET_DEBUG_TPRINTF _tprintf
#endif

// Maximum number of local IP addresses supported
#ifndef MAXIMUM_NUMBER_OF_INTERNAL_IDS
#define MAXIMUM_NUMBER_OF_INTERNAL_IDS 10
#endif

// Maximum size in bytes of a session-handshake payload (see RakPeerInterface::SetSessionConfig).
// Enforced on both send and receive; an oversized inbound payload is a protocol violation and
// closes the connection. The payload rides the reliability layer after MTU negotiation, so it is
// split into ordinary reliable datagrams rather than sent as one oversized burst.
#ifndef MAXIMUM_SESSION_CONFIG_SIZE
#define MAXIMUM_SESSION_CONFIG_SIZE 65536
#endif

// Bounds on the application messages the connecting side holds while it finishes the session
// handshake (only on connections this peer initiated: a peer that connects to us can never make us
// hold anything). The accepting side reports the connection first and may send at once, so its first
// messages can arrive before its session payload (ID_SESSION_CONFIG) when that datagram is lost or
// overtaken; they are held and delivered right after ID_CONNECTION_REQUEST_ACCEPTED. That window lasts
// about one retransmission timeout of a brand-new connection, and ID_SESSION_CONFIG is resent at
// Immediate priority ahead of any application data. A remote that exceeds either bound fails the
// connection attempt (ID_CONNECTION_ATTEMPT_FAILED) and the held messages are freed.
//
// The byte bound is 64 session payloads. The message count bounds the per-message bookkeeping (about
// 50 bytes each, so 3 MB at most) and is set high enough that a server dumping tens of thousands of
// small messages outside channel 0 the moment it reports the connection still fits; channel-0 ordered
// messages queue in the reliability layer instead and never count.
#ifndef MAXIMUM_HELD_SESSION_MESSAGES
#define MAXIMUM_HELD_SESSION_MESSAGES 65536
#endif
#ifndef MAXIMUM_HELD_SESSION_BYTES
#define MAXIMUM_HELD_SESSION_BYTES (64u * MAXIMUM_SESSION_CONFIG_SIZE)
#endif

#ifndef RakAssert
#if defined(_DEBUG)
#define RakAssert(x) assert(x);
#else
#define RakAssert(x)
#endif
#endif

#if !defined(_DEBUG)
#define SLNET_VERIFY(x) ((void) (x))
#else
#define SLNET_VERIFY(x) RakAssert(x)
#endif

/// This controls the amount of memory used per connection.
/// This many datagrams are tracked by datagramNumber. If more than this many datagrams are sent, then an ack for an older datagram would be ignored
/// This results in an unnecessary resend in that case
#ifndef DATAGRAM_MESSAGE_ID_ARRAY_LENGTH
#define DATAGRAM_MESSAGE_ID_ARRAY_LENGTH 512
#endif

/// This is the maximum number of reliable user messages that can be on the wire at a time
/// If this is too low, then high ping connections with a large throughput will be underutilized
/// This will be evident because RakNetStatistics::messagesInSend buffer will increase over time, yet at the same time the outgoing bandwidth per second is less than your connection supports
#ifndef RESEND_BUFFER_ARRAY_LENGTH
#define RESEND_BUFFER_ARRAY_LENGTH 512
#define RESEND_BUFFER_ARRAY_MASK 511
#endif

#ifndef GET_TIME_SPIKE_LIMIT
/// Workaround for http://support.microsoft.com/kb/274323
/// If two calls between MafiaNet::GetTime() happen farther apart than this time in microseconds, this delta will be returned instead
/// Note: This will cause ID_TIMESTAMP to be temporarily inaccurate if you set a breakpoint that pauses the UpdateNetworkLoop() thread in RakPeer
/// Define in definesoverrides.h to enable (non-zero) or disable (0)
#define GET_TIME_SPIKE_LIMIT 0
#endif

// Use sliding window congestion control instead of ping based congestion control
#ifndef USE_SLIDING_WINDOW_CONGESTION_CONTROL
#define USE_SLIDING_WINDOW_CONGESTION_CONTROL 1
#endif

// When a large message is arriving, preallocate the memory for the entire block
// This results in large messages not taking up time to reassembly with memcpy, but is vulnerable to attackers causing the host to run out of memory
#ifndef PREALLOCATE_LARGE_MESSAGES
#define PREALLOCATE_LARGE_MESSAGES 0
#endif

#if defined(RAKNET_SUPPORT_IPV6) && RAKNET_SUPPORT_IPV6 == 0
#error "RAKNET_SUPPORT_IPV6 was removed: IPv6 is always compiled in"
#endif

#ifndef RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS
#define RPC_GLOBAL_REGISTRATION_MAX_FUNCTIONS 48
#endif

#ifndef RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH
#define RPC_GLOBAL_REGISTRATION_MAX_FUNCTION_NAME_LENGTH 48
#endif

// If defined to 1, the user is responsible for calling RakPeer::RunUpdateCycle and RakPeer::RunRecvfrom
#ifndef RAKPEER_USER_THREADED
#define RAKPEER_USER_THREADED 0
#endif

//#define USE_THREADED_SEND

// @since 0.1.1: added
// Controls the maximum retrievable filesize for incoming files using FileListTransfer.
// The configured limit only applies for files which are transferred incrementally (which basically applies to any larger file).
// Note that this also impacts the upper limit for memory allocations. It's suggested to redefine the value to a reasonable smaller size in the defineoverrides.h header file.
// For backwards compatibility with RakNet, the default is set to 4 GiB-1.
// #low - consider introducing GiB/MiB/KiB-functions and then define as GiB(4)?
#ifndef SLNET_MAX_RETRIEVABLE_FILESIZE
#define SLNET_MAX_RETRIEVABLE_FILESIZE (0xFFFFFFFF)
#endif

// Short-hand alias for the MafiaNet namespace.
// note: we use a preprocessor macro rather than a namespace alias to ensure ABI compatibility with shared
// libraries/DLLs. With a namespace alias the names in the DLLs would still point to the actual namespace
// (MafiaNet) rather than the alias namespace, so the macro simply rewrites MNet to the real MafiaNet namespace.
#define MNet MafiaNet

#endif // __RAKNET_DEFINES_H
