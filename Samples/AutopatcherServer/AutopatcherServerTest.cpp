/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

// Common includes
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <limits.h>
#include <cerrno>
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#endif

#include "mafianet/Kbhit.h"

#include "mafianet/GetTime.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/StringCompressor.h"
#include "mafianet/FileListTransfer.h"
#include "mafianet/FileList.h" // FLP_Printf
#include "mafianet/PacketizedTCP.h"
#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

// Server only includes
#include "AutopatcherServer.h"
// Replace this repository with your own implementation if you don't want to use PostgreSQL
#include "AutopatcherPostgreRepository.h"

#ifdef _WIN32
#include "mafianet/WindowsIncludes.h" // Sleep
#else
#include <unistd.h> // usleep
#endif

#define USE_TCP
#define LISTEN_PORT 60000
#define MAX_INCOMING_CONNECTIONS 128

char WORKING_DIRECTORY[MAX_PATH];
char PATH_TO_XDELTA_EXE[MAX_PATH];

// The default AutopatcherPostgreRepository2 uses bsdiff which takes too much memory for large files.
// I override MakePatch to use XDelta in this case
class AutopatcherPostgreRepository2_WithXDelta : public MafiaNet::AutopatcherPostgreRepository2
{
	int MakePatch(const char *oldFile, const char *newFile, char **patch, unsigned int *patchLength, int *patchAlgorithm)
	{
		FILE *fpOld;
		fopen_s(&fpOld, oldFile, "rb");
		fseek(fpOld, 0, SEEK_END);
		int contentLengthOld = ftell(fpOld);
		FILE *fpNew;
		fopen_s(&fpNew, newFile, "rb");
		fseek(fpNew, 0, SEEK_END);
		int contentLengthNew = ftell(fpNew);

		if ((contentLengthOld < 33554432 && contentLengthNew < 33554432) || PATH_TO_XDELTA_EXE[0]==0)
		{
			// Use bsdiff, which does a good job but takes a lot of memory based on the size of the file
			*patchAlgorithm=0;
			bool b = MakePatchBSDiff(fpOld, contentLengthOld, fpNew, contentLengthNew, patch, patchLength);
			fclose(fpOld);
			fclose(fpNew);
			return b==false ? -1 : 0;
		}
		else
		{
#ifdef _WIN32
			*patchAlgorithm=1;
			fclose(fpOld);
			fclose(fpNew);

			char buff[128];
			MafiaNet::TimeUS time = MafiaNet::GetTimeUS();
			sprintf_s(buff, "%I64u", time);

			// Invoke xdelta
			// See https://code.google.com/p/xdelta/wiki/CommandLineSyntax
			char commandLine[512];
			_snprintf(commandLine, sizeof(commandLine)-1, "-f -s %s %s patchServer_%s.tmp", oldFile, newFile, buff);
			commandLine[511]=0;

			SHELLEXECUTEINFOA shellExecuteInfo;
			shellExecuteInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
			shellExecuteInfo.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE;
			shellExecuteInfo.hwnd = nullptr;
			shellExecuteInfo.lpVerb = "open";
			shellExecuteInfo.lpFile = PATH_TO_XDELTA_EXE;
			shellExecuteInfo.lpParameters = commandLine;
			shellExecuteInfo.lpDirectory = WORKING_DIRECTORY;
			shellExecuteInfo.nShow = SW_SHOWNORMAL;
			shellExecuteInfo.hInstApp = nullptr;
			ShellExecuteExA(&shellExecuteInfo);
			//ShellExecute(nullptr, "open", PATH_TO_XDELTA_EXE, commandLine, WORKING_DIRECTORY, SW_SHOWNORMAL);

			char pathToPatch[MAX_PATH];
			sprintf_s(pathToPatch, "%s/patchServer_%s.tmp", WORKING_DIRECTORY, buff);
			// r+ instead of r, because I want exclusive access in case xdelta is still working
			FILE *fpPatch;
			errno_t error = fopen_s(&fpPatch, pathToPatch, "r+b");
			MafiaNet::TimeUS stopWaiting = time + 60000000 * 5;
			while (error!=0 && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				error = fopen_s(&fpPatch, pathToPatch, "r+b");
			}
			if (error!=0)
				return 1;
			fseek(fpPatch, 0, SEEK_END);
			*patchLength = ftell(fpPatch);
			fseek(fpPatch, 0, SEEK_SET);
			*patch = (char*) rakMalloc_Ex(*patchLength, _FILE_AND_LINE_);
			fread(*patch, 1, *patchLength, fpPatch);
			fclose(fpPatch);

			int unlinkRes = _unlink(pathToPatch);
			while (unlinkRes!=0 && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				unlinkRes = _unlink(pathToPatch);
			}
			if (unlinkRes!=0) {
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nWARNING: unlink %s failed.\nerr=%i (%s)\n", pathToPatch, errno, buff2);
			}

			return 0;
#else
			// xdelta not supported on non-Windows, use bsdiff for large files
			*patchAlgorithm=0;
			bool b = MakePatchBSDiff(fpOld, contentLengthOld, fpNew, contentLengthNew, patch, patchLength);
			fclose(fpOld);
			fclose(fpNew);
			return b==false ? -1 : 0;
#endif
		}
	}
};

int main(int, char **)
{
	printf("Server starting... ");
	MafiaNet::AutopatcherServer autopatcherServer;
	// MafiaNet::FLP_Printf progressIndicator;
	MafiaNet::FileListTransfer fileListTransfer;
	static const int workerThreadCount=4; // Used for checking patches only
	static const int sqlConnectionObjectCount=32; // Used for both checking patches and downloading
	AutopatcherPostgreRepository2_WithXDelta connectionObject[sqlConnectionObjectCount];
	MafiaNet::AutopatcherRepositoryInterface *connectionObjectAddresses[sqlConnectionObjectCount];
	for (int i=0; i < sqlConnectionObjectCount; i++)
		connectionObjectAddresses[i]=&connectionObject[i];
//	fileListTransfer.AddCallback(&progressIndicator);
	autopatcherServer.SetFileListTransferPlugin(&fileListTransfer);
	// PostgreSQL is fast, so this may not be necessary, or could use fewer threads
	// This is used to read increments of large files concurrently, thereby serving users downloads as other users read from the DB
	fileListTransfer.StartIncrementalReadThreads(sqlConnectionObjectCount);
	autopatcherServer.SetMaxConurrentUsers(MAX_INCOMING_CONNECTIONS); // More users than this get queued up
	MafiaNet::AutopatcherServerLoadNotifier_Printf loadNotifier;
	autopatcherServer.SetLoadManagementCallback(&loadNotifier);
#ifdef USE_TCP
	MafiaNet::PacketizedTCP packetizedTCP;
	if (packetizedTCP.Start(LISTEN_PORT,MAX_INCOMING_CONNECTIONS)==false)
	{
		printf("Failed to start TCP. Is the port already in use?");
		return 1;
	}
	packetizedTCP.AttachPlugin(&autopatcherServer);
	packetizedTCP.AttachPlugin(&fileListTransfer);
#else
	MafiaNet::RakPeerInterface *rakPeer;
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::SocketDescriptor socketDescriptor(LISTEN_PORT,0);
	rakPeer->Startup(MAX_INCOMING_CONNECTIONS,&socketDescriptor, 1);
	rakPeer->SetMaximumIncomingConnections(MAX_INCOMING_CONNECTIONS);
	rakPeer->AttachPlugin(&autopatcherServer);
	rakPeer->AttachPlugin(&fileListTransfer);
#endif
	printf("started.\n");

	printf("Enter database password:\n");
	char connectionString[256],password[128];
	char username[256];
	strcpy_s(username, "postgres");
	gets_s(password);
	if (password[0]==0) strcpy_s(password, "aaaa");
	strcpy_s(connectionString, "user=");
	strcat_s(connectionString, username);
	strcat_s(connectionString, " password=");
	strcat_s(connectionString, password);
	for (int conIdx=0; conIdx < sqlConnectionObjectCount; conIdx++)
	{
		if (connectionObject[conIdx].Connect(connectionString)==false)
		{
			printf("Database connection failed.\n");
			return 1;
		}
	}

	printf("Database connection suceeded.\n");
	printf("Starting threads\n");
	// 4 Worker threads, which is CPU intensive
	// A greater number of SQL connections, which read files incrementally for large downloads
	autopatcherServer.StartThreads(workerThreadCount,sqlConnectionObjectCount, connectionObjectAddresses);
	autopatcherServer.CacheMostRecentPatch(0);
	// autopatcherServer.SetAllowDownloadOfOriginalUnmodifiedFiles(false);
	printf("System ready for connections\n");

#ifdef _WIN32
	// https://code.google.com/p/xdelta/downloads/list
	printf("Optional: Enter path to xdelta.exe: ");
	Gets(PATH_TO_XDELTA_EXE, sizeof(PATH_TO_XDELTA_EXE));
	if (PATH_TO_XDELTA_EXE[0]==0)
		strcpy_s(PATH_TO_XDELTA_EXE, "c:/xdelta3-3.0.6-win32.exe");

	if (PATH_TO_XDELTA_EXE[0])
	{
		printf("Enter working directory to store temporary files: ");
		Gets(WORKING_DIRECTORY, sizeof(WORKING_DIRECTORY));
		if (WORKING_DIRECTORY[0]==0)
			GetTempPathA(MAX_PATH, WORKING_DIRECTORY);
		if (WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='\\' || WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='/')
			WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=0;
	}
#else
	// xdelta not supported on non-Windows platforms
	PATH_TO_XDELTA_EXE[0] = 0;
	WORKING_DIRECTORY[0] = 0;
#endif

	printf("(D)rop database\n(C)reate database.\n(A)dd application\n(U)pdate revision.\n(R)emove application\n(Q)uit\n");

	int ch;
	MafiaNet::Packet *p;
	for(;;)
	{
#ifdef USE_TCP
		MafiaNet::SystemAddress notificationAddress;
		notificationAddress=packetizedTCP.HasCompletedConnectionAttempt();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
		notificationAddress=packetizedTCP.HasNewIncomingConnection();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_NEW_INCOMING_CONNECTION\n");
		notificationAddress=packetizedTCP.HasLostConnection();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_CONNECTION_LOST\n");

		p=packetizedTCP.Receive();
		while (p)
		{
			packetizedTCP.DeallocatePacket(p);
			p=packetizedTCP.Receive();
		}
#else
		p=rakPeer->Receive();
		while (p)
		{
			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
				printf("ID_NEW_INCOMING_CONNECTION\n");
			else if (p->data[0]==ID_DISCONNECTION_NOTIFICATION)
				printf("ID_DISCONNECTION_NOTIFICATION\n");
			else if (p->data[0]==ID_CONNECTION_LOST)
				printf("ID_CONNECTION_LOST\n");

			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
		}
#endif

		if (_kbhit())
		{
			ch=_getch();
			if (ch=='q')
				break;
			else if (ch=='c')
			{
				if (connectionObject[0].CreateAutopatcherTables()==false)
					printf("%s", connectionObject[0].GetLastError());
			}
			else if (ch=='d')
			{
                if (connectionObject[0].DestroyAutopatcherTables()==false)
					printf("%s", connectionObject[0].GetLastError());
			}
			else if (ch=='a')
			{
				printf("Enter application name to add: ");
				char appName[512];
				Gets(appName,sizeof(appName));
				if (appName[0]==0)
					strcpy_s(appName, "TestApp");

				if (connectionObject[0].AddApplication(appName, username)==false)
					printf("%s", connectionObject[0].GetLastError());
				else
					printf("Done\n");
			}
			else if (ch=='r')
			{
				printf("Enter application name to remove: ");
				char appName[512];
				Gets(appName,sizeof(appName));
				if (appName[0]==0)
					strcpy_s(appName, "TestApp");

				if (connectionObject[0].RemoveApplication(appName)==false)
					printf("%s", connectionObject[0].GetLastError());
				else
					printf("Done\n");
			}
			else if (ch=='u')
			{
				printf("Enter application name: ");
				char appName[512];
				Gets(appName,sizeof(appName));
				if (appName[0]==0)
					strcpy_s(appName, "TestApp");

				printf("Enter application directory: ");
				char appDir[512];
				Gets(appDir,sizeof(appDir));
				if (appDir[0]==0)
					strcpy_s(appDir, "D:\\temp");
				
				if (connectionObject[0].UpdateApplicationFiles(appName, appDir, username, 0)==false)
				{
					printf("%s", connectionObject[0].GetLastError());
				}
				else
				{
					printf("Update success.\n");
					autopatcherServer.CacheMostRecentPatch(appName);
				}
			}
		}

		RakSleep(30);
	}


#ifdef USE_TCP
	packetizedTCP.Stop();
#else
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
#endif


return 0;
}
