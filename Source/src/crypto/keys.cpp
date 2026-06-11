/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "mafianet/crypto/keys.h"
#include <sodium.h>
#include <cstring>

namespace MafiaNet {

ServerSecurityKey GenerateServerSecurityKey()
{
	ServerSecurityKey k;
	// These helpers are documented as callable before RakPeerInterface::Startup(),
	// so they must initialize libsodium themselves (sodium_init is idempotent and
	// thread-safe). Fail closed with an all-zero key if the RNG cannot be set up.
	if (sodium_init() < 0) {
		memset(k.publicKey, 0, sizeof(k.publicKey));
		memset(k.secretKey, 0, sizeof(k.secretKey));
		return k;
	}
	randombytes_buf(k.secretKey, sizeof(k.secretKey));
	// Derive the X25519 public key from the random secret.
	crypto_scalarmult_base(k.publicKey, k.secretKey);
	return k;
}

ServerSecurityKey ServerSecurityKeyFromSecret(const unsigned char secretKey[32])
{
	ServerSecurityKey k;
	if (sodium_init() < 0) {
		memset(k.publicKey, 0, sizeof(k.publicKey));
		memset(k.secretKey, 0, sizeof(k.secretKey));
		return k;
	}
	memcpy(k.secretKey, secretKey, 32);
	crypto_scalarmult_base(k.publicKey, k.secretKey);
	return k;
}

bool ServerSecurityKeyFromSecretHex(const char* secretHex, ServerSecurityKey& outKey)
{
	if (secretHex == nullptr) return false;
	unsigned char sec[32];
	size_t binLen = 0;
	if (sodium_hex2bin(sec, sizeof sec, secretHex, strlen(secretHex), nullptr, &binLen, nullptr) != 0 || binLen != 32) {
		sodium_memzero(sec, sizeof sec);
		return false;
	}
	outKey = ServerSecurityKeyFromSecret(sec);
	sodium_memzero(sec, sizeof sec);
	return true;
}

bool ServerSecurityKeyFromSecretBase64(const char* secretBase64, ServerSecurityKey& outKey)
{
	if (secretBase64 == nullptr) return false;
	unsigned char sec[32];
	size_t binLen = 0;
	if (sodium_base642bin(sec, sizeof sec, secretBase64, strlen(secretBase64), nullptr, &binLen, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0 || binLen != 32) {
		sodium_memzero(sec, sizeof sec);
		return false;
	}
	outKey = ServerSecurityKeyFromSecret(sec);
	sodium_memzero(sec, sizeof sec);
	return true;
}

bool ServerPublicKeyFromHex(const char* publicHex, unsigned char outPublicKey[32])
{
	if (publicHex == nullptr) return false;
	size_t binLen = 0;
	return sodium_hex2bin(outPublicKey, 32, publicHex, strlen(publicHex), nullptr, &binLen, nullptr) == 0 && binLen == 32;
}

bool ServerPublicKeyFromBase64(const char* publicBase64, unsigned char outPublicKey[32])
{
	if (publicBase64 == nullptr) return false;
	size_t binLen = 0;
	return sodium_base642bin(outPublicKey, 32, publicBase64, strlen(publicBase64), nullptr, &binLen, nullptr, sodium_base64_VARIANT_ORIGINAL) == 0 && binLen == 32;
}

void ServerSecurityKeySecretToHex(const ServerSecurityKey& key, char* out, size_t outLen)
{ sodium_bin2hex(out, outLen, key.secretKey, 32); }

void ServerSecurityKeyPublicToHex(const ServerSecurityKey& key, char* out, size_t outLen)
{ sodium_bin2hex(out, outLen, key.publicKey, 32); }

void ServerSecurityKeySecretToBase64(const ServerSecurityKey& key, char* out, size_t outLen)
{ sodium_bin2base64(out, outLen, key.secretKey, 32, sodium_base64_VARIANT_ORIGINAL); }

void ServerSecurityKeyPublicToBase64(const ServerSecurityKey& key, char* out, size_t outLen)
{ sodium_bin2base64(out, outLen, key.publicKey, 32, sodium_base64_VARIANT_ORIGINAL); }

} // namespace MafiaNet
