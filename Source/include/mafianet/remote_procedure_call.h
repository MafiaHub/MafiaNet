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

/// \file
/// \brief Remote procedure call, supporting C functions only. No external dependencies required.
///


#pragma once

#include "mafianet/native_feature_includes.h"
#include "mafianet/packet_priority.h"
#include "mafianet/types.h"
#include "mafianet/bit_stream.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/// \defgroup RPC_GROUP RPC
/// \brief Remote procedure calls, without external dependencies.

namespace MafiaNet
{
/// Forward declarations
class RakPeerInterface;
class TCPInterface;

/// \brief Error codes returned by a remote system as to why an RPC function call cannot execute
/// \details Error code follows packet ID ID_RPC_REMOTE_ERROR, that is packet->data[1]<BR>
/// Name of the function will be appended starting at packet->data[2]
/// \ingroup RPC_GROUP
enum RPCErrorCodes
{
	/// Named function was not registered with RegisterFunction(). Check your spelling.
	RPC_ERROR_FUNCTION_NOT_REGISTERED,
};

/// \brief Instantiate this class globally if you want to register a function with RPC at the global space
class RAK_DLL_EXPORT RPCGlobalRegistration
{
public:
	/// \brief Queue a call to RPC::RegisterFunction() globally. Endpoints apply pending global registrations at RPC setup.
	/// \param[in] context Opaque user pointer passed back to the handler on every invocation. Used to route the call to an object instance without a global.
	RPCGlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context);

	/// \brief Queue a call to RPC::RegisterSlot() globally. Endpoints apply pending global registrations at RPC setup.
	/// \param[in] context Opaque user pointer passed back to the handler on every invocation.
	RPCGlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context, int callPriority);

	/// \brief Queue a call to RPC::RegisterBlockingFunction() globally. Endpoints apply pending global registrations at RPC setup.
	/// \param[in] context Opaque user pointer passed back to the handler on every invocation.
	RPCGlobalRegistration(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context), void *context);

	/// \brief Queue a call to RPC::RegisterLocalCallback() globally. Endpoints apply pending global registrations at RPC setup.
	RPCGlobalRegistration(const char *uniqueID, MessageID messageId);
};

/// \brief Named remote calls and signals owned by a networking endpoint.
/// Callers serialize parameters into a BitStream and register free functions as handlers.
/// \ingroup RPC_GROUP
class RAK_DLL_EXPORT RPC final
{
public:
	~RPC();

	// Owns its slots; not copyable.
	RPC(const RPC &) = delete;
	RPC &operator=(const RPC &) = delete;

	/// \deprecated Use RegisterSlot
	/// \brief Register a function pointer to be callable from a remote system
	/// \details The hash of the function name will be stored as an association with the function pointer
	/// When a call is made to call this function from the \a Call() or CallLoopback() function, the function pointer will be invoked with the passed bitStream to Call() and the actual Packet that RakNet got.
	/// \sa RegisterPacketCallback()
	/// \param[in] uniqueID Identifier to be associated with \a functionPointer. If this identifier is already in use, the call will return false.
	/// \param[in] functionPointer C function pointer to be called
	/// \param[in] context Opaque user pointer passed back to \a functionPointer on every invocation. Lets the handler recover its owning object instance without a file-static global. The pointer is not owned by RPC; keep it valid until the function is unregistered.
	/// \return True if the hash of uniqueID is not in use, false otherwise.
	bool RegisterFunction(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context);

	/// Register a slot, which is a function pointer to one or more implementations that supports this function signature
	/// When a signal occurs, all slots with the same identifier are called.
	/// \param[in] sharedIdentifier A string to identify the slot. Recommended to be the same as the name of the function.
	/// \param[in] functionPtr Callback to invoke, with the supplied context.
	/// \param[in] context Opaque user pointer passed back to \a functionPointer on every invocation. Each registration carries its own context, so the same handler may be registered under one identifier for several object instances. Not owned by RPC; keep it valid until the slot is unregistered.
	/// \param[in] callPriority Slots are called by order of the highest callPriority first. For slots with the same priority, they are called in the order they are registered
	void RegisterSlot(const char *sharedIdentifier, void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *context, int callPriority);

	/// \brief Same as \a RegisterFunction, but is called with CallBlocking() instead of Call() and returns a value to the caller
	/// \param[in] context Opaque user pointer passed back to \a functionPointer on every invocation. Not owned by RPC; keep it valid until the function is unregistered.
	bool RegisterBlockingFunction(const char *uniqueID, void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context), void *context);

	/// \deprecated Use RegisterSlot and invoke on self only when the packet you want arrives
	/// When a RakNet Packet with the specified identifier is returned, execute CallLoopback() on a function previously registered with RegisterFunction()
	/// For example, you could call "OnClosedConnection" whenever you get ID_DISCONNECTION_NOTIFICATION or ID_CONNECTION_LOST
	/// \param[in] uniqueID Identifier passed to RegisterFunction()
	/// \param[in] messageId What RakNet packet ID to call on, for example ID_DISCONNECTION_NOTIFICATION or ID_CONNECTION_LOST
	void RegisterLocalCallback(const char *uniqueID, MessageID messageId);

	/// \brief Unregister a function pointer previously registered with RegisterFunction()
	/// \param[in] Identifier originally passed to RegisterFunction()
	/// \return True if the hash of uniqueID was in use, and hence removed. false otherwise.
	bool UnregisterFunction(const char *uniqueID);

	/// \brief Same as UnregisterFunction, except for a blocking function
	bool UnregisterBlockingFunction(const char *uniqueID);

	/// Remove the association created with RegisterPacketCallback()
	/// \param[in] uniqueID Identifier passed as uniqueID to RegisterLocalCallback()
	/// \param[in] messageId Identifier passed as messageId to RegisterLocalCallback()
	/// \return True if the combination of uniqueID and messageId was in use, and hence removed
	bool UnregisterLocalCallback(const char *uniqueID, MessageID messageId);

	/// Remove the association created with RegisterSlot()
	/// \param[in] sharedIdentifier Identifier passed as sharedIdentifier to RegisterSlot()
	bool UnregisterSlot(const char *sharedIdentifier);

	/// \deprecated Use RegisterSlot() and Signal() with your own RakNetGUID as the send target
	/// Queue a call on this endpoint. See RakPeerInterface::SendLoopback()
	/// \param[in] Identifier originally passed to RegisterFunction() on the local system
	/// \param[in] bitStream bitStream encoded data to send to the function callback
	void CallLoopback(const char *uniqueID, MafiaNet::BitStream *bitStream);

	/// \deprecated, use Signal()
	/// Send to the specified remote instance of RakPeer.
	/// \param[in] uniqueID Identifier originally passed to RegisterFunction() on the remote system(s)
	/// \param[in] bitStream bitStream encoded data to send to the function callback
	/// \param[in] priority See RakPeerInterface::Send()
	/// \param[in] reliability See RakPeerInterface::Send()
	/// \param[in] orderingChannel See RakPeerInterface::Send()
	/// \param[in] systemIdentifier See RakPeerInterface::Send()
	/// \param[in] broadcast See RakPeerInterface::Send()
	void Call(const char *uniqueID, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast);

	/// \brief Same as call, but don't return until the remote system replies.
	/// Broadcasting parameter does not exist, this can only call one remote system
	/// \note This function does not return until the remote system responds, disconnects, or was never connected to begin with
	/// \param[in] Identifier originally passed to RegisterBlockingFunction() on the remote system(s)
	/// \param[in] bitStream bitStream encoded data to send to the function callback
	/// \param[in] priority See RakPeerInterface::Send()
	/// \param[in] reliability See RakPeerInterface::Send()
	/// \param[in] orderingChannel See RakPeerInterface::Send()
	/// \param[in] systemIdentifier See RakPeerInterface::Send()
	/// \param[out] returnData Written to by the function registered with RegisterBlockingFunction.
	/// \return true if successfully called. False on disconnect, function not registered, or not connected to begin with
	bool CallBlocking(const char *uniqueID, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, MafiaNet::BitStream *returnData);

	/// Calls zero or more functions identified by sharedIdentifier registered with RegisterSlot()
	/// \param[in] sharedIdentifier parameter of the same name passed to RegisterSlot() on the remote system
	/// \param[in] bitStream bitStream encoded data to send to the function callback
	/// \param[in] priority See RakPeerInterface::Send()
	/// \param[in] reliability See RakPeerInterface::Send()
	/// \param[in] orderingChannel See RakPeerInterface::Send()
	/// \param[in] systemIdentifier See RakPeerInterface::Send()
	/// \param[in] broadcast See RakPeerInterface::Send()
	/// \param[in] invokeLocal If true, also sends to self.
	void Signal(const char *sharedIdentifier, MafiaNet::BitStream *bitStream, MafiaNet::Priority priority, MafiaNet::Reliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, bool invokeLocal);

	/// If called while processing a slot, no further slots for the currently executing signal will be executed
	void InterruptSignal(void);

	/// The slots registered under one shared identifier with RegisterSlot(). Opaque: use it only as a handle for InvokeSignal().
	struct LocalSlot;

	/// \brief Looks up the slots registered under \a sharedIdentifier, to invoke them repeatedly without a string lookup.
	/// \details The pointer is owned by RPC. It stays valid until UnregisterSlot(sharedIdentifier) is called or this RPC is destroyed;
	/// further RegisterSlot() calls with the same identifier add to the same LocalSlot and do not invalidate it.
	/// \param[in] sharedIdentifier Identifier passed to RegisterSlot()
	/// \return The slots, or nullptr if no slot is registered under \a sharedIdentifier
	LocalSlot *GetLocalSlot(const char *sharedIdentifier);

	/// \brief Calls every slot of \a slot locally, highest callPriority first, as if the signal had arrived in \a packet.
	/// \details InterruptSignal() from within a slot stops the remaining calls. Does nothing if \a slot is nullptr.
	/// \param[in] slot A handle returned by GetLocalSlot()
	/// \param[in] serializedParameters Passed to every slot; its read pointer is reset between calls
	/// \param[in] packet Passed to every slot
	void InvokeSignal(LocalSlot *slot, MafiaNet::BitStream *serializedParameters, Packet *packet);

private:
	/// \internal
	struct LocalCallback
	{
		MessageID messageId;
		/// Sorted function names
		std::vector<std::string> functions;
	};

	/// \internal
	// Callable object, along with priority to call relative to other objects
	struct LocalSlotObject
	{
		LocalSlotObject() {}
		LocalSlotObject(unsigned int _registrationCount, int _callPriority, void (*_functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context), void *_context)
		{
			registrationCount = _registrationCount;
			callPriority = _callPriority;
			functionPointer = _functionPointer;
			context = _context;
		}
		~LocalSlotObject() {}

		// Used so slots are called in the order they are registered
		unsigned int registrationCount;
		int callPriority;
		void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context);
		// Opaque user pointer passed back to functionPointer on invocation
		void *context;
	};

	static int LocalSlotObjectComp(const LocalSlotObject &key, const LocalSlotObject &data);

	friend class RakPeer;
	friend class TCPInterface;
	explicit RPC(RakPeerInterface &peer);
#if _RAKNET_SUPPORT_TCPInterface == 1
	explicit RPC(TCPInterface &tcp);
	TCPInterface *tcpInterface = nullptr;
#endif
	RakPeerInterface *rakPeerInterface = nullptr;
	void RegisterGlobals();
	unsigned int appliedGlobalRegistrations = 0;
	bool ProcessPacket(Packet *packet);
	void Send(const BitStream *data, Priority priority, Reliability reliability, char channel, const AddressOrGUID target, bool broadcast);

	/// \internal A registered nonblocking function paired with its user context
	struct RegisteredNonblockingFunction
	{
		void (*functionPointer)(MafiaNet::BitStream *userData, Packet *packet, void *context);
		void *context;
	};
	/// \internal A registered blocking function paired with its user context
	struct RegisteredBlockingFunction
	{
		void (*functionPointer)(MafiaNet::BitStream *userData, MafiaNet::BitStream *returnData, Packet *packet, void *context);
		void *context;
	};

	/// Slots by shared identifier. Each LocalSlot is heap-allocated, so GetLocalSlot() pointers survive a rehash.
	std::unordered_map<std::string, std::unique_ptr<LocalSlot>> localSlots;
	std::unordered_map<std::string, RegisteredNonblockingFunction> registeredNonblockingFunctions;
	std::unordered_map<std::string, RegisteredBlockingFunction> registeredBlockingFunctions;
	std::map<MessageID, LocalCallback> localCallbacks;

	MafiaNet::BitStream blockingReturnValue;
	bool gotBlockingReturnValue = false;
	RakNetGUID blockingPeer = UNASSIGNED_RAKNET_GUID;

	/// Used so slots are called in the order they are registered
	unsigned int nextSlotRegistrationCount = 0;

	bool interruptSignal = false;
};

} // End namespace
