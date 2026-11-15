/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Generates a server identity key pair and prints both halves in hex.
//
//   public key  (32 bytes): publish it. Clients pin it with Connect()'s PublicKey* (or
//                           ClientBuilder::pin_server_identity()).
//   secret key  (64 bytes): keep it private. The server installs it with
//                           RakPeerInterface::SetIdentityKey() (or ServerBuilder::identity_key())
//                           before Startup().
//
// Usage: IdentityKeyGen

#include <cstdio>

#include "mafianet/session_crypto.h"

using namespace MafiaNet;

namespace
{
void PrintHex(const char *label, const uint8_t *data, int length)
{
	printf("%s", label);
	for (int i = 0; i < length; ++i)
		printf("%02x", data[i]);
	printf("\n");
}
} // namespace

int main()
{
	uint8_t publicKey[kIdentityPublicKeyBytes];
	uint8_t secretKey[kIdentitySecretKeyBytes];
	GenerateIdentityKeyPair(publicKey, secretKey);

	PrintHex("public key: ", publicKey, kIdentityPublicKeyBytes);
	PrintHex("secret key: ", secretKey, kIdentitySecretKeyBytes);

	SecureZero(secretKey, sizeof(secretKey));
	return 0;
}
