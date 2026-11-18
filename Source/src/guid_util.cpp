/*
 *  Copyright (c) 2024, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "mafianet/guid_util.h"

namespace MafiaNet
{

std::string to_string(const RakNetGUID &g)
{
	// The longest possible output is "UNASSIGNED_RAKNET_GUID" (22 chars) or a
	// 64-bit decimal (20 chars); 64 bytes matches the legacy buffer size and
	// leaves ample headroom. Buffer is local, so this is thread-safe.
	char buffer[64];
	g.ToString(buffer, sizeof(buffer));
	return std::string(buffer);
}

std::string to_string(const SystemAddress &address, bool writePort, char portDelineator)
{
	char buffer[SystemAddress::TO_STRING_BUFFER_SIZE];
	address.ToString(writePort, buffer, sizeof(buffer), portDelineator);
	return std::string(buffer);
}

std::string to_string(const AddressOrGUID &value, bool writePort)
{
	if (value.rakNetGuid != UNASSIGNED_RAKNET_GUID)
		return to_string(value.rakNetGuid);
	return to_string(value.systemAddress, writePort);
}

std::optional<SystemAddress> connected_address(RakPeerInterface &peer, const RakNetGUID &g)
{
	const SystemAddress address = peer.GetSystemAddressFromGuid(g);
	if (address == UNASSIGNED_SYSTEM_ADDRESS)
		return std::nullopt;
	return address;
}

} // namespace MafiaNet
