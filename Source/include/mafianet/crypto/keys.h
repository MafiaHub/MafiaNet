/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#pragma once
#include "mafianet/Export.h"
#include <cstddef>

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

// Buffer sizes for the string encoders below (include the NUL terminator).
static const size_t SERVER_KEY_HEX_LEN    = 65;  // 32 bytes -> 64 hex chars + NUL
static const size_t SERVER_KEY_BASE64_LEN = 45;  // sodium_base64_ENCODED_LEN(32, ORIGINAL variant)

/// Derive the full keypair (public computed from the secret) from a raw 32-byte secret.
RAK_DLL_EXPORT ServerSecurityKey ServerSecurityKeyFromSecret(const unsigned char secretKey[32]);

/// Load a server keypair from just the SECRET, encoded as a string (public is derived).
/// Returns false if the string is malformed or not exactly 32 bytes once decoded.
RAK_DLL_EXPORT bool ServerSecurityKeyFromSecretHex(const char* secretHex, ServerSecurityKey& outKey);
RAK_DLL_EXPORT bool ServerSecurityKeyFromSecretBase64(const char* secretBase64, ServerSecurityKey& outKey);

/// Client side: parse the pinned 32-byte server PUBLIC key from a config string into outPublicKey.
/// Returns false on malformed input. Pass outPublicKey to Connect(...).
RAK_DLL_EXPORT bool ServerPublicKeyFromHex(const char* publicHex, unsigned char outPublicKey[32]);
RAK_DLL_EXPORT bool ServerPublicKeyFromBase64(const char* publicBase64, unsigned char outPublicKey[32]);

/// Encode a key half to a string (for a key-generation tool or to write into a config).
/// `out` must be at least SERVER_KEY_HEX_LEN (hex) or SERVER_KEY_BASE64_LEN (base64) bytes.
/// Note: sodium_bin2hex / sodium_bin2base64 abort if the output buffer is too small.
RAK_DLL_EXPORT void ServerSecurityKeySecretToHex(const ServerSecurityKey& key, char* out, size_t outLen);
RAK_DLL_EXPORT void ServerSecurityKeyPublicToHex(const ServerSecurityKey& key, char* out, size_t outLen);
RAK_DLL_EXPORT void ServerSecurityKeySecretToBase64(const ServerSecurityKey& key, char* out, size_t outLen);
RAK_DLL_EXPORT void ServerSecurityKeyPublicToBase64(const ServerSecurityKey& key, char* out, size_t outLen);

} // namespace MafiaNet
