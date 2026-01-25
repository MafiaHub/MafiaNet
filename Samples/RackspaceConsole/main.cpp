/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2020, SLikeSoft UG (haftungsbeschrÃ¤nkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/Rackspace.h"
#include <stdio.h>
#include "mafianet/Gets.h"
#include "mafianet/Rackspace.h"
#include "mafianet/TCPInterface.h"
#include "mafianet/Kbhit.h"
#include "mafianet/sleep.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

struct CommandAndDescription
{
	const char *command;
	const char *description;
};

CommandAndDescription commands[]=
{
	{"Help","This screen"},
	{"Quit","Exit application"},
	{"Authenticate","desc"},
	{"ListServers","desc"},
	{"ListServersWithDetails","desc"},
	{"CreateServer","desc"},
	{"GetServerDetails","desc"},
	{"UpdateServerNameOrPassword","desc"},
	{"DeleteServer","desc"},
	{"ListServerAddresses","desc"},
	{"ShareServerAddress","desc"},
	{"DeleteServerAddress","desc"},
	{"RebootServer","desc"},
	{"RebuildServer","desc"},
	{"ResizeServer","desc"},
	{"ConfirmResizedServer","desc"},
	{"RevertResizedServer","desc"},
	{"ListFlavors","desc"},
	{"GetFlavorDetails","desc"},
	{"ListImages","desc"},
	{"CreateImage","desc"},
	{"GetImageDetails","desc"},
	{"DeleteImage","desc"},
	{"ListSharedIPGroups","desc"},
	{"ListSharedIPGroupsWithDetails","desc"},
	{"CreateSharedIPGroup","desc"},
	{"GetSharedIPGroupDetails","desc"},
	{"DeleteSharedIPGroup","desc"},

};

void PrintCommands(void)
{
	printf("\n");

	unsigned int i;
	for (i=0; i < sizeof(commands) / sizeof(CommandAndDescription); i++)
	{
		printf("%s - %s\n", commands[i].command, commands[i].description);
	}

	printf("\n");
}

bool Authenticate(MafiaNet::Rackspace *rackspaceApi, MafiaNet::TCPInterface *tcpInterface)
{

	MafiaNet::SystemAddress serverAddress;
	char authenticationURL[128];
	char rackspaceCloudUsername[128];
	char apiAccessKey[128];

	// See http://docs.rackspacecloud.com/servers/api/v1.0/cs-devguide-20110112.pdf
	// US-based accounts authenticate through auth.api.rackspacecloud.com
	// UK-based accounts authenticate through lon.auth.api.rackspacecloud.com
	printf("Enter authenticationURL: ");
	Gets(authenticationURL, sizeof(authenticationURL));
	if (authenticationURL[0]==0)
	{
		FILE *fp;
		if (fopen_s(&fp, "authenticationURL.txt", "rb") == 0)
		{
			fgets(authenticationURL,128,fp);
			fclose(fp);
			printf("Using %s\n", authenticationURL);
		}
		else
		{
			printf("Can't open authenticationURL.txt");
			return false;
		}
	}

	printf("Enter rackspaceCloudUsername: ");
	Gets(rackspaceCloudUsername, sizeof(rackspaceCloudUsername));
	if (rackspaceCloudUsername[0]==0)
	{
		FILE *fp;
		if (fopen_s(&fp, "rackspaceCloudUsername.txt", "rb") == 0)
		{
			fgets(rackspaceCloudUsername,128,fp);
			fclose(fp);
			printf("Using %s\n", rackspaceCloudUsername);
		}
		else
		{
			printf("Can't open rackspaceCloudUsername.txt");
			return false;
		}
	}

	// This is found when you log in, under Your Account, expanded to API Access. Then click Show Key under Enable API Access.
	printf("Enter apiAccessKey: ");
	Gets(apiAccessKey, sizeof(apiAccessKey));
	if (apiAccessKey[0]==0)
	{
		FILE *fp;
		if (fopen_s(&fp, "apiAccessKey.txt", "rb") == 0)
		{
			fgets(apiAccessKey,128,fp);
			fclose(fp);
			printf("Using %s\n", apiAccessKey);
		}
		else
		{
			printf("Can't open apiAccessKey.txt");
			return false;
		}
	}

	serverAddress=rackspaceApi->Authenticate(tcpInterface, authenticationURL, rackspaceCloudUsername, apiAccessKey);
	if (serverAddress== MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		printf("Failed to connect to %s\n", authenticationURL);

	return true;
}

class DisplayHTMLPage : public MafiaNet::RackspaceEventCallback_Default
{
	virtual void ExecuteDefault(const char *callbackName, MafiaNet::RackspaceEventType eventType, const char *htmlAdditionalInfo)
	{
		/*
		MafiaNet::RakString fileName("%s.html", callbackName);
		FILE *fp;
		if (fopen_s(&fp, fileName.C_String(), "wb") == 0)
		{
			fwrite(htmlAdditionalInfo,1,strlen(htmlAdditionalInfo),fp);
			fclose(fp);
			TCHAR szDirectory[MAX_PATH] = "";
			GetCurrentDirectory(sizeof(szDirectory) - 1, szDirectory);
			ShellExecute(nullptr, "open", fileName.C_String(), nullptr, szDirectory, SW_SHOWNORMAL);
		}
		*/
		printf("%s completed with result %s\n", callbackName, MafiaNet::Rackspace::EventTypeToString(eventType));
		printf(htmlAdditionalInfo);
		printf("\n");
	}
	virtual void OnConnectionAttemptFailure(MafiaNet::RackspaceOperationType operationType, const char *url) {printf("OnConnectionAttemptFailure\noperationType=%i\n%s\n", operationType,url);}
};

int main()
{
#if OPEN_SSL_CLIENT_SUPPORT!=1
	printf("RakNet must be built with OPEN_SSL_CLIENT_SUPPORT");
	return 1;
#else

	MafiaNet::Rackspace rackspaceApi;
	MafiaNet::TCPInterface tcpInterface;

	DisplayHTMLPage callback;
	rackspaceApi.AddEventCallback(&callback);

	tcpInterface.Start(0, 0, 1);

	if (!Authenticate(&rackspaceApi, &tcpInterface))
	{
		return 1;
	}
	//rackspaceApi.ListImages();

	PrintCommands();

	MafiaNet::SystemAddress systemAddress;
	MafiaNet::Packet *packet;
	for(;;)
	{
		for (packet=tcpInterface.Receive(); packet; tcpInterface.DeallocatePacket(packet), packet=tcpInterface.Receive())
		{
			rackspaceApi.OnReceive(packet);
		}

		MafiaNet::SystemAddress lostConnectionAddress = tcpInterface.HasLostConnection();
		if (lostConnectionAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
			rackspaceApi.OnClosedConnection(lostConnectionAddress);

		if (_kbhit())
		{
			printf("Command: ");

			char command[128];
			Gets(command, sizeof(command));

			if (_stricmp(command,"Help")==0)
			{
				PrintCommands();
			}
			else if (_stricmp(command,"Quit")==0)
			{
				break;
			}
			else if (_stricmp(command,"Authenticate")==0)
			{
				Authenticate(&rackspaceApi, &tcpInterface);
			}
			else if (_stricmp(command,"ListServers")==0)
			{
				rackspaceApi.ListServers();
			}
			else if (_stricmp(command,"ListServersWithDetails")==0)
			{
				rackspaceApi.ListServersWithDetails();
			}
			else if (_stricmp(command,"CreateServer")==0)
			{
				MafiaNet::RakString name;
				printf("Enter server name: ");
				Gets(command, sizeof(command));
				name=command;
				MafiaNet::RakString imageId;
				printf("Enter imageId: ");
				Gets(command, sizeof(command));
				imageId=command;
				MafiaNet::RakString flavorId;
				printf("Enter flavorId: ");
				Gets(command, sizeof(command));
				flavorId=command;

				rackspaceApi.CreateServer(name, imageId, flavorId);
			}
			else if (_stricmp(command,"GetServerDetails")==0)
			{
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				rackspaceApi.GetServerDetails(command);
			}
			else if (_stricmp(command,"UpdateServerNameOrPassword")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString newName;
				printf("Enter newName: ");
				Gets(command, sizeof(command));
				newName=command;
				MafiaNet::RakString newPassword;
				printf("Enter newPassword: ");
				Gets(command, sizeof(command));
				newPassword=command;

				rackspaceApi.UpdateServerNameOrPassword(serverId,newName,newPassword);
			}
			else if (_stricmp(command,"DeleteServer")==0)
			{
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				rackspaceApi.DeleteServer(command);
			}
			else if (_stricmp(command,"ListServerAddresses")==0)
			{
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				rackspaceApi.ListServerAddresses(command);
			}
			else if (_stricmp(command,"ShareServerAddress")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString serverAddress;
				printf("Enter server serverAddress: ");
				Gets(command, sizeof(command));
				serverAddress=command;
				rackspaceApi.ShareServerAddress(serverId, serverAddress);
			}
			else if (_stricmp(command,"DeleteServerAddress")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString serverAddress;
				printf("Enter server serverAddress: ");
				Gets(command, sizeof(command));
				serverAddress=command;
				rackspaceApi.DeleteServerAddress(serverId, serverAddress);
			}
			else if (_stricmp(command,"RebootServer")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString rebootType;
				printf("Enter rebootType: ");
				Gets(command, sizeof(command));
				rebootType=command;
				rackspaceApi.RebootServer(serverId,rebootType);
			}
			else if (_stricmp(command,"RebuildServer")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString imageId;
				printf("Enter imageId: ");
				Gets(command, sizeof(command));
				imageId=command;
				rackspaceApi.RebuildServer(serverId,imageId);
			}
			else if (_stricmp(command,"ResizeServer")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString flavorId;
				printf("Enter flavorId: ");
				Gets(command, sizeof(command));
				flavorId=command;
				rackspaceApi.ResizeServer(serverId,flavorId);
			}
			else if (_stricmp(command,"ConfirmResizedServer")==0)
			{
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				rackspaceApi.ConfirmResizedServer(command);
			}
			else if (_stricmp(command,"RevertResizedServer")==0)
			{
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				rackspaceApi.RevertResizedServer(command);
			}
			else if (_stricmp(command,"ListFlavors")==0)
			{
				rackspaceApi.ListFlavors();
			}
			else if (_stricmp(command,"GetFlavorDetails")==0)
			{
				printf("Enter flavor id: ");
				Gets(command, sizeof(command));
				rackspaceApi.GetFlavorDetails(command);
			}
			else if (_stricmp(command,"ListImages")==0)
			{
				rackspaceApi.ListImages();
			}
			else if (_stricmp(command,"CreateImage")==0)
			{
				MafiaNet::RakString serverId;
				printf("Enter server id: ");
				Gets(command, sizeof(command));
				serverId=command;
				MafiaNet::RakString imageName;
				printf("Enter imageName: ");
				Gets(command, sizeof(command));
				imageName=command;
				rackspaceApi.CreateImage(serverId,imageName);
			}
			else if (_stricmp(command,"GetImageDetails")==0)
			{
				printf("Enter image id: ");
				Gets(command, sizeof(command));
				rackspaceApi.GetImageDetails(command);
			}
			else if (_stricmp(command,"DeleteImage")==0)
			{
				printf("Enter image id: ");
				Gets(command, sizeof(command));
				rackspaceApi.DeleteImage(command);
			}
			else if (_stricmp(command,"ListSharedIPGroups")==0)
			{
				rackspaceApi.ListSharedIPGroups();
			}
			else if (_stricmp(command,"ListSharedIPGroupsWithDetails")==0)
			{
				rackspaceApi.ListSharedIPGroupsWithDetails();
			}
			else if (_stricmp(command,"CreateSharedIPGroup")==0)
			{
				rackspaceApi.CreateSharedIPGroup("testSharedIPGroup","");
			}
			else if (_stricmp(command,"GetSharedIPGroupDetails")==0)
			{
				printf("Enter group id: ");
				Gets(command, sizeof(command));
				rackspaceApi.GetSharedIPGroupDetails(command);
			}
			else if (_stricmp(command,"DeleteSharedIPGroup")==0)
			{
				printf("Enter group id: ");
				Gets(command, sizeof(command));
				rackspaceApi.DeleteSharedIPGroup(command);
			}
			else
			{
				printf("Unknown command. Type 'help' for help.\n");
			}
		}

		RakSleep(30);
	}

	return 0;
#endif
}
