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

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_TwoWayAuthentication == 1

#include "mafianet/two_way_authentication.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/peer_interface.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <sodium.h>

using namespace MafiaNet;

enum NegotiationIdentifiers
{
	ID_NONCE_REQUEST,
	ID_NONCE_REPLY,
	ID_HASHED_NONCE_AND_PASSWORD,
};

// A clock that stepped backwards never expires anything.
static bool HasExpired(MafiaNet::Time curTime, MafiaNet::Time since, MafiaNet::Time lifetimeMs)
{
	return curTime >= since && curTime - since >= lifetimeMs;
}

TwoWayAuthentication::NonceGenerator::NonceGenerator()
{
	nextRequestId = 0;
}
TwoWayAuthentication::NonceGenerator::~NonceGenerator()
{
	Clear();
}
void TwoWayAuthentication::NonceGenerator::GetNonce(char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], unsigned short *requestId, MafiaNet::AddressOrGUID remoteSystem)
{
	std::unique_ptr<TwoWayAuthentication::NonceAndRemoteSystemRequest> narsr(new TwoWayAuthentication::NonceAndRemoteSystemRequest);
	narsr->remoteSystem = remoteSystem;
	GenerateNonce(narsr->nonce);
	narsr->requestId = nextRequestId++;
	*requestId = narsr->requestId;
	memcpy(nonce, narsr->nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
	narsr->whenGenerated = MafiaNet::GetTime();
	generatedNonces.push_back(std::move(narsr));
}
void TwoWayAuthentication::NonceGenerator::GenerateNonce(char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH])
{
	// A nonce must be unpredictable, which the Mersenne Twister is not. libsodium is initialised by
	// RakPeer::Startup; sodium_init is idempotent, so make sure for a plugin used before that.
	const int sodiumReady = sodium_init();
	(void) sodiumReady;
	randombytes_buf(nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
}
bool TwoWayAuthentication::NonceGenerator::GetNonceById(char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], unsigned short requestId, MafiaNet::AddressOrGUID remoteSystem, bool popIfFound)
{
	for (size_t i = 0; i < generatedNonces.size(); i++)
	{
		if (generatedNonces[i]->requestId == requestId)
		{
			if (remoteSystem == generatedNonces[i]->remoteSystem)
			{
				memcpy(nonce, generatedNonces[i]->nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
				if (popIfFound)
					generatedNonces.erase(generatedNonces.begin() + (std::ptrdiff_t) i);
				return true;
			}
			else
			{
				return false;
			}
		}
	}
	return false;
}
void TwoWayAuthentication::NonceGenerator::Clear(void)
{
	generatedNonces.clear();
}
void TwoWayAuthentication::NonceGenerator::ClearByAddress(MafiaNet::AddressOrGUID remoteSystem)
{
	generatedNonces.erase(std::remove_if(generatedNonces.begin(), generatedNonces.end(), [&remoteSystem](const std::unique_ptr<NonceAndRemoteSystemRequest> &generated)
	                                     { return generated->remoteSystem == remoteSystem; }),
	                      generatedNonces.end());
}
void TwoWayAuthentication::NonceGenerator::Update(MafiaNet::Time curTime, MafiaNet::Time lifetimeMs)
{
	// Generated in time order, so the expired ones are a prefix. All of them go: removing one per
	// call let a busy peer keep nonces far past their lifetime.
	size_t expired = 0;
	while (expired < generatedNonces.size() && HasExpired(curTime, generatedNonces[expired]->whenGenerated, lifetimeMs))
		expired++;
	generatedNonces.erase(generatedNonces.begin(), generatedNonces.begin() + (std::ptrdiff_t) expired);
}
TwoWayAuthentication::TwoWayAuthentication()
{
	timeoutMs = TWO_WAY_AUTHENTICATION_DEFAULT_TIMEOUT_MS;
}
void TwoWayAuthentication::SetTimeout(MafiaNet::Time _timeoutMs)
{
	timeoutMs = _timeoutMs;
}
MafiaNet::Time TwoWayAuthentication::GetTimeout(void) const
{
	return timeoutMs;
}
TwoWayAuthentication::~TwoWayAuthentication()
{
	Clear();
}
bool TwoWayAuthentication::AddPassword(MafiaNet::RakString identifier, MafiaNet::RakString password)
{
	if (password.IsEmpty())
		return false;

	if (identifier.IsEmpty())
		return false;

	if (password == identifier)
		return false; // Insecure

	// emplace fails if this identifier is already in use
	return passwords.emplace(identifier, password).second;
}
bool TwoWayAuthentication::Challenge(MafiaNet::RakString identifier, AddressOrGUID remoteSystem)
{
	if (passwords.count(identifier) == 0)
		return false;

	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_TWO_WAY_AUTHENTICATION_NEGOTIATION);
	bsOut.Write((MessageID) ID_NONCE_REQUEST);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, remoteSystem, false);

	PendingChallenge pc;
	pc.identifier = identifier;
	pc.remoteSystem = remoteSystem;
	pc.time = MafiaNet::GetTime();
	pc.sentHash = false;
	outgoingChallenges.push_back(pc);

	return true;
}
void TwoWayAuthentication::Update(void)
{
	UpdateTimeouts(MafiaNet::GetTime());
}
void TwoWayAuthentication::UpdateTimeouts(MafiaNet::Time curTime)
{
	nonceGenerator.Update(curTime, timeoutMs);
	while (!outgoingChallenges.empty() && HasExpired(curTime, outgoingChallenges.front().time, timeoutMs))
	{
		PendingChallenge pc = outgoingChallenges.front();
		outgoingChallenges.pop_front();

		// Tell the user about the timeout
		PushToUser(ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT, pc.identifier, pc.remoteSystem);
	}
}
PluginReceiveResult TwoWayAuthentication::OnReceive(Packet *packet)
{
	switch (packet->data[0])
	{
	case ID_TWO_WAY_AUTHENTICATION_NEGOTIATION:
	{
		if (packet->length >= sizeof(MessageID) * 2)
		{
			switch (packet->data[sizeof(MessageID)])
			{
			case ID_NONCE_REQUEST:
			{
				OnNonceRequest(packet);
			}
			break;
			case ID_NONCE_REPLY:
			{
				OnNonceReply(packet);
			}
			break;
			case ID_HASHED_NONCE_AND_PASSWORD:
			{
				return OnHashedNonceAndPassword(packet);
			}
			break;
			}
		}
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	}
	case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE:
	case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS:
	{
		if (packet->wasGeneratedLocally == false)
		{
			OnPasswordResult(packet);
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
		else
			break;
	}
	break;
	// These should only be generated locally
	case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS:
	case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE:
	case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT:
		if (packet->wasGeneratedLocally == false)
			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		break;
	}

	return RR_CONTINUE_PROCESSING;
}
void TwoWayAuthentication::OnRakPeerShutdown(void)
{
	Clear();
}
void TwoWayAuthentication::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;

	// Remove from pending challenges
	outgoingChallenges.erase(std::remove_if(outgoingChallenges.begin(), outgoingChallenges.end(), [&](const PendingChallenge &challenge)
	                                        { return (rakNetGUID != UNASSIGNED_RAKNET_GUID && challenge.remoteSystem.rakNetGuid == rakNetGUID) ||
		                                             (systemAddress != UNASSIGNED_SYSTEM_ADDRESS && challenge.remoteSystem.systemAddress == systemAddress); }),
	                         outgoingChallenges.end());

	if (rakNetGUID != UNASSIGNED_RAKNET_GUID)
		nonceGenerator.ClearByAddress(rakNetGUID);
	else
		nonceGenerator.ClearByAddress(systemAddress);
}
void TwoWayAuthentication::Clear(void)
{
	outgoingChallenges.clear();
	passwords.clear();
	nonceGenerator.Clear();
}
void TwoWayAuthentication::PushToUser(MessageID messageId, MafiaNet::RakString password, MafiaNet::AddressOrGUID remoteSystem)
{
	MafiaNet::BitStream output;
	output.Write(messageId);
	if (password.IsEmpty() == false)
		output.Write(password);
	Packet *p = AllocatePacketUnified(output.GetNumberOfBytesUsed());
	p->systemAddress = remoteSystem.systemAddress;
	p->systemAddress.systemIndex = (SystemIndex) -1;
	p->guid = remoteSystem.rakNetGuid;
	p->wasGeneratedLocally = true;
	memcpy(p->data, output.GetData(), output.GetNumberOfBytesUsed());
	rakPeerInterface->PushBackPacket(p, true);
}
void TwoWayAuthentication::OnNonceRequest(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
	unsigned short requestId;
	nonceGenerator.GetNonce(nonce, &requestId, packet);

	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_TWO_WAY_AUTHENTICATION_NEGOTIATION);
	bsOut.Write((MessageID) ID_NONCE_REPLY);
	bsOut.Write(requestId);
	bsOut.WriteAlignedBytes((const unsigned char *) nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet, false);
}
void TwoWayAuthentication::OnNonceReply(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	char thierNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
	unsigned short requestId;
	bsIn.Read(requestId);
	bsIn.ReadAlignedBytes((unsigned char *) thierNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);

	// Lookup one of the negotiations for this guid/system address
	AddressOrGUID aog(packet);
	for (size_t i = 0; i < outgoingChallenges.size(); i++)
	{
		if (outgoingChallenges[i].remoteSystem == aog && outgoingChallenges[i].sentHash == false)
		{
			outgoingChallenges[i].sentHash = true;

			// Get the password for this identifier
			const auto entry = passwords.find(outgoingChallenges[i].identifier);
			if (entry != passwords.end())
			{
				MafiaNet::RakString password = entry->second;

				// Hash their nonce with password and reply
				char hashedNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
				Hash(thierNonce, password, hashedNonceAndPw);

				// Send
				MafiaNet::BitStream bsOut;
				bsOut.Write((MessageID) ID_TWO_WAY_AUTHENTICATION_NEGOTIATION);
				bsOut.Write((MessageID) ID_HASHED_NONCE_AND_PASSWORD);
				bsOut.Write(requestId);
				bsOut.Write(outgoingChallenges[i].identifier); // Identifier helps the other system lookup the password quickly.
				bsOut.WriteAlignedBytes((const unsigned char *) hashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH);
				SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet, false);
			}

			return;
		}
	}
}
PluginReceiveResult TwoWayAuthentication::OnHashedNonceAndPassword(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 2);

	char remoteHashedNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
	unsigned short requestId;
	bsIn.Read(requestId);
	MafiaNet::RakString passwordIdentifier;
	bsIn.Read(passwordIdentifier);
	bsIn.ReadAlignedBytes((unsigned char *) remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH);

	// Look up used nonce from requestId
	char usedNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
	if (nonceGenerator.GetNonceById(usedNonce, requestId, packet, true) == false)
		return RR_STOP_PROCESSING_AND_DEALLOCATE;

	// Look the received identifier up as is, never through RakString's printf-style constructor.
	const auto entry = passwords.find(passwordIdentifier);
	if (entry != passwords.end())
	{
		char hashedThisNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
		Hash(usedNonce, entry->second, hashedThisNonceAndPw);
		if (memcmp(hashedThisNonceAndPw, remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH) == 0)
		{
			// Pass
			MafiaNet::BitStream bsOut;
			bsOut.Write((MessageID) ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS);
			bsOut.WriteAlignedBytes((const unsigned char *) usedNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
			bsOut.WriteAlignedBytes((const unsigned char *) remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH);
			bsOut.Write(passwordIdentifier);
			SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet, false);

			// Incoming success, modify packet header to tell user
			PushToUser(ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS, passwordIdentifier, packet);

			return RR_STOP_PROCESSING_AND_DEALLOCATE;
		}
	}

	// Incoming failure, modify arrived packet header to tell user
	packet->data[0] = (MessageID) ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE;

	MafiaNet::BitStream bsOut;
	bsOut.Write((MessageID) ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE);
	bsOut.WriteAlignedBytes((const unsigned char *) usedNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
	bsOut.WriteAlignedBytes((const unsigned char *) remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH);
	bsOut.Write(passwordIdentifier);
	SendUnified(&bsOut, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, packet, false);

	return RR_CONTINUE_PROCESSING;
}
void TwoWayAuthentication::OnPasswordResult(Packet *packet)
{
	MafiaNet::BitStream bsIn(packet->data, packet->length, false);
	bsIn.IgnoreBytes(sizeof(MessageID) * 1);
	char usedNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
	bsIn.ReadAlignedBytes((unsigned char *) usedNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
	char hashedNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
	bsIn.ReadAlignedBytes((unsigned char *) hashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH);
	MafiaNet::RakString passwordIdentifier;
	bsIn.Read(passwordIdentifier);

	const auto entry = passwords.find(passwordIdentifier);
	if (entry != passwords.end())
	{
		MafiaNet::RakString password = entry->second;
		char testHash[HASHED_NONCE_AND_PW_LENGTH];
		Hash(usedNonce, password, testHash);
		if (memcmp(testHash, hashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH) == 0)
		{
			// Lookup the outgoing challenge and remove it from the list
			AddressOrGUID aog(packet);
			for (size_t i = 0; i < outgoingChallenges.size(); i++)
			{
				if (outgoingChallenges[i].identifier == passwordIdentifier &&
				    outgoingChallenges[i].remoteSystem == aog &&
				    outgoingChallenges[i].sentHash == true)
				{
					outgoingChallenges.erase(outgoingChallenges.begin() + (std::ptrdiff_t) i);

					PushToUser(packet->data[0], passwordIdentifier, packet);
					return;
				}
			}
		}
	}
}
void TwoWayAuthentication::Hash(char thierNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], MafiaNet::RakString password, char out[HASHED_NONCE_AND_PW_LENGTH])
{
	CSHA1 sha1;
	sha1.Update((unsigned char *) thierNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH);
	sha1.Update((unsigned char *) password.C_String(), (unsigned int) password.GetLength());
	sha1.Final();
	sha1.GetHash((unsigned char *) out);
}

#endif
