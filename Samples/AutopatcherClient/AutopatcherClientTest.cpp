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
#include <limits> // used for std::numeric_limits
#include "mafianet/Kbhit.h"

// Platform-specific includes for path constants and process execution
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <limits.h>
#include <unistd.h>
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#define _snprintf snprintf
#define _unlink unlink
#endif

#include "mafianet/GetTime.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/StringCompressor.h"
#include "mafianet/PacketizedTCP.h"

// Client only includes
#include "mafianet/FileListTransferCBInterface.h"
#include "mafianet/FileListTransfer.h"
#include "AutopatcherClient.h"
#include "mafianet/AutopatcherPatchContext.h"
#include "mafianet/Gets.h"
#include "mafianet/sleep.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

char WORKING_DIRECTORY[MAX_PATH];
char PATH_TO_XDELTA_EXE[MAX_PATH];

class TestCB : public MafiaNet::AutopatcherClientCBInterface
{
public:
	virtual bool OnFile(OnFileStruct *onFileStruct)
	{
		if (onFileStruct->context.op==PC_HASH_1_WITH_PATCH || onFileStruct->context.op==PC_HASH_2_WITH_PATCH)
			printf("Patched: ");
		else if (onFileStruct->context.op==PC_WRITE_FILE)
			printf("Written: ");
		else if (onFileStruct->context.op==PC_ERROR_FILE_WRITE_FAILURE)
			printf("Write Failure: ");
		else if (onFileStruct->context.op==PC_ERROR_PATCH_TARGET_MISSING)
			printf("Patch target missing: ");
		else if (onFileStruct->context.op==PC_ERROR_PATCH_APPLICATION_FAILURE)
			printf("Patch process failure: ");
		else if (onFileStruct->context.op==PC_ERROR_PATCH_RESULT_CHECKSUM_FAILURE)
			printf("Patch checksum failure: ");
		else if (onFileStruct->context.op==PC_NOTICE_WILL_COPY_ON_RESTART)
			printf("Copy pending restart: ");
		else if (onFileStruct->context.op==PC_NOTICE_FILE_DOWNLOADED)
			printf("Downloaded: ");
		else if (onFileStruct->context.op==PC_NOTICE_FILE_DOWNLOADED_PATCH)
			printf("Downloaded Patch: ");
		else
			RakAssert(0);


		printf("%i. (100%%) %i/%i %s %ib / %ib\n", onFileStruct->setID, onFileStruct->fileIndex+1, onFileStruct->numberOfFilesInThisSet,
			onFileStruct->fileName, onFileStruct->byteLengthOfThisFile,
			onFileStruct->byteLengthOfThisSet);

		// Return false for the file data to be deallocated automatically
		return false;
	}

	virtual void OnFileProgress(FileProgressStruct *fps)
	{
		printf("Downloading: %i. (%i%%) %i/%i %s %ib/%ib %ib/%ib total\n", fps->onFileStruct->setID,
			(int) (100.0*(double)fps->onFileStruct->bytesDownloadedForThisFile/(double)fps->onFileStruct->byteLengthOfThisFile),
			fps->onFileStruct->fileIndex+1, fps->onFileStruct->numberOfFilesInThisSet, fps->onFileStruct->fileName,
			fps->onFileStruct->bytesDownloadedForThisFile,
			fps->onFileStruct->byteLengthOfThisFile,			
			fps->onFileStruct->bytesDownloadedForThisSet,
			fps->onFileStruct->byteLengthOfThisSet
			);
	}

	virtual PatchContext ApplyPatchBase(const char *oldFilePath, char **newFileContents, unsigned int *newFileSize, char *patchContents, unsigned int patchSize, uint32_t patchAlgorithm)
	{
		if (patchAlgorithm==0)
		{
			return ApplyPatchBSDiff(oldFilePath, newFileContents, newFileSize, patchContents, patchSize);
		}
		else
		{
			char buff[128];
			MafiaNet::TimeUS time = MafiaNet::GetTimeUS();
#if defined(_WIN32)
			sprintf_s(buff, "%I64u", time);
#else
			sprintf_s(buff, "%llu", (long long unsigned int) time);
#endif

			char pathToPatch1[MAX_PATH], pathToPatch2[MAX_PATH];
			sprintf_s(pathToPatch1, "%s/patchClient_%s.tmp", WORKING_DIRECTORY, buff);
			FILE *fpPatch;
			if (fopen_s(&fpPatch, pathToPatch1, "wb")!=0)
				return PC_ERROR_PATCH_TARGET_MISSING;
			fwrite(patchContents, 1, patchSize, fpPatch);
			fclose(fpPatch);

			// Invoke xdelta
			// See https://code.google.com/p/xdelta/wiki/CommandLineSyntax
			char commandLine[512];
#ifdef _WIN32
			_snprintf(commandLine, sizeof(commandLine)-1, "-d -f -s %s patchClient_%s.tmp newFile_%s.tmp", oldFilePath, buff, buff);
			commandLine[511]=0;

			SHELLEXECUTEINFOA shellExecuteInfo;
			shellExecuteInfo.cbSize = sizeof(SHELLEXECUTEINFO);
			shellExecuteInfo.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE;
			shellExecuteInfo.hwnd = nullptr;
			shellExecuteInfo.lpVerb = "open";
			shellExecuteInfo.lpFile = PATH_TO_XDELTA_EXE;
			shellExecuteInfo.lpParameters = commandLine;
			shellExecuteInfo.lpDirectory = WORKING_DIRECTORY;
			shellExecuteInfo.nShow = SW_SHOWNORMAL;
			shellExecuteInfo.hInstApp = nullptr;
			ShellExecuteExA(&shellExecuteInfo);
#else
			// On Unix, use system() to invoke xdelta
			snprintf(commandLine, sizeof(commandLine)-1, "cd \"%s\" && \"%s\" -d -f -s \"%s\" patchClient_%s.tmp newFile_%s.tmp",
				WORKING_DIRECTORY, PATH_TO_XDELTA_EXE, oldFilePath, buff, buff);
			commandLine[511]=0;
			system(commandLine);
#endif

			sprintf_s(pathToPatch2, "%s/newFile_%s.tmp", WORKING_DIRECTORY, buff);
			errno_t error = fopen_s(&fpPatch, pathToPatch2, "r+b");
			MafiaNet::TimeUS stopWaiting = time + 60000000;
			while (error!=0 && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				error = fopen_s(&fpPatch, pathToPatch2, "r+b");
			}
			if (error!=0)
			{
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nERROR: Could not open %s.\nerr=%i (%s)\narguments=%s\n", pathToPatch2, errno, buff2, commandLine);
				return PC_ERROR_PATCH_TARGET_MISSING;
			}
		
			fseek(fpPatch, 0, SEEK_END);
			*newFileSize = ftell(fpPatch);
			fseek(fpPatch, 0, SEEK_SET);
			*newFileContents = (char*) rakMalloc_Ex(*newFileSize, _FILE_AND_LINE_);
			fread(*newFileContents, 1, *newFileSize, fpPatch);
			fclose(fpPatch);

			int unlinkRes1 = _unlink(pathToPatch1);
			int unlinkRes2 = _unlink(pathToPatch2);
			while ((unlinkRes1!=0 || unlinkRes2!=0) && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				if (unlinkRes1!=0)
					unlinkRes1 = _unlink(pathToPatch1);
				if (unlinkRes2!=0)
					unlinkRes2 = _unlink(pathToPatch2);
			}

			if (unlinkRes1!=0) {
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nWARNING: unlink %s failed.\nerr=%i (%s)\n", pathToPatch1, errno, buff2);
			}
			if (unlinkRes2!=0) {
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nWARNING: unlink %s failed.\nerr=%i (%s)\n", pathToPatch2, errno, buff2);
			}

			return PC_WRITE_FILE;
		}
	}

} transferCallback;

#define USE_TCP

int main(int argc, char **argv)
{
	printf("A simple client interface for the advanced autopatcher.\n");
	printf("Use DirectoryDeltaTransfer for a simpler version of an autopatcher.\n");
	printf("Difficulty: Intermediate\n\n");

	printf("Client starting...");
	MafiaNet::SystemAddress serverAddress= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	MafiaNet::AutopatcherClient autopatcherClient;
	MafiaNet::FileListTransfer fileListTransfer;
	autopatcherClient.SetFileListTransferPlugin(&fileListTransfer);
	unsigned short localPort=0;
	if (argc>=6)
	{
		const int intLocalPort = atoi(argv[5]);
		if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
			return 2;
		}
		localPort = static_cast<unsigned short>(intLocalPort);
	}
#ifdef USE_TCP
	MafiaNet::PacketizedTCP packetizedTCP;
	if (packetizedTCP.Start(localPort,1)==false)
	{
		printf("Failed to start TCP. Is the port already in use?");
		return 1;
	}
	packetizedTCP.AttachPlugin(&autopatcherClient);
	packetizedTCP.AttachPlugin(&fileListTransfer);
#else
	MafiaNet::RakPeerInterface *rakPeer;
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::SocketDescriptor socketDescriptor(localPort,0);
	rakPeer->Startup(1,&socketDescriptor, 1);
	// Plugin will send us downloading progress notifications if a file is split to fit under the MTU 10 or more times
	rakPeer->SetSplitMessageProgressInterval(10);
	rakPeer->AttachPlugin(&autopatcherClient);
	rakPeer->AttachPlugin(&fileListTransfer);
#endif
	printf("started\n");
	char buff[512];
	if (argc<2)
	{
		printf("Enter server IP: ");
		Gets(buff,sizeof(buff));
		if (buff[0]==0)
			//strcpy_s(buff, "natpunch.slikesoft.com");
			strcpy_s(buff, "127.0.0.1");
	}
	else
		strcpy_s(buff, argv[1]);

#ifdef USE_TCP
	packetizedTCP.Connect(buff,60000,false);
#else
	rakPeer->Connect(buff, 60000, 0, 0);
#endif

	printf("Connecting...\n");
	char appDir[512];
	if (argc<3)
	{
		printf("Enter application directory: ");
		Gets(appDir,sizeof(appDir));
		if (appDir[0]==0)
		{
			strcpy_s(appDir, "D:/temp2");
		}
	}
	else
		strcpy_s(appDir, argv[2]);
	char appName[512];
	if (argc<4)
	{
		printf("Enter application name: ");
		Gets(appName,sizeof(appName));
		if (appName[0]==0)
			strcpy_s(appName, "TestApp");
	}
	else
		strcpy_s(appName, argv[3]);

	bool patchImmediately=argc>=5 && argv[4][0]=='1';
	
	if (patchImmediately==false)
	{
		printf("Optional: Enter path to xdelta exe: ");
		Gets(PATH_TO_XDELTA_EXE, sizeof(PATH_TO_XDELTA_EXE));
		// https://code.google.com/p/xdelta/downloads/list
		if (PATH_TO_XDELTA_EXE[0]==0)
			strcpy_s(PATH_TO_XDELTA_EXE, "c:/xdelta3-3.0.6-win32.exe");

		if (PATH_TO_XDELTA_EXE[0])
		{
			printf("Enter working directory to store temporary files: ");
			Gets(WORKING_DIRECTORY, sizeof(WORKING_DIRECTORY));
			if (WORKING_DIRECTORY[0]==0)
			{
#ifdef _WIN32
				GetTempPathA(MAX_PATH, WORKING_DIRECTORY);
#else
				const char* tmpdir = getenv("TMPDIR");
				if (tmpdir)
					strcpy_s(WORKING_DIRECTORY, tmpdir);
				else
					strcpy_s(WORKING_DIRECTORY, "/tmp");
#endif
			}
			if (WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='\\' || WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='/')
				WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=0;
		}

		printf("Hit 'q' to quit, 'p' to patch, 'f' to full scan. 'c' to cancel the patch. 'r' to reconnect. 'd' to disconnect.\n");
	}
	else
		printf("Hit 'q' to quit, 'c' to cancel the patch.\n");

	int ch;
	MafiaNet::Packet *p;
	for(;;)
	{
#ifdef USE_TCP
		MafiaNet::SystemAddress notificationAddress;
		notificationAddress=packetizedTCP.HasCompletedConnectionAttempt();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
			serverAddress=notificationAddress;
		}
		notificationAddress=packetizedTCP.HasNewIncomingConnection();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_NEW_INCOMING_CONNECTION\n");
		notificationAddress=packetizedTCP.HasLostConnection();
		if (notificationAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			printf("ID_CONNECTION_LOST\n");


		p=packetizedTCP.Receive();
		while (p)
		{
			if (p->data[0]==ID_AUTOPATCHER_REPOSITORY_FATAL_ERROR)
			{
				char buff2[256];
				MafiaNet::BitStream temp(p->data, p->length, false);
				temp.IgnoreBits(8);
				MafiaNet::StringCompressor::Instance()->DecodeString(buff2, 256, &temp);
				printf("ID_AUTOPATCHER_REPOSITORY_FATAL_ERROR\n");
				printf("%s\n", buff2);
			}
			else if (p->data[0]==ID_AUTOPATCHER_CANNOT_DOWNLOAD_ORIGINAL_UNMODIFIED_FILES)
			{
				printf("ID_AUTOPATCHER_CANNOT_DOWNLOAD_ORIGINAL_UNMODIFIED_FILES\n");
			}			
			else if (p->data[0]==ID_AUTOPATCHER_FINISHED)
			{
				printf("ID_AUTOPATCHER_FINISHED with server time %f\n", autopatcherClient.GetServerDate());
				double srvDate=autopatcherClient.GetServerDate();
				FILE *fp;
				fopen_s(&fp, "srvDate", "wb");
				fwrite(&srvDate,sizeof(double),1,fp);
				fclose(fp);
			}
			else if (p->data[0]==ID_AUTOPATCHER_RESTART_APPLICATION)
				printf("Launch \"AutopatcherClientRestarter.exe autopatcherRestart.txt\"\nQuit this application immediately after to unlock files.\n");

			packetizedTCP.DeallocatePacket(p);
			p=packetizedTCP.Receive();
		}
#else
		p=rakPeer->Receive();
		while (p)
		{
			if (p->data[0]==ID_DISCONNECTION_NOTIFICATION)
				printf("ID_DISCONNECTION_NOTIFICATION\n");
			else if (p->data[0]==ID_CONNECTION_LOST)
				printf("ID_CONNECTION_LOST\n");
			else if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
				serverAddress=p->systemAddress;
			}
			else if (p->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
				printf("ID_CONNECTION_ATTEMPT_FAILED\n");
			else if (p->data[0]==ID_NO_FREE_INCOMING_CONNECTIONS)
				printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
			else if (p->data[0]==ID_AUTOPATCHER_REPOSITORY_FATAL_ERROR)
			{
				char buff[256];
				MafiaNet::BitStream temp(p->data, p->length, false);
				temp.IgnoreBits(8);
				MafiaNet::StringCompressor::Instance()->DecodeString(buff, 256, &temp);
				printf("ID_AUTOPATCHER_REPOSITORY_FATAL_ERROR\n");
				printf("%s\n", buff);
			}
			else if (p->data[0]==ID_AUTOPATCHER_CANNOT_DOWNLOAD_ORIGINAL_UNMODIFIED_FILES)
			{
				printf("ID_AUTOPATCHER_CANNOT_DOWNLOAD_ORIGINAL_UNMODIFIED_FILES\n");
			}
			else if (p->data[0]==ID_AUTOPATCHER_FINISHED)
			{
				printf("ID_AUTOPATCHER_FINISHED with server time %f\n", autopatcherClient.GetServerDate());
				double srvDate=autopatcherClient.GetServerDate();
				FILE *fp;
				fopen_s(&fp, "srvDate", "wb");
				fwrite(&srvDate,sizeof(double),1,fp);
				fclose(fp);
			}
			else if (p->data[0]==ID_AUTOPATCHER_RESTART_APPLICATION)
				printf("Launch \"AutopatcherClientRestarter.exe autopatcherRestart.txt\"\nQuit this application immediately after to unlock files.\n");

			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
		}
#endif

		if (_kbhit())
			ch=_getch();
		else
			ch=0;

		if (ch=='q')
			break;
		else if (ch=='r')
		{
#ifdef USE_TCP
			packetizedTCP.Connect(buff,60000,false);
#else
			rakPeer->Connect(buff, 60000, 0, 0);
#endif
		}
		else if (ch=='d')
		{
#ifdef USE_TCP
			packetizedTCP.CloseConnection(serverAddress);
#else
			rakPeer->CloseConnection(serverAddress, true);
#endif
		}
		else if (ch=='p' || (serverAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS && patchImmediately==true) || ch=='f')
		{
			patchImmediately=false;
			char restartFile[512];
			strcpy_s(restartFile, appDir);
			strcat_s(restartFile, "/autopatcherRestart.txt");

			double lastUpdateDate;
			if (ch=='f')
			{
				lastUpdateDate=0;
			}
			else
			{
				FILE *fp;
				if (fopen_s(&fp, "srvDate", "rb") == 0)
				{
					fread(&lastUpdateDate, sizeof(lastUpdateDate), 1, fp);
					fclose(fp);
				}
				else
					lastUpdateDate=0;
			}

			if (autopatcherClient.PatchApplication(appName, appDir, lastUpdateDate, serverAddress, &transferCallback, restartFile, argv[0]))
			{
				printf("Patching process starting.\n");
			}
			else
			{
				printf("Failed to start patching.\n");
			}
		}
		else if (ch=='c')
		{
			autopatcherClient.Clear();
			printf("Autopatcher cleared.\n");
		}

		RakSleep(30);
	}

	// Dereference so the destructor doesn't crash
	autopatcherClient.SetFileListTransferPlugin(0);

#ifdef USE_TCP
	packetizedTCP.Stop();
#else
	rakPeer->Shutdown(500,0);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
#endif
	return 1;
}
