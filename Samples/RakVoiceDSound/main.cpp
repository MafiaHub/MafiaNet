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

#define INTERACTIVE

#if defined(INTERACTIVE)
#include "slikenet/Kbhit.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <limits> // used for std::numeric_limits
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"

#include "slikenet/sleep.h"
#include "RakVoice.h"
#include "slikenet/statistics.h"
#include "slikenet/GetTime.h"
#include "slikenet/assert.h"
#include "slikenet/Gets.h"
#include "DSoundVoiceAdapter.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

// Reads and writes per second of the sound data
// Speex only supports these 3 values
#define SAMPLE_RATE  (8000)
//#define SAMPLE_RATE  (16000)
//#define SAMPLE_RATE  (32000)

#define FRAMES_PER_BUFFER  (2048 / (32000 / SAMPLE_RATE))

// define sample type. Only short(16 bits sound) is supported at the moment.
typedef short SAMPLE;

MafiaNet::RakPeerInterface *rakPeer= nullptr;
MafiaNet::RakVoice rakVoice;

struct myStat{
	uint64_t time;
	uint64_t bitsRec;
	uint64_t bitsSent;
};

// Keeps a record of the last 20 calls, to give a faster response to traffic change
void LogStats(){
	const int numStats = 20;
	static myStat data[numStats];

	for(int i=0; i<=numStats-2; i++){
		data[i] = data[i+1];
	}

	MafiaNet::RakNetStatistics *rss=rakPeer->GetStatistics(rakPeer->GetSystemAddressFromIndex(0));
	unsigned int currTime = MafiaNet::GetTimeMS();

	data[numStats-1].time = currTime;
	data[numStats-1].bitsSent = BYTES_TO_BITS(rss->runningTotal[MafiaNet::USER_MESSAGE_BYTES_SENT]);
	data[numStats-1].bitsRec = BYTES_TO_BITS(rss->runningTotal[MafiaNet::USER_MESSAGE_BYTES_RECEIVED_PROCESSED]);

	float totalTime = (data[numStats-1].time - data[0].time) / 1000.f ;
	uint64_t totalBitsSent = data[numStats-1].bitsSent - data[0].bitsSent;
	uint64_t totalBitsRec = data[numStats-1].bitsRec - data[0].bitsRec;
	float bpsSent = totalBitsSent/totalTime;
	float bpsRec = totalBitsRec/totalTime;
	float avgBpsSent = static_cast<float>(rss->valueOverLastSecond[MafiaNet::USER_MESSAGE_BYTES_SENT]);
	float avgBpsRec = static_cast<float>(rss->valueOverLastSecond[MafiaNet::USER_MESSAGE_BYTES_RECEIVED_PROCESSED]);

	printf("avgKbpsSent=%02.1f avgKbpsRec=%02.1f kbpsSent=%02.1f kbpsRec=%02.1f    \r", avgBpsSent/1000, avgBpsRec/1000, bpsSent/1000 , bpsRec/1000);
	//printf("MsgBuf=%6i SndBuf=%10i RcvBuf=%10i    \r", rakVoice.GetRakPeerInterface()->GetStatistics(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)->messageSendBuffer[HIGH_PRIORITY], rakVoice.GetBufferedBytesToSend(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS), rakVoice.GetBufferedBytesToReturn(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS));
}

// Prints the current encoder parameters
void PrintParameters(void)
{
	printf("\nComplexity=%3d Noise filter=%3s VAD=%3s VBR=%3s\n"
		,rakVoice.GetEncoderComplexity()
		,(rakVoice.IsNoiseFilterActive()) ? "ON" : "OFF"
		,(rakVoice.IsVADActive()) ? "ON" : "OFF"
		,(rakVoice.IsVBRActive()) ? "ON" : "OFF");
}


//
// Utility function to obtain a Console Window Handle (HWND), as explained in:
// http://support.microsoft.com/kb/124103
//
HWND GetConsoleHwnd(void)
{
#define MY_BUFSIZE 1024 // Buffer size for console window titles.
	HWND hwndFound;         // This is what is returned to the caller.
	TCHAR pszNewWindowTitle[MY_BUFSIZE]; // Contains fabricated
	// WindowTitle.
	TCHAR pszOldWindowTitle[MY_BUFSIZE]; // Contains original
	// WindowTitle.

	// Fetch current window title.
	GetConsoleTitle(pszOldWindowTitle, MY_BUFSIZE);

	// Format a "unique" NewWindowTitle.
	wsprintf(pszNewWindowTitle,TEXT("%d/%d"),
		GetTickCount(),
		GetCurrentProcessId());

	// Change current window title.
	SetConsoleTitle(pszNewWindowTitle);

	// Ensure window title has been updated.
	Sleep(40);

	// Look for NewWindowTitle.
	hwndFound=FindWindow(nullptr, pszNewWindowTitle);

	// Restore original window title.
	SetConsoleTitle(pszOldWindowTitle);

	return(hwndFound);
}

int main(void)
{

	printf("A sample on how to use RakVoice together with DirectSound.\n");
	printf("You need a microphone for this sample.\n");
	printf("RakVoice relies on Speex for voice encoding and decoding.\n");
	printf("See DependentExtensions/RakVoice/speex-1.2beta3 for speex projects.\n");
	printf("For windows, I had to define HAVE_CONFIG_H, include win32/config.h,\n");
	printf("and include the files under libspeex, except those that start with test.\n");
	printf("Difficulty: Advanced\n\n");

	bool mute=false;
	bool quit;
	int ch;

	char port[256];
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
#if defined(INTERACTIVE)
	printf("Enter local port: ");
	Gets(port, sizeof(port));
	if (port[0]==0)
#endif
		strcpy_s(port, "60000");
	const int intLocalPort = atoi(port);
	if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
		printf("Failed. Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
		return -1;
	}
	MafiaNet::SocketDescriptor socketDescriptor(static_cast<unsigned short>(intLocalPort),0);

	rakPeer->Startup(4, &socketDescriptor, 1);

	rakPeer->SetMaximumIncomingConnections(4);
	rakPeer->AttachPlugin(&rakVoice);

	rakVoice.Init(SAMPLE_RATE, FRAMES_PER_BUFFER*sizeof(SAMPLE));

	// Initialize our connection with DirectSound
	if (!MafiaNet::DSoundVoiceAdapter::Instance()->SetupAdapter(&rakVoice, GetConsoleHwnd(), DSSCL_EXCLUSIVE))
	{
		printf("An error occurred while initializing DirectSound.\n");
		exit(-1);
	}

	MafiaNet::Packet *p;
	quit=false;
#if defined(INTERACTIVE)
	printf("(Q)uit. (C)onnect. (D)isconnect. (M)ute. ' ' for stats.\n");
	printf("(+/-)encoder complexity.  (N)oise filter on/off. (V)AD on/off. (B)vbr on/off.\n");
#else
	rakPeer->Connect("1.1.1.1", 60000, 0,0);
#endif
	PrintParameters();
	while (!quit)
	{
#if defined(INTERACTIVE)
		if (_kbhit())
		{
			ch=_getch();
			if (ch=='+'){
				// Increase encoder complexity
				int v = rakVoice.GetEncoderComplexity();
				if (v<10) rakVoice.SetEncoderComplexity(v+1);
				PrintParameters();
			}
			else if (ch=='-'){
				// Decrease encoder complexity
				int v = rakVoice.GetEncoderComplexity();
				if (v>0) rakVoice.SetEncoderComplexity(v-1);
				PrintParameters();
			}
			else if (ch=='n'){
				// Turn on/off noise filter
				rakVoice.SetNoiseFilter(!rakVoice.IsNoiseFilterActive());
				PrintParameters();
			}
			else if (ch=='v') {
				// Turn on/off Voice detection
				rakVoice.SetVAD(!rakVoice.IsVADActive());
				PrintParameters();
			}
			else if (ch=='b') {
				// Turn on/off VBR
				rakVoice.SetVBR(!rakVoice.IsVBRActive());
				PrintParameters();
			}
			else if (ch=='y')
			{
				quit=true;
			}
			else if (ch=='c')
			{
				char ip[256];
				printf("\nEnter IP of remote system: ");
				Gets(ip, sizeof(ip));
				if (ip[0]==0)
					strcpy_s(ip, "127.0.0.1");
				printf("\nEnter port of remote system: ");
				Gets(port, sizeof(port));
				if (port[0]==0)
					strcpy_s(port, "60000");
				const int intRemotePort = atoi(port);
				if ((intRemotePort < 0) || (intRemotePort > std::numeric_limits<unsigned short>::max())) {
					printf("Specified remote port %d is outside valid bounds [0, %u]", intRemotePort, std::numeric_limits<unsigned short>::max());
					return -1;
				}
				rakPeer->Connect(ip, static_cast<unsigned short>(intRemotePort), 0,0);
			}
			else if (ch=='m')
			{
				mute=!mute;
				MafiaNet::DSoundVoiceAdapter::Instance()->SetMute(mute);
				if (mute)
					printf("\nNow muted.\n");
				else
					printf("\nNo longer muted.\n");
			}
			else if (ch=='d')
			{
				rakPeer->Shutdown(100,0);
			}
			else if (ch==' ')
			{
				char message[2048];
				MafiaNet::RakNetStatistics *rss=rakPeer->GetStatistics(rakPeer->GetSystemAddressFromIndex(0));
				StatisticsToString(rss, message, 2048, 2);
				printf("%s", message);
			}
			else if (ch=='q')
				quit=true;
			ch=0;
		}

#endif

		p=rakPeer->Receive();
		while (p)
		{
			if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				printf("\nID_CONNECTION_REQUEST_ACCEPTED from %s\n", p->systemAddress.ToString());
				rakVoice.RequestVoiceChannel(p->guid);
			}
			else if (p->data[0]==ID_RAKVOICE_OPEN_CHANNEL_REQUEST)
			{
				printf("\nOpen Channel request from %s\n", p->systemAddress.ToString());
			}
			else if (p->data[0]==ID_RAKVOICE_OPEN_CHANNEL_REPLY)
			{
				printf("\nGot new channel from %s\n", p->systemAddress.ToString());
			}

			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
		}
		
		// Update our connection with DirectSound
		MafiaNet::DSoundVoiceAdapter::Instance()->Update();

		LogStats();
		RakSleep(20);
	}

	// Release any FMOD resources we used, and shutdown FMOD itself
	MafiaNet::DSoundVoiceAdapter::Instance()->Release();

	rakPeer->Shutdown(300);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);

	return 0;
}
