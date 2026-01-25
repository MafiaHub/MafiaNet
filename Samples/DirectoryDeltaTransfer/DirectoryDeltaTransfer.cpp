/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2024, MafiaHub
 *
 *  This source code was modified by MafiaHub. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/GetTime.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/statistics.h"
#include "mafianet/DirectoryDeltaTransfer.h"
#include "mafianet/FileListTransfer.h"
#include "mafianet/FileList.h"
#include "mafianet/DataCompressor.h"
#include "mafianet/FileListTransferCBInterface.h"
#include "mafianet/Gets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h> // Sleep
#include <conio.h>
#else
#include <unistd.h> // usleep
#include "mafianet/Kbhit.h"
#endif

using namespace MafiaNet;

class TestCB : public FileListTransferCBInterface
{
public:
	bool OnFile(OnFileStruct *onFileStruct) override
	{
		printf("%i. %i/%i %s %llub / %ub\n",
			onFileStruct->setID,
			onFileStruct->fileIndex,
			onFileStruct->numberOfFilesInThisSet,
			onFileStruct->fileName,
			(unsigned long long)onFileStruct->byteLengthOfThisFile,
			onFileStruct->byteLengthOfThisSet);
		return true;
	}
	void OnFileProgress(FileProgressStruct *fps) override
	{
		// Progress callback - do nothing for this sample
		(void)fps;
	}
} transferCallback;


int main(void)
{
	char ch;
	RakPeerInterface *rakPeer;

	// directoryDeltaTransfer is the main plugin that does the work for this sample.
	DirectoryDeltaTransfer directoryDeltaTransfer;
	// The fileListTransfer plugin is used by the DirectoryDeltaTransfer plugin and must also be registered (you could use this yourself too if you wanted, of course).
	FileListTransfer fileListTransfer;

	rakPeer = RakPeerInterface::GetInstance();
	rakPeer->AttachPlugin(&directoryDeltaTransfer);
	rakPeer->AttachPlugin(&fileListTransfer);
	directoryDeltaTransfer.SetFileListTransferPlugin(&fileListTransfer);

	printf("This sample demonstrates the plugin to incrementally transfer compressed\n");
	printf("deltas of directories.  In essence, it's a simple autopatcher.\n");
	printf("Unlike the full autopatcher, it has no dependencies.  It is suitable for\n");
	printf("patching from non-dedicated servers at runtime.\n");
	printf("Difficulty: Intermediate\n\n");

	printf("Enter listen port, or hit enter to choose automatically\n");
	unsigned short localPort;
	char str[256];
	Gets(str, sizeof(str));
	if (str[0]==0)
		localPort=60000;
	else
		localPort=atoi(str);
	SocketDescriptor sd(localPort, 0);
	if (rakPeer->Startup(8, &sd, 1) != RAKNET_STARTED)
	{
		RakPeerInterface::DestroyInstance(rakPeer);
		printf("MafiaNet startup failed. Possibly duplicate port.\n");
		return 1;
	}
	rakPeer->SetMaximumIncomingConnections(8);

	printf("Commands:\n");
	printf("(S)et application directory.\n");
	printf("(A)dd allowed uploads from subdirectory.\n");
	printf("(D)ownload from subdirectory.\n");
	printf("(C)lear allowed uploads.\n");
	printf("C(o)nnect to another system.\n");
	printf("(Q)uit.\n");

	Packet *p;
	while (1)
	{
		// Process packets
		p=rakPeer->Receive();
		while (p)
		{
			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
				printf("ID_NEW_INCOMING_CONNECTION\n");
			else if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");

			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
		}
		

		if (_kbhit())
		{
			ch=_getch();
			if (ch=='s')
			{
				printf("Enter application directory\n");
				Gets(str, sizeof(str));
				if (str[0]==0)
					strcpy(str, "C:/RakNet");
				directoryDeltaTransfer.SetApplicationDirectory(str);
				printf("This directory will be prefixed to upload and download subdirectories.\n");
			}
			else if (ch=='a')
			{
				printf("Enter uploads subdirectory\n");
				Gets(str, sizeof(str));
				directoryDeltaTransfer.AddUploadsFromSubdirectory(str);
				printf("%i files for upload.\n", directoryDeltaTransfer.GetNumberOfFilesForUpload());
			}
			else if (ch=='d')
			{
				char subdir[256];
				char outputSubdir[256];
				printf("Enter remote subdirectory to download from.\n");
				printf("This directory may be any uploaded directory, or a subdir therein.\n");
				Gets(subdir, sizeof(subdir));
				printf("Enter subdirectory to output to.\n");
				Gets(outputSubdir, sizeof(outputSubdir));
                
				unsigned short setId;
				setId=directoryDeltaTransfer.DownloadFromSubdirectory(subdir, outputSubdir, true, rakPeer->GetSystemAddressFromIndex(0), &transferCallback, HIGH_PRIORITY, 0, nullptr);
				if (setId==(unsigned short)-1)
					printf("Download failed.  Host unreachable.\n");
				else
					printf("Downloading set %i\n", setId);
			}
			else if (ch=='c')
			{
				directoryDeltaTransfer.ClearUploads();
				printf("Uploads cleared.\n");
			}
			else if (ch=='o')
			{
				char host[256];
				printf("Enter host IP: ");
				Gets(host, sizeof(host));
				if (host[0]==0)
					strcpy(host, "127.0.0.1");
				unsigned short remotePort;
				printf("Enter host port: ");
				Gets(str, sizeof(str));
				if (str[0]==0)
					remotePort=60000;
				else
					remotePort=atoi(str);
				rakPeer->Connect(host, remotePort, 0, 0);
				printf("Connecting.\n");
			}
			else if (ch=='q')
			{
				printf("Bye!\n");
				break;
			}
		}
	}

	RakPeerInterface::DestroyInstance(rakPeer);

	return 0;
}
