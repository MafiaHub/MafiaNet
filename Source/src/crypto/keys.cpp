/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "mafianet/crypto/keys.h"
#include <sodium.h>

namespace MafiaNet {

ServerSecurityKey GenerateServerSecurityKey()
{
	ServerSecurityKey k;
	randombytes_buf(k.secretKey, sizeof(k.secretKey));
	// Derive the X25519 public key from the random secret.
	crypto_scalarmult_base(k.publicKey, k.secretKey);
	return k;
}

} // namespace MafiaNet
