/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */

#include "CryptoUnitTest.h"
#include "mafianet/crypto/keys.h"
#include "mafianet/crypto/noise.h"
#include "mafianet/crypto/securesession.h"
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

	// --- full NK handshake: matching keys, mirrored direction ---
	{
		ServerSecurityKey srv = GenerateServerSecurityKey();
		NoiseHandshake cli, ser;
		cli.InitInitiator(srv.publicKey);
		ser.InitResponder(srv.publicKey, srv.secretKey);
		unsigned char msgA[48], msgB[48];
		cli.WriteMessageA(msgA);
		if (!ser.ReadMessageA(msgA)) return 20;
		ser.WriteMessageB(msgB);
		if (!cli.ReadMessageB(msgB)) return 21;
		unsigned char cs[32], cr[32], srs[32], srr[32];
		cli.GetTransportKeys(cs, cr);
		ser.GetTransportKeys(srs, srr);
		if (memcmp(cs, srr, 32) != 0) return 22;   // client.send == server.recv
		if (memcmp(cr, srs, 32) != 0) return 23;   // client.recv == server.send
	}
	// --- wrong pinned key is rejected (handshake must not fully complete) ---
	{
		ServerSecurityKey srv = GenerateServerSecurityKey();
		ServerSecurityKey wrong = GenerateServerSecurityKey();
		NoiseHandshake cli, ser;
		cli.InitInitiator(wrong.publicKey);
		ser.InitResponder(srv.publicKey, srv.secretKey);
		unsigned char msgA[48], msgB[48];
		cli.WriteMessageA(msgA);
		bool aOk = ser.ReadMessageA(msgA);
		ser.WriteMessageB(msgB);
		bool bOk = cli.ReadMessageB(msgB);
		if (aOk && bOk) return 24;                  // must NOT both succeed
	}

	if (isVerbose) printf("CryptoUnitTest: NK handshake property tests OK\n");

	// --- known-answer vector vs independent reference (Noise_NK_25519_ChaChaPoly_SHA512) ---
	{
		// helper: parse hex -> bytes
		auto hex2bin = [](const char *hex, unsigned char *out, size_t n) {
			for (size_t i = 0; i < n; ++i) {
				auto v = [](char c)->int { return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:0; };
				out[i] = (unsigned char)((v(hex[2*i])<<4) | v(hex[2*i+1]));
			}
		};
		unsigned char sStaticSk[32], sStaticPk[32], cEphSk[32], sEphSk[32];
		unsigned char expMsgA[48], expMsgB[48], expSend[32], expRecv[32];
		hex2bin("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", sStaticSk, 32);
		hex2bin("8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f", sStaticPk, 32);
		hex2bin("202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f", cEphSk, 32);
		hex2bin("404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f", sEphSk, 32);
		hex2bin("358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd1662545af44fef396413c7a4df9863201ca3e3", expMsgA, 48);
		hex2bin("79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51aeb4c231a74b919c6a6eb434316d726da", expMsgB, 48);
		hex2bin("0f2da207713331188b71d56e612b55e5ea8d6c48957f8d883f01cd7a0ce45fc2", expSend, 32);
		hex2bin("2ba4183122cd88990aa7b7c6de4533e6ad3f58d679bfa6212bbd27b29280394f", expRecv, 32);

		NoiseHandshake cli, ser;
		cli.InitInitiator(sStaticPk);
		ser.InitResponder(sStaticPk, sStaticSk);
		cli.SetEphemeralForTesting(cEphSk);
		ser.SetEphemeralForTesting(sEphSk);
		unsigned char msgA[48], msgB[48];
		cli.WriteMessageA(msgA);
		if (memcmp(msgA, expMsgA, 48) != 0) return 25;
		if (!ser.ReadMessageA(msgA)) return 26;
		ser.WriteMessageB(msgB);
		if (memcmp(msgB, expMsgB, 48) != 0) return 27;
		if (!cli.ReadMessageB(msgB)) return 28;
		unsigned char cs[32], cr[32];
		cli.GetTransportKeys(cs, cr);
		if (memcmp(cs, expSend, 32) != 0) return 29;
		if (memcmp(cr, expRecv, 32) != 0) return 30;
	}

	if (isVerbose) printf("CryptoUnitTest: NK known-answer vector OK\n");

	// --- trial-verify on a copy: RakPeer verifies message B on a copy of the
	// initiator state so a stale/forged REPLY_2 cannot corrupt the attempt.
	// A failed ReadMessageB on a copy must leave the original verifiable. ---
	{
		ServerSecurityKey srv = GenerateServerSecurityKey();
		NoiseHandshake cli, ser;
		cli.InitInitiator(srv.publicKey);
		ser.InitResponder(srv.publicKey, srv.secretKey);
		unsigned char msgA[48], msgB[48];
		cli.WriteMessageA(msgA);
		if (!ser.ReadMessageA(msgA)) return 31;
		ser.WriteMessageB(msgB);
		// A forged message B must fail when verified on a copy...
		NoiseHandshake bad = cli;
		unsigned char garbage[48]; memset(garbage, 0xAB, sizeof garbage);
		if (bad.ReadMessageB(garbage)) return 32;
		// ...and the original must remain intact so the genuine B still verifies.
		NoiseHandshake good = cli;
		if (!good.ReadMessageB(msgB)) return 33;
		unsigned char cs[32], cr[32], ss2[32], sr2[32];
		good.GetTransportKeys(cs, cr);
		ser.GetTransportKeys(ss2, sr2);
		if (memcmp(cs, sr2, 32) != 0 || memcmp(cr, ss2, 32) != 0) return 34;
	}

	if (isVerbose) printf("CryptoUnitTest: trial-verify on copy OK\n");

	// --- SecureSession round-trip, tamper, replay ---
	{
		unsigned char k1[32], k2[32];
		for (int i = 0; i < 32; ++i) { k1[i] = (unsigned char)i; k2[i] = (unsigned char)(255 - i); }
		SecureSession a, b;
		a.SetKeys(k1, k2);   // a sends with k1
		b.SetKeys(k2, k1);   // b receives a's traffic (rx=k1)

		unsigned char buf[256]; const char *msg = "hello world";
		unsigned int len = (unsigned int)strlen(msg);
		memcpy(buf, msg, len);
		if (!a.Encrypt(buf, sizeof buf, len)) return 40;
		unsigned char framed[256]; memcpy(framed, buf, len); unsigned int flen = len;
		if (!b.Decrypt(buf, len)) return 41;
		if (len != strlen(msg) || memcmp(buf, msg, len) != 0) return 42;

		// tamper: flip a ciphertext byte -> must fail
		unsigned char t[256]; unsigned int tlen = flen; memcpy(t, framed, flen);
		t[12] ^= 0x01;
		if (b.Decrypt(t, tlen)) return 43;

		// replay: re-decrypt the original framed datagram -> must fail
		unsigned int rlen = flen; unsigned char r[256]; memcpy(r, framed, flen);
		if (b.Decrypt(r, rlen)) return 44;

		// fresh sequence still accepted after the rejected ones
		unsigned char buf2[256]; const char *msg2 = "second";
		unsigned int len2 = (unsigned int)strlen(msg2); memcpy(buf2, msg2, len2);
		if (!a.Encrypt(buf2, sizeof buf2, len2)) return 45;
		if (!b.Decrypt(buf2, len2)) return 46;
		if (len2 != strlen(msg2) || memcmp(buf2, msg2, len2) != 0) return 47;
	}

	if (isVerbose) printf("CryptoUnitTest: SecureSession round-trip/tamper/replay OK\n");

	// --- string key helpers (hex/base64) round-trips ---
	{
		ServerSecurityKey g = GenerateServerSecurityKey();
		// secret hex round-trip -> derived keypair matches original
		char shex[SERVER_KEY_HEX_LEN];
		ServerSecurityKeySecretToHex(g, shex, sizeof shex);
		ServerSecurityKey k1;
		if (!ServerSecurityKeyFromSecretHex(shex, k1)) return 50;
		if (memcmp(k1.secretKey, g.secretKey, 32) != 0) return 51;
		if (memcmp(k1.publicKey, g.publicKey, 32) != 0) return 52;   // public correctly derived
		// secret base64 round-trip
		char sb64[SERVER_KEY_BASE64_LEN];
		ServerSecurityKeySecretToBase64(g, sb64, sizeof sb64);
		ServerSecurityKey k2;
		if (!ServerSecurityKeyFromSecretBase64(sb64, k2)) return 53;
		if (memcmp(k2.secretKey, g.secretKey, 32) != 0 || memcmp(k2.publicKey, g.publicKey, 32) != 0) return 54;
		// public hex/base64 parse (client side)
		char phex[SERVER_KEY_HEX_LEN]; ServerSecurityKeyPublicToHex(g, phex, sizeof phex);
		unsigned char pub[32];
		if (!ServerPublicKeyFromHex(phex, pub) || memcmp(pub, g.publicKey, 32) != 0) return 55;
		char pb64[SERVER_KEY_BASE64_LEN]; ServerSecurityKeyPublicToBase64(g, pb64, sizeof pb64);
		unsigned char pub2[32];
		if (!ServerPublicKeyFromBase64(pb64, pub2) || memcmp(pub2, g.publicKey, 32) != 0) return 56;
		// malformed inputs rejected
		ServerSecurityKey bad;
		if (ServerSecurityKeyFromSecretHex("zzzz", bad)) return 57;           // non-hex
		if (ServerSecurityKeyFromSecretHex("00112233", bad)) return 58;        // wrong length (4 bytes)
	}

	if (isVerbose) printf("CryptoUnitTest: string key helpers (hex/base64) OK\n");
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
		case 20: return "readA failed";
		case 21: return "readB failed";
		case 22: return "client.send != server.recv";
		case 23: return "client.recv != server.send";
		case 24: return "wrong key accepted";
		case 25: return "KAT msgA mismatch";
		case 26: return "KAT readA failed";
		case 27: return "KAT msgB mismatch";
		case 28: return "KAT readB failed";
		case 29: return "KAT send key mismatch";
		case 30: return "KAT recv key mismatch";
		case 31: return "copy-verify: readA failed";
		case 32: return "copy-verify: forged message B accepted";
		case 33: return "copy-verify: original state corrupted by failed copy verify";
		case 34: return "copy-verify: transport keys mismatch";
		case 40: return "encrypt failed";
		case 41: return "decrypt failed";
		case 42: return "roundtrip mismatch";
		case 43: return "tamper accepted";
		case 44: return "replay accepted";
		case 45: return "encrypt2 failed";
		case 46: return "decrypt2 failed";
		case 47: return "roundtrip2 mismatch";
		case 50: return "ServerSecurityKeyFromSecretHex returned false";
		case 51: return "hex round-trip: secret mismatch";
		case 52: return "hex round-trip: derived public mismatch";
		case 53: return "ServerSecurityKeyFromSecretBase64 returned false";
		case 54: return "base64 round-trip: key mismatch";
		case 55: return "ServerPublicKeyFromHex failed or mismatch";
		case 56: return "ServerPublicKeyFromBase64 failed or mismatch";
		case 57: return "non-hex input incorrectly accepted";
		case 58: return "wrong-length hex incorrectly accepted";
		default: return "unknown";
	}
}

CryptoUnitTest::CryptoUnitTest(void) {}
CryptoUnitTest::~CryptoUnitTest(void) {}
