/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#ifndef __RAKNET_SOCKET_2_H
#define __RAKNET_SOCKET_2_H

#include "types.h"
#include "MTUSize.h"
#include "LocklessTypes.h"
#include "thread.h"
#include "DS_ThreadsafeAllocatingQueue.h"
#include "Export.h"

#include <atomic>

// Batched datagram I/O (recvmmsg / sendmmsg) is guarded by a plain
// `#if defined(__linux__)` wherever it appears. There is no macro and no build
// option for it: the syscalls exist on Linux and nowhere else MafiaNet targets,
// so it is simply always on there. Every other platform compiles the portable
// fallbacks instead -- RakNetSocket2::SendBatch loops Send(), and the scalar
// recvfrom loop drains the socket.
//
// Delivery semantics are identical either way: the same datagrams arrive, in
// the same order, with the same reliability, and nothing differs that
// application code can observe. Two internals DO differ -- the number of
// system calls, and how SendBatch reports a transient send failure: the
// batched override classifies errno and can return 0 ("nothing sent, retry
// later"), whereas the portable loop cannot read errno through Send() and
// reports every failure as permanent. See the SendBatch contract below.
// Either way the datagrams are dropped and the reliability layer resends.

// For CFSocket
// https://developer.apple.com/library/mac/#documentation/CoreFOundation/Reference/CFSocketRef/Reference/reference.html
// Reason: http://sourceforge.net/p/open-dis/discussion/683284/thread/0929d6a0
#if defined(__APPLE__)
#import <CoreFoundation/CoreFoundation.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

namespace MafiaNet
{

class RakNetSocket2;
struct RNS2_BerkleyBindParameters;
struct RNS2_SendParameters;
#ifdef WIN32
typedef SOCKET RNS2Socket;
#else
// #low determine whether we cannot use SOCKET on all platforms...
typedef int RNS2Socket;
#endif

enum RNS2BindResult
{
	BR_SUCCESS,
	BR_REQUIRES_RAKNET_SUPPORT_IPV6_DEFINE,
	BR_FAILED_TO_BIND_SOCKET,
	BR_FAILED_SEND_TEST,
};

typedef int RNS2SendResult;

/// The last socket error the calling thread raised, as WSAGetLastError() on
/// Windows and errno everywhere else.
///
/// Send() collapses every failure to a single negative RNS2SendResult (sendto's
/// own return value), which loses the reason. Call this immediately after a
/// failing socket call on the same thread to recover it -- any intervening
/// socket call may overwrite it.
int RNS2_GetLastSocketError(void);

/// Does \a err mean "that datagram was larger than the outgoing interface's
/// MTU"? WSAEMSGSIZE on Windows, EMSGSIZE elsewhere.
///
/// This is a local refusal, not a network event: the stack rejected the send
/// outright, so every retry at that size fails identically and instantly. The
/// connection handshake uses it to abandon an MTU rung the moment the local
/// interface refuses it -- a VPN or PPPoE adapter with a small MTU -- instead of
/// spending that rung's whole attempt budget on sends that never leave the
/// machine.
///
/// Split out as a free function taking a plain int so the classification is
/// unit testable without provoking a real socket failure.
bool RNS2_IsDatagramTooLargeError(int err);

enum RNS2Type
{
	RNS2T_WINDOWS,
	RNS2T_LINUX
};

struct RNS2_SendParameters
{
	RNS2_SendParameters() {ttl=0;}
	char *data;
	int length;
	SystemAddress systemAddress;
	int ttl;
};

struct RNS2RecvStruct
{

	char data[MAXIMUM_MTU_SIZE];

	int bytesRead;
	SystemAddress systemAddress;
	MafiaNet::TimeUS timeRead;
	RakNetSocket2 *socket;
};

class RakNetSocket2Allocator
{
public:
	static RakNetSocket2* AllocRNS2(void);
	static void DeallocRNS2(RakNetSocket2 *s);
};

class RAK_DLL_EXPORT RNS2EventHandler
{
public:
	RNS2EventHandler() {}
	virtual ~RNS2EventHandler() {}

	//		bufferedPackets.Push(recvFromStruct);
	//		quitAndDataEvents.SetEvent();
	virtual void OnRNS2Recv(RNS2RecvStruct *recvStruct)=0;
	virtual void DeallocRNS2RecvStruct(RNS2RecvStruct *s, const char *file, unsigned int line)=0;
	virtual RNS2RecvStruct *AllocRNS2RecvStruct(const char *file, unsigned int line)=0;

	// recvFromStruct=bufferedPackets.Allocate( _FILE_AND_LINE_ );
	// 	DataStructures::ThreadsafeAllocatingQueue<RNS2RecvStruct> bufferedPackets;
};

class RakNetSocket2
{
public:
	RakNetSocket2();
	virtual ~RakNetSocket2();

	// In order for the handler to trigger, some platforms must call PollRecvFrom, some platforms this create an internal thread.
	void SetRecvEventHandler(RNS2EventHandler *_eventHandler);
	virtual RNS2SendResult Send( RNS2_SendParameters *sendParameters, const char *file, unsigned int line )=0;
	// Batched send. The base implementation simply loops Send() so every socket
	// type has a working default; RNS2_Linux overrides it with sendmmsg where the
	// syscall exists (Linux).
	// Returns a datagram COUNT, NOT the byte total Send() returns:
	//   > 0        datagrams accepted (may be < count -- see the drop rule below)
	//   0          nothing went out, but nothing is permanently wrong: a transient
	//              socket-wide condition (send buffer full, interrupted call).
	//              Retry on a later tick.
	//   < 0        nothing went out and at least one datagram failed permanently;
	//              the value is the first such error, mirroring sendmmsg(2)'s
	//              "an error is returned only if no datagrams could be sent".
	// A datagram that fails on its own (bad destination, oversized) is dropped and
	// the rest of the batch is still sent, so the count may be short without an
	// error being reported.
	// Both implementations agree on the sign convention above, but NOT on how they
	// detect the transient case: the sendmmsg override classifies errno
	// (ClassifySendmmsgErrno) and so can return 0, whereas the base loop cannot
	// portably read errno through Send() and reports every failure as permanent.
	// A caller must therefore treat "0" and "negative" alike as "these datagrams
	// did not go out"; only the diagnostics differ. See MmsgBatchTests.
	// RNS2_SendParameters::ttl is honoured either way: sendmmsg has no per-message
	// TTL, so the override defers a batch carrying one to this base loop.
	virtual RNS2SendResult SendBatch( RNS2_SendParameters *sends, unsigned count, const char *file, unsigned int line );
	RNS2Type GetSocketType(void) const;
	void SetSocketType(RNS2Type t);
	bool IsBerkleySocket(void) const;
	SystemAddress GetBoundAddress(void) const;
	unsigned int GetUserConnectionSocketIndex(void) const;
	void SetUserConnectionSocketIndex(unsigned int i);
	RNS2EventHandler * GetEventHandler(void) const;

	// ----------- STATICS ------------
	static void GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
	static void DomainNameToIP( const char *domainName, char ip[65] );

protected:
	RNS2EventHandler *eventHandler;
	RNS2Type socketType;
	SystemAddress boundAddress;
	unsigned int userConnectionSocketIndex;
};

struct RNS2_BerkleyBindParameters
{
	// Input parameters
	unsigned short port;
	char *hostAddress;
	unsigned short addressFamily; // AF_INET or AF_INET6
	int type; // SOCK_DGRAM
	int protocol; // 0
	bool nonBlockingSocket;
	int setBroadcast;
	int setIPHdrIncl;
	int doNotFragment;
	int pollingThreadPriority;
	RNS2EventHandler *eventHandler;
	unsigned short remotePortRakNetWasStartedOn_PS3_PS4_PSP2;
};

// Berkeley sockets interface - base class for all platforms
class IRNS2_Berkley : public RakNetSocket2
{
public:
	// ----------- STATICS ------------
	// For addressFamily, use AF_INET
	// For type, use SOCK_DGRAM
	static bool IsPortInUse(unsigned short port, const char *hostAddress, unsigned short addressFamily, int type );

	// ----------- MEMBERS ------------
	virtual RNS2BindResult Bind( RNS2_BerkleyBindParameters *bindParameters, const char *file, unsigned int line )=0;
};
// Common Berkeley socket implementation for Windows and Linux
class RNS2_Berkley : public IRNS2_Berkley
{
public:
	RNS2_Berkley();
	virtual ~RNS2_Berkley();
	int CreateRecvPollingThread(int threadPriority);
	void SignalStopRecvPollingThread(void);
	void BlockOnStopRecvPollingThread(void);
	const RNS2_BerkleyBindParameters *GetBindings(void) const;
	RNS2Socket GetSocket(void) const;
	void SetDoNotFragment( int opt );

protected:
	// Used by other classes
	RNS2BindResult BindShared( RNS2_BerkleyBindParameters *bindParameters, const char *file, unsigned int line );
	RNS2BindResult BindSharedIPV4( RNS2_BerkleyBindParameters *bindParameters, const char *file, unsigned int line );
	RNS2BindResult BindSharedIPV4And6( RNS2_BerkleyBindParameters *bindParameters, const char *file, unsigned int line );
	
	static void GetSystemAddressIPV4 ( RNS2Socket rns2Socket, SystemAddress *systemAddressOut );
	static void GetSystemAddressIPV4And6 ( RNS2Socket rns2Socket, SystemAddress *systemAddressOut );

	// Internal
	void SetNonBlockingSocket(unsigned long nonblocking);
	void SetSocketOptions(void);
	void SetBroadcastSocket(int broadcast);
	void SetIPHdrIncl(int ipHdrIncl);
	void RecvFromBlocking(RNS2RecvStruct *recvFromStruct);
	void RecvFromBlockingIPV4(RNS2RecvStruct *recvFromStruct);
	void RecvFromBlockingIPV4And6(RNS2RecvStruct *recvFromStruct);

	RNS2Socket rns2Socket;
	RNS2_BerkleyBindParameters binding;

	unsigned RecvFromLoopInt(void);
	// Batched drain of the recv socket via recvmmsg (Linux).
	// Declared unconditionally; only ever called from RecvFromLoopInt under the
	// same guard, and only defined on that platform.
	void RecvFromBatchedLoop(void);
	MafiaNet::LocklessUint32_t isRecvFromLoopThreadActive;
	std::atomic<bool> endThreads;
	// The recv polling thread is joinable so teardown can wait for it to fully
	// exit before the socket (and the RakPeer it calls back into) are freed.
	// A detached thread with a bounded wait allowed a leaked thread to keep
	// dereferencing both after Shutdown (issue #7).
	RakThread::ThreadHandle recvThread;
	bool recvThreadJoinable;
	// Constructor not called!

#if defined(__APPLE__)
	// http://sourceforge.net/p/open-dis/discussion/683284/thread/0929d6a0
	CFSocketRef             _cfSocket;
#endif

	static RAK_THREAD_DECLARATION(RecvFromLoop);
};

#if defined(_WIN32) || defined(__GNUC__)  || defined(__GCCXML__) || defined(__S3E__)
class RNS2_Windows_Linux_360
{
public:
protected:
	static RNS2SendResult Send_Windows_Linux_360NoVDP( RNS2Socket rns2Socket, RNS2_SendParameters *sendParameters, const char *file, unsigned int line );
};
#endif

#if   defined(_WIN32)

class RAK_DLL_EXPORT SocketLayerOverride
{
public:
	SocketLayerOverride() {}
	virtual ~SocketLayerOverride() {}

	/// Called when SendTo would otherwise occur.
	virtual int RakNetSendTo( const char *data, int length, const SystemAddress &systemAddress )=0;

	/// Called when RecvFrom would otherwise occur. Return number of bytes read. Write data into dataOut
	// Return -1 to use RakNet's normal recvfrom, 0 to abort RakNet's normal recvfrom, and positive to return data
	virtual int RakNetRecvFrom( char dataOut[ MAXIMUM_MTU_SIZE ], SystemAddress *senderOut, bool calledFromMainThread )=0;
};

class RNS2_Windows : public RNS2_Berkley, public RNS2_Windows_Linux_360
{
public:
	RNS2_Windows();
	virtual ~RNS2_Windows();
	RNS2BindResult Bind( RNS2_BerkleyBindParameters *bindParameters, const char *file, unsigned int line );
	RNS2SendResult Send( RNS2_SendParameters *sendParameters, const char *file, unsigned int line );
	void SetSocketLayerOverride(SocketLayerOverride *_slo);
	SocketLayerOverride* GetSocketLayerOverride(void);
	// ----------- STATICS ------------
	static void GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
protected:
	static void GetMyIPIPV4( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
	static void GetMyIPIPV4And6( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
	SocketLayerOverride *slo;
};

#else
class RNS2_Linux : public RNS2_Berkley, public RNS2_Windows_Linux_360
{
public:
	RNS2BindResult Bind( RNS2_BerkleyBindParameters *bindParameters, const char *file, unsigned int line );
	RNS2SendResult Send( RNS2_SendParameters *sendParameters, const char *file, unsigned int line );
	// Guarded because RNS2_Linux is the non-Windows socket class: macOS and the
	// BSDs compile it too, and sendmmsg/mmsghdr do not exist there. Those
	// platforms inherit the portable base SendBatch (a Send() loop).
#if defined(__linux__)
	RNS2SendResult SendBatch( RNS2_SendParameters *sends, unsigned count, const char *file, unsigned int line );
#endif

	// ----------- STATICS ------------
	static void GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
protected:
	static void GetMyIPIPV4( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
	static void GetMyIPIPV4And6( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
};

#endif // Linux

} // namespace MafiaNet

#endif // __RAKNET_SOCKET_2_H
