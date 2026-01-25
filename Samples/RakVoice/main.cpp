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

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <limits> // used for std::numeric_limits
#include "portaudio.h"
#include "mafianet/Kbhit.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"

#include "RakVoice.h"
#include "mafianet/statistics.h"
#include "mafianet/NatPunchthroughClient.h"
#include "mafianet/BitStream.h"
#include "mafianet/Getche.h"
#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

/// To test sending to myself. Also uncomment in RakVoice.cpp
//#define _TEST_LOOPBACK

// RakVoice only works with 16-bits sound data
#define PA_SAMPLE_TYPE  paInt16
typedef short SAMPLE;

// Reads and writes per second of the sound data
// Speex only supports these 3 values
#define SAMPLE_RATE  (8000)
//#define SAMPLE_RATE  (16000)
//#define SAMPLE_RATE  (32000)

MafiaNet::RakPeerInterface *rakPeer;

// I think one buffer has to be full (of samples) before you hear the sound.
// So higher frames per buffer means that there will be a larger latency before you hear the sound
// However, it would lock and unlock the buffer more often, hindering performance.
#define FRAMES_PER_BUFFER  (2048 / (32000 / SAMPLE_RATE))

bool mute;
MafiaNet::RakVoice rakVoice;

// inputBuffer and outputBuffer is an array of SAMPLE of count framesPerBuffer
// A sample is one unit of sound.
// The sample rate is the number of sound samples taken per second
// I think one frame is a set of samples equal to the number of channels.  I'm not sure though or how that sample is arranged.
static int PACallback( void *inputBuffer, void *outputBuffer,
						  unsigned long framesPerBuffer,
						  PaTimestamp outTime, void *userData )
{
	// unused parameters
	(void)framesPerBuffer;
	(void)outTime;
	(void)userData;

	if (inputBuffer && !mute)
	{
		// TODO - if the input data is mostly silence, don't send and save the bandwidth.

#ifndef _TEST_LOOPBACK
		unsigned i;

		for (i=0; i < rakPeer->GetMaximumNumberOfPeers(); i++)
		{
			rakVoice.SendFrame(rakPeer->GetGUIDFromIndex(i), inputBuffer);
		}
#else
		rakVoice.SendFrame(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, inputBuffer);
#endif
	}

	rakVoice.ReceiveFrame(outputBuffer);

	return 0;
}

#define NAT_PUNCHTHROUGH_FACILITATOR_PORT 61111

int main(void)
{
	PortAudioStream *stream;
	PaError    err;
	mute=false;

	bool quit;
	int ch;

	printf("A sample on how to use RakVoice. You need a microphone for this sample.\n");
	printf("RakVoice relies on Speex for voice encoding and decoding.\n");
	printf("See DependentExtensions/RakVoice/speex-1.1.12 for speex projects.\n");
	printf("For windows, I had to define HAVE_CONFIG_H, include win32/config.h,\n");
	printf("and include the files under libspeex, except those that start with test.\n");
	printf("PortAudio is also included and is used to read and write audio data.  You\n");
	printf("can substitute whatever you want if you do not want to use portaudio.\n");
	printf("Difficulty: Advanced\n\n");

	// Since voice is peer to peer, we give the option to use the nat punchthrough client if desired.
	MafiaNet::NatPunchthroughClient natPunchthroughClient;
	char port[256];
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	printf("Enter local port (enter for default): ");
	Gets(port, sizeof(port));
	if (port[0]==0)
		strcpy_s(port, "60000");
	const int intLocalPort = atoi(port);
	if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
		return 1;
	}
	MafiaNet::SocketDescriptor socketDescriptor(static_cast<unsigned short>(intLocalPort), 0);
	rakPeer->Startup(4, &socketDescriptor, 1);
	rakPeer->SetMaximumIncomingConnections(4);
	rakPeer->AttachPlugin(&rakVoice);
	rakPeer->AttachPlugin(&natPunchthroughClient);
	rakVoice.Init(SAMPLE_RATE, FRAMES_PER_BUFFER*sizeof(SAMPLE));

	err = Pa_Initialize();
	if( err != paNoError ) goto error;
	
	err = Pa_OpenStream(
		&stream,
		Pa_GetDefaultInputDeviceID(),
		1, // Num channels, whatever that means
		PA_SAMPLE_TYPE,
		nullptr,
		Pa_GetDefaultOutputDeviceID(),
		1, // Num channels
		PA_SAMPLE_TYPE,
		nullptr,
		SAMPLE_RATE,
		FRAMES_PER_BUFFER,            /* frames per buffer */
		0,               /* number of buffers, if zero then use default minimum */
		0, /* paDitherOff, // flags */
		PACallback,
		0 );

	if( err != paNoError ) goto error;

	err = Pa_StartStream( stream );
	if( err != paNoError ) goto error;

	printf("Support NAT punchthrough? (y/n)? ");
	bool useNatPunchthrough;
	useNatPunchthrough=(_getche()=='y');
	printf("\n");
	char facilitatorIP[256];
	{//Linux fix. Won't compile without it. Because of the goto error above, the scope is ambigious. Make it a block to define that it will not be used after the jump.
	//Doesn't change current logic
		MafiaNet::SystemAddress facilitator;
	if (useNatPunchthrough)
	{
		printf("My GUID is %s\n", rakPeer->GetGuidFromSystemAddress(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS).ToString());

		printf("Enter IP of facilitator (enter for default): ");
		Gets(facilitatorIP,sizeof(facilitatorIP));
		if (facilitatorIP[0]==0)
			strcpy_s(facilitatorIP, "natpunch.slikesoft.com");
		facilitator.FromString(facilitatorIP);
		facilitator.SetPortHostOrder(NAT_PUNCHTHROUGH_FACILITATOR_PORT);
		rakPeer->Connect(facilitatorIP, NAT_PUNCHTHROUGH_FACILITATOR_PORT, 0, 0);
		printf("Connecting to facilitator...\n");
	}
	else
	{
		printf("Not supporting NAT punchthrough.\n");
	}


    
	MafiaNet::Packet *p;
	quit=false;
	if (useNatPunchthrough==false)
		printf("(Q)uit. (C)onnect. (D)isconnect. C(l)ose voice channels. (M)ute. ' ' for stats.\n");

	while (!quit)
	{
		if (_kbhit())
		{
			ch=_getch();
			if (ch=='y')
			{
				quit=true;
			}
			else if (ch=='c')
			{
				if (useNatPunchthrough)
				{
					MafiaNet::RakNetGUID destination;
					printf("Enter GUID of destination: ");
					char guidStr[256];
					for(;;)
					{
						Gets(guidStr,sizeof(guidStr));
						if (!destination.FromString(guidStr))
							printf("Invalid GUID format. Try again.\nEnter GUID of destination: ");
						else
							break;
					}
					printf("Starting NAT punch. Please wait...\n");
					natPunchthroughClient.OpenNAT(destination,facilitator);
				}
				else
				{
					char ip[256];
					printf("Enter IP of remote system: ");
					Gets(ip, sizeof(ip));
					if (ip[0]==0)
						strcpy_s(ip, "127.0.0.1");
					printf("Enter port of remote system: ");
					Gets(port, sizeof(port));
					if (port[0]==0)
						strcpy_s(port, "60000");
					const int intRemotePort = atoi(port);
					if ((intRemotePort < 0) || (intRemotePort > std::numeric_limits<unsigned short>::max())) {
						printf("Specified remote port %d is outside valid bounds [0, %u]", intRemotePort, std::numeric_limits<unsigned short>::max());
						return 2;
					}
					rakPeer->Connect(ip, static_cast<unsigned short>(intRemotePort), 0,0);

				}
			}
			else if (ch=='m')
			{
				mute=!mute;
				if (mute)
					printf("Now muted.\n");
				else
					printf("No longer muted.\n");
			}
			else if (ch=='d')
			{
				rakPeer->Shutdown(100,0);
			}
			else if (ch=='l')
			{
				rakVoice.CloseAllChannels();
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

		p=rakPeer->Receive();
		while (p)
		{
			if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				if (p->systemAddress==facilitator)
				{
					printf("Connection to facilitator completed\n");
					printf("(Q)uit. (C)onnect. (D)isconnect. (M)ute. ' ' for stats.\n");
				}
				else
				{
					printf("ID_CONNECTION_REQUEST_ACCEPTED from %s\n", p->systemAddress.ToString());
					rakVoice.RequestVoiceChannel(p->guid);
				}
			}
			else if (p->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
			{
				if (p->systemAddress==facilitator)
				{
					printf("Connection to facilitator failed. Using direct connections\n");
					useNatPunchthrough=false;
					printf("(Q)uit. (C)onnect. (D)isconnect. (M)ute. ' ' for stats.\n");
				}
				else
				{
					printf("ID_CONNECTION_ATTEMPT_FAILED\n");
				}
			}
			else if (p->data[0]==ID_RAKVOICE_OPEN_CHANNEL_REQUEST || p->data[0]==ID_RAKVOICE_OPEN_CHANNEL_REPLY)
			{
				printf("Got new channel from %s\n", p->systemAddress.ToString());
			}
			else if (p->data[0]==ID_NAT_TARGET_NOT_CONNECTED)
			{
				MafiaNet::RakNetGUID g;
				MafiaNet::BitStream b(p->data, p->length, false);
				b.IgnoreBits(8); // Ignore the ID_...
				b.Read(g);
				printf("ID_NAT_TARGET_NOT_CONNECTED for %s\n", g.ToString());
			}
			else if (p->data[0]==ID_NAT_TARGET_UNRESPONSIVE)
			{
				MafiaNet::RakNetGUID g;
				MafiaNet::BitStream b(p->data, p->length, false);
				b.IgnoreBits(8); // Ignore the ID_...
				b.Read(g);
				printf("ID_NAT_TARGET_UNRESPONSIVE for %s\n", g.ToString());
			}
			else if (p->data[0]==ID_NAT_CONNECTION_TO_TARGET_LOST)
			{
				MafiaNet::RakNetGUID g;
				MafiaNet::BitStream b(p->data, p->length, false);
				b.IgnoreBits(8); // Ignore the ID_...
				b.Read(g);
				printf("ID_NAT_CONNECTION_TO_TARGET_LOST for %s\n", g.ToString());
			}
			else if (p->data[0]==ID_NAT_ALREADY_IN_PROGRESS)
			{
				MafiaNet::RakNetGUID g;
				MafiaNet::BitStream b(p->data, p->length, false);
				b.IgnoreBits(8); // Ignore the ID_...
				b.Read(g);
				printf("ID_NAT_ALREADY_IN_PROGRESS for %s\n", g.ToString());
			}
			else if (p->data[0]==ID_NAT_PUNCHTHROUGH_FAILED)
			{
				printf("ID_NAT_PUNCHTHROUGH_FAILED for %s\n", p->guid.ToString());
			}
			else if (p->data[0]==ID_NAT_PUNCHTHROUGH_SUCCEEDED)
			{
				printf("ID_NAT_PUNCHTHROUGH_SUCCEEDED for %s. Connecting...\n", p->guid.ToString());
				rakPeer->Connect(p->systemAddress.ToString(false),p->systemAddress.GetPort(),0,0);
			}
			else if (p->data[0]==ID_ALREADY_CONNECTED)
			{
				printf("ID_ALREADY_CONNECTED\n");
			}
			else if (p->data[0]==ID_RAKVOICE_CLOSE_CHANNEL)
			{
				printf("ID_RAKVOICE_CLOSE_CHANNEL\n");
			}
			else if (p->data[0]==ID_DISCONNECTION_NOTIFICATION)
			{
				printf("ID_DISCONNECTION_NOTIFICATION\n");
			}
			else if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
			{
				printf("ID_NEW_INCOMING_CONNECTION\n");
			}
			else
			{
				printf("Unknown packet ID %i\n", p->data[0]);
			}


			rakPeer->DeallocatePacket(p);
			p=rakPeer->Receive();
		}


		Pa_Sleep( 30 );
	}
	}

	err = Pa_CloseStream( stream );
	if( err != paNoError ) goto error;

	Pa_Terminate();

	rakPeer->Shutdown(300);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);

	return 0;

error:
	Pa_Terminate();
	fprintf( stderr, "An error occured while using the portaudio stream\n" );
	fprintf( stderr, "Error number: %d\n", err );
	fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
	return -1;
}
