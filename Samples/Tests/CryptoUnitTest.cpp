/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "CryptoUnitTest.h"
#include "mafianet/crypto/keys.h"
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
		default: return "unknown";
	}
}

CryptoUnitTest::CryptoUnitTest(void) {}
CryptoUnitTest::~CryptoUnitTest(void) {}
