/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "CryptoUnitTest.h"
#include "mafianet/crypto/keys.h"
#include "mafianet/crypto/noise.h"
#include <sodium.h>
#include <cstring>
#include <cstdio>

using namespace MafiaNet;

int CryptoUnitTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	if (sodium_init() < 0) return 1;

	// --- keys: public key is the scalarmult_base of the secret, and nonzero ---
	ServerSecurityKey k = GenerateServerSecurityKey();
	unsigned char expectedPub[32];
	crypto_scalarmult_base(expectedPub, k.secretKey);
	if (memcmp(expectedPub, k.publicKey, 32) != 0) return 2;
	unsigned char zero[32]; memset(zero, 0, 32);
	if (memcmp(k.secretKey, zero, 32) == 0) return 3;

	// two calls produce different keys
	ServerSecurityKey k2 = GenerateServerSecurityKey();
	if (memcmp(k.secretKey, k2.secretKey, 32) == 0) return 4;

	if (isVerbose) printf("CryptoUnitTest: keys OK\n");

	// --- HKDF determinism + output length sanity ---
	{
		unsigned char ck[64]; memset(ck, 0x01, 64);
		unsigned char ikm[32]; memset(ikm, 0x02, 32);
		unsigned char a[128], b[128];
		Noise_HKDF(ck, ikm, 32, 2, a);
		Noise_HKDF(ck, ikm, 32, 2, b);
		if (memcmp(a, b, 128) != 0) return 10;
		if (memcmp(a, a + 64, 64) == 0) return 11;
	}
	// --- SymmetricState round-trips an AEAD payload ---
	{
		NoiseSymmetricState s1, s2;
		s1.InitializeSymmetric("Noise_NK_25519_ChaChaPoly_SHA512");
		s2.InitializeSymmetric("Noise_NK_25519_ChaChaPoly_SHA512");
		unsigned char ikm[32]; memset(ikm, 0x07, 32);
		s1.MixKey(ikm, 32);
		s2.MixKey(ikm, 32);
		const unsigned char pt[5] = {1,2,3,4,5};
		unsigned char ct[5 + 16]; unsigned char rt[5 + 16]; size_t rlen = 0;
		size_t clen = s1.EncryptAndHash(pt, 5, ct);
		if (clen != 21) return 12;
		if (!s2.DecryptAndHash(ct, clen, rt, &rlen)) return 13;
		if (rlen != 5 || memcmp(rt, pt, 5) != 0) return 14;
	}

	if (isVerbose) printf("CryptoUnitTest: noise symmetric core OK\n");
	return 0;
}

RakString CryptoUnitTest::GetTestName() { return "CryptoUnitTest"; }

RakString CryptoUnitTest::ErrorCodeToString(int e)
{
	switch (e) {
		case 0: return "ok";
		case 1: return "sodium_init failed";
		case 2: return "public key != scalarmult_base(secret)";
		case 3: return "secret key all zero";
		case 4: return "two keypairs identical";
		case 10: return "HKDF not deterministic";
		case 11: return "HKDF outputs equal";
		case 12: return "ciphertext len wrong";
		case 13: return "AEAD decrypt failed";
		case 14: return "roundtrip mismatch";
		default: return "unknown";
	}
}

CryptoUnitTest::CryptoUnitTest(void) {}
CryptoUnitTest::~CryptoUnitTest(void) {}
