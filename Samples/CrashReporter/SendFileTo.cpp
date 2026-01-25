/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/WindowsIncludes.h"
#include "SendFileTo.h"
#include <shlwapi.h>
#include <tchar.h>
#include <stdio.h>
#include <direct.h>
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

bool CSendFileTo::SendMail(HWND hWndParent, const char *strAttachmentFilePath, const char *strAttachmentFileName, const char *strSubject, const char *strBody, const char *strRecipient)
{
//	if (strAttachmentFileName==0)
//		return false;

//	if (!hWndParent || !::IsWindow(hWndParent))
//		return false;

	HINSTANCE hMAPI = LoadLibrary(_T("MAPI32.DLL"));
	if (!hMAPI)
		return false;

	ULONG (PASCAL *SendMail)(ULONG, ULONG_PTR, MapiMessage*, FLAGS, ULONG);
	(FARPROC&)SendMail = GetProcAddress(hMAPI, "MAPISendMail");

	if (!SendMail)
		return false;

//	char szFileName[_MAX_PATH];
//	char szPath[_MAX_PATH];
	char szName[_MAX_PATH];
	char szSubject[_MAX_PATH];
	char szBody[_MAX_PATH];
	char szAddress[_MAX_PATH];
	char szSupport[_MAX_PATH];
	//strcpy_s(szFileName, strAttachmentFileName);
	//strcpy_s(szPath, strAttachmentFilePath);
	if (strAttachmentFileName)
		strcpy_s(szName, strAttachmentFileName);
	strcpy_s(szSubject, strSubject);
	strcpy_s(szBody, strBody);
	sprintf_s(szAddress, "SMTP:%s", strRecipient);
	//strcpy_s(szSupport, "Support");

	char fullPath[_MAX_PATH];
	if (strAttachmentFileName && strAttachmentFilePath)
	{
		if (strlen(strAttachmentFilePath)<3 ||
			strAttachmentFilePath[1]!=':' ||
			(strAttachmentFilePath[2]!='\\' && 
			strAttachmentFilePath[2]!='/'))
		{
			// Make relative paths absolute
			_getcwd(fullPath, _MAX_PATH);
			strcat_s(fullPath, "/");
			strcat_s(fullPath, strAttachmentFilePath);
		}
		else
			strcpy_s(fullPath, strAttachmentFilePath);


		// All slashes have to be \\ and not /
		int len=(unsigned int)strlen(fullPath);
		int i;
		for (i=0; i < len; i++)
		{
			if (fullPath[i]=='/')
				fullPath[i]='\\';
		}
	}


	MapiFileDesc fileDesc;
	if (strAttachmentFileName && strAttachmentFilePath)
	{
		ZeroMemory(&fileDesc, sizeof(fileDesc));
		fileDesc.nPosition = (ULONG)-1;
		fileDesc.lpszPathName = fullPath;
		fileDesc.lpszFileName = szName;
	}
	
	MapiRecipDesc recipDesc;
	ZeroMemory(&recipDesc, sizeof(recipDesc));
	recipDesc.lpszName = szSupport;
	recipDesc.ulRecipClass = MAPI_TO;
	recipDesc.lpszName = szAddress+5;
	recipDesc.lpszAddress = szAddress;

	MapiMessage message;
	ZeroMemory(&message, sizeof(message));
	message.nRecipCount = 1;
	message.lpRecips = &recipDesc;
	message.lpszSubject = szSubject;
	message.lpszNoteText = szBody;
	if (strAttachmentFileName && strAttachmentFilePath)
	{
		message.nFileCount = 1;
		message.lpFiles = &fileDesc;
	}
	
	int nError = SendMail(0, (ULONG_PTR)hWndParent, &message, MAPI_LOGON_UI|MAPI_DIALOG, 0);

	if (nError != SUCCESS_SUCCESS && nError != MAPI_USER_ABORT && nError != MAPI_E_LOGIN_FAILURE)
		return false;

	return true;
}
