/*
 *  Copyright (c) 2018-2019, SLikeSoft UG (haftungsbeschraenkt)
 *
 *  This source code is  licensed under the MIT-style license found in the license.txt
 *  file in the root directory of this source tree.
 */
#pragma once

#include "mafianet/crypto/securestring.h"   // used for MafiaNet::Crypto::CSecureString
#include "mafianet/crypto/ifileencrypter.h" // used for MafiaNet::Crypto::IFileEncrypter

namespace MafiaNet
{
	namespace Experimental
	{
		namespace Crypto
		{
			class Factory
			{
			public:
				static IFileEncrypter* ConstructFileEncrypter(const char *publicKey, size_t publicKeyLength);
				static IFileEncrypter* ConstructFileEncrypter(const char *publicKey, size_t publicKeyLength, const char *privateKey, size_t privateKeyLength, CSecureString& privateKeyPassword);
			};
		}
	}
}