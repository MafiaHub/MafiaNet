/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

// Mostly from http://www.codeproject.com/internet/SendTo.asp and
// Also see http://www.codeguru.com/cpp/i-n/network/messaging/article.php/c5417/

#ifndef __SENDFILETO_H__
#define __SENDFILETO_H__

#include "mafianet/WindowsIncludes.h"
#include <mapi.h>


class CSendFileTo
{
public:
	bool SendMail(HWND hWndParent, const char *strAttachmentFilePath, const char *strAttachmentFileName,const char *strSubject, const char *strBody, const char *strRecipient);
};

#endif
