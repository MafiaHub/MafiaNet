#pragma once
#include "mafianet/Export.h"
#include <cstdint>

namespace MafiaNet {

/// A server's long-term X25519 identity keypair.
/// The public half is distributed to (and pinned by) clients; the secret half
/// never leaves the server.
struct RAK_DLL_EXPORT ServerSecurityKey
{
	unsigned char publicKey[32];
	unsigned char secretKey[32];
};

/// Generate a fresh random server identity keypair.
/// Requires sodium_init() to have succeeded (RakPeerInterface::Startup does this).
RAK_DLL_EXPORT ServerSecurityKey GenerateServerSecurityKey();

} // namespace MafiaNet
