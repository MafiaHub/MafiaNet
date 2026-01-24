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

// How to patch
// 1. Create a new server instance using the most recent image (optional)
// 2. Connect to the new (or any existing) instance through remote desktop. Immediately set max concurrent users to 0. Wait for users to finish patching, if any started.
// 3. Update the patch
// 4. Image the server
// 5. Wait for the image to complete (press l to check, or use Rackspace control panel. Takes like half an hour).
// 6. When the image completes, set the image spawn from the 'l' option to the image that was created in step 4
// 6. Send terminate command. Servers running the old patch will shutdown automatically.
// 9. Set max concurrent users back above 0.
// 10. Optional: Manually call SpawnServers() if load is immediately anticipated

// Common includes
#include <stdio.h>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "slikenet/Kbhit.h"

#include "slikenet/GetTime.h"
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/BitStream.h"
#include "slikenet/StringCompressor.h"
#include "slikenet/FileListTransfer.h"
#include "slikenet/FileList.h" // FLP_Printf
#include "slikenet/PacketizedTCP.h"
#include "slikenet/Gets.h"
#include "CloudServerHelper.h"
#include "slikenet/FullyConnectedMesh2.h"
#include "slikenet/TwoWayAuthentication.h"
#include "slikenet/CloudClient.h"
#include "slikenet/DynDNS.h"
#include "slikenet/peerinterface.h"
#include "slikenet/sleep.h"
#include "slikenet/ConnectionGraph2.h"
#include "CloudServerHelper.h"
#include "slikenet/HTTPConnection2.h"
#include "Rackspace2.h"
#include "slikenet/GetTime.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"
// See http://www.digip.org/jansson/doc/2.4/
// This is used to make it easier to parse the JSON returned from the master server
#include "jansson.h"

// Server only includes
#include "AutopatcherServer.h"
// Replace this repository with your own implementation if you don't want to use PostgreSQL
#include "AutopatcherPostgreRepository.h"

#ifdef _WIN32
#include "slikenet/WindowsIncludes.h" // Sleep
#else
#include <unistd.h> // usleep
#endif

#define LISTEN_PORT_TCP_PATCHER 60000

using namespace MafiaNet;
static const MafiaNet::Time LOAD_CHECK_INTERVAL=1000*60*5;
static const MafiaNet::Time LOAD_CHECK_INTERVAL_AFTER_SPAWN=1000*60*60;
char databasePassword[128];
int workerThreadCount;
int sqlConnectionObjectCount;
int allowDownloadingUnmodifiedFiles;

unsigned short autopatcherLoad=0;

MafiaNet::RakPeerInterface *g_rakPeer;
MafiaNet::AutopatcherServer *autopatcherServer;

enum AppState
{
	AP_RUNNING,
	AP_TERMINATE_WHEN_ZERO_USERS_REACHED,
	AP_TERMINATE_IF_NOT_DNS_HOST,
	AP_TERMINATING,
	AP_TERMINATED,
} appState;

MafiaNet::Time timeSinceZeroUsers;


struct CloudServerHelper_RackspaceCloudDNS : public CloudServerHelper, public Rackspace2EventCallback
{
public:

	enum CSHState
	{
		AUTHENTICATING,
		AUTHENTICATED,
		CANNOT_FIND_PATCHER_DOMAIN,
		CANNOT_FIND_PATCHER_RECORD,
		GETTING_SERVERS,
		GOT_MY_SERVER,
		CANNOT_FIND_MY_SERVER,
		GETTING_DOMAINS,
		GOT_DOMAIN,
		GETTING_RECORDS,
		GOT_RECORDS,

	} cshState;

	CloudServerHelper_RackspaceCloudDNS() {
		rackspace2= MafiaNet::OP_NEW<Rackspace2>(_FILE_AND_LINE_);
		memset(&thisServerDetail, 0, sizeof(ServerDetail));
	}
	~CloudServerHelper_RackspaceCloudDNS() {
		MafiaNet::OP_DELETE(rackspace2,_FILE_AND_LINE_);
	}
	virtual bool Update(void) {
		UpdateTCP();
		return true;
	}
	void CopyServerDetails(json_t *arrayElement, const char *publicIPV4)
	{
		strcpy_s(thisServerDetail.publicIPV4, publicIPV4);
		strcpy_s(thisServerDetail.id, json_string_value(json_object_get(arrayElement, "id")));
		json_t *privateAddressesArray = json_object_get(json_object_get(arrayElement, "addresses"),"private");
		size_t privateAddressesArraySize = json_array_size(privateAddressesArray);
		for (size_t l=0; l < privateAddressesArraySize; l++)
		{
			json_t *privateAddressElement = json_array_get(privateAddressesArray, l);
			if (json_integer_value(json_object_get(privateAddressElement, "version"))==4)
			{
				strcpy_s(thisServerDetail.privateIPV4,json_string_value(json_object_get(privateAddressElement, "addr")));
				break;
			}
		}

		strcpy_s(thisServerDetail.flavorId,json_string_value(json_object_get(json_object_get(arrayElement, "flavor"),"id")));
		strcpy_s(thisServerDetail.hostId,json_string_value(json_object_get(arrayElement, "hostId")));
		strcpy_s(thisServerDetail.id,json_string_value(json_object_get(arrayElement, "id")));
		strcpy_s(thisServerDetail.imageId,json_string_value(json_object_get(json_object_get(arrayElement, "image"),"id")));
		strcpy_s(thisServerDetail.name,json_string_value(json_object_get(arrayElement, "name")));
		strcpy_s(spawningImageId, thisServerDetail.imageId);
		strcpy_s(spawningFlavorId, thisServerDetail.flavorId);
		printf("Using image id %s for new servers\n", spawningImageId);
	}
	virtual void OnTCPFailure(void){ printf("--ERROR--: OnTCPFailure()\n"); }
	virtual void OnTransmissionFailed(HTTPConnection2 *httpConnection2, RakString postStr, RakString authURLDomain)
	{
		printf("--ERROR--: OnTransmissionFailed().\npostStr=%s\nauthURLDomain=%s\n", postStr.C_String(), authURLDomain.C_String());

		RakSleep(30000);
		printf("Retrying...\n");
		httpConnection2->TransmitRequest(postStr,authURLDomain, 443, true);
	}
	virtual void OnEmptyResponse(RakString stringTransmitted)
	{
		printf("--ERROR--: OnEmptyResponse(). stringTransmitted=%s", stringTransmitted.C_String());
	}
	virtual void OnMessage(const char *message, RakString responseReceived, RakString stringTransmitted, ptrdiff_t contentOffset)
	{
		// unused parameters
		(void)responseReceived;
		(void)contentOffset;

		printf("--WARNING--: OnMessage(). message=%s\nstringTransmitted=%s", message, stringTransmitted.C_String());
	}
	virtual void OnResponse(Rackspace2ResponseCode r2rc, RakString responseReceived, ptrdiff_t contentOffset)
	{
		if (r2rc==R2RC_AUTHENTICATED)
		{
			printf("Authenticated with Rackspace\nX-Auth-Token: %s\n", rackspace2->GetAuthToken());
			cshState=AUTHENTICATED;

			json_error_t error;
			json_t *root = json_loads(strstr(responseReceived.C_String() + contentOffset, "{"), JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK, &error);
			json_t *accessJson = json_object_get(root, "access");
			json_t *userJson  = json_object_get(accessJson, "user");
			json_t *regionJson = json_object_get(userJson, "RAX-AUTH:defaultRegion");
			const char *userRegionStr = json_string_value(regionJson);
			json_t *serviceCatalogArray = json_object_get(accessJson, "serviceCatalog");
			size_t arraySize = json_array_size(serviceCatalogArray);
			size_t i;
			printf("Services:\n");
			for (i=0; i < arraySize; i++)
			{
				json_t *arrayElement = json_array_get(serviceCatalogArray, i);
				json_t *elementNameJson = json_object_get(arrayElement, "name");
				const char *elementNameStr = json_string_value(elementNameJson);
				json_t *elementTypeJson = json_object_get(arrayElement, "type");
				const char *elementTypeStr = json_string_value(elementTypeJson);

				printf("%i. name=%s type=%s ", i+1, elementNameStr, elementTypeStr);

				json_t *endpointArray = json_object_get(arrayElement, "endpoints");
				size_t endpointArraySize = json_array_size(endpointArray);
				size_t j;
				char *myPublicURLStr=0;
				for (j=0; j < endpointArraySize; j++)
				{
					json_t *endpointArrayElement = json_array_get(endpointArray, j);
					json_t *publicURLJson = json_object_get(endpointArrayElement, "publicURL");
					const char *publicURLStr = json_string_value(publicURLJson);
					regionJson = json_object_get(endpointArrayElement, "region");
					if (regionJson)
					{
						const char *regionStr = json_string_value(regionJson);
						if (strcmp(regionStr, userRegionStr)==0)
						{
							printf("My URL=%s", publicURLStr);
							myPublicURLStr=(char*) publicURLStr;
							break;
						}
					}
					else
					{
						printf("My URL=%s", publicURLStr);
						myPublicURLStr=(char*) publicURLStr;
						break;
					}
				}
				if (j==endpointArraySize)
					printf("My URL=<unknown>");
				if (myPublicURLStr)
				{
					if (strcmp(elementNameStr, "cloudDNS")==0)
						strcpy_s(rackspaceDNSURL, myPublicURLStr);
					else if (strcmp(elementNameStr, "cloudServersOpenStack")==0)
						strcpy_s(rackspaceServersURL, myPublicURLStr);
				}
				printf("\n");
			}

			if (appState==AP_TERMINATING || appState==AP_TERMINATED ||
				appState==AP_TERMINATE_WHEN_ZERO_USERS_REACHED || appState==AP_TERMINATE_IF_NOT_DNS_HOST)
				Terminate();


			json_decref(root);
		}
		else if (r2rc==R2RC_GOT_SERVERS)
		{
			json_error_t error;
			json_t *root = json_loads(strstr(responseReceived.C_String() + contentOffset, "{"), JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK, &error);
			json_t *serversArray = json_object_get(root, "servers");
			size_t arraySize = json_array_size(serversArray);
			size_t i;

			for (i=0; i < arraySize && cshState==GETTING_SERVERS; i++)
			{
				json_t *arrayElement = json_array_get(serversArray, i);
				//json_t *publicAddressesArray = json_object_get(json_object_get(arrayElement, "addresses"),"public");
				//size_t publicAddressesArraySize = json_array_size(publicAddressesArray);
				//for (size_t j=0; j < publicAddressesArraySize; j++)
				//{
				for (unsigned k=0; k < g_rakPeer->GetNumberOfAddresses(); k++)
				{
					if (strcmp(g_rakPeer->GetLocalIP(k), json_string_value(json_object_get(arrayElement, "accessIPv4")))==0)
					{
						CopyServerDetails(arrayElement, g_rakPeer->GetLocalIP(k));

						cshState=GOT_MY_SERVER;

						break;
					}
				}
				//}



			}		

			if (cshState==GETTING_SERVERS)
			{
// 				printf("Could not find a server with this IP address\n");
// 				printf("Continue anyway (y/n)?\n");
// 				if (_getch()=='y')
// 				{
				
				for (unsigned k=0; k < g_rakPeer->GetNumberOfAddresses(); k++)
				{
					SystemAddress sa = g_rakPeer->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS,k);
					if (sa.IsLANAddress()==false)
					{
						json_t *arrayElement = json_array_get(serversArray, 0);
						CopyServerDetails(arrayElement, g_rakPeer->GetLocalIP(0));
						cshState=GOT_MY_SERVER;;
						break;
					}
				}

					
// 				}
// 				else
// 					cshState=CANNOT_FIND_MY_SERVER;
			}

			json_decref(root);
		}
		else if (r2rc==R2RC_GOT_IMAGES)
		{
			json_error_t error;
			json_t *root = json_loads(strstr(responseReceived.C_String() + contentOffset, "{"), JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK, &error);
			json_t *domainsArray = json_object_get(root, "images");
			size_t arraySize = json_array_size(domainsArray);
			size_t i;
			printf("Got %i images:\n", arraySize);
			for (i=0; i < arraySize; i++)
			{
				json_t *arrayElement = json_array_get(domainsArray, i);
				printf("%i. name=%s ", i+1, json_string_value(json_object_get(arrayElement, "name")));
				printf("progress=%" JSON_INTEGER_FORMAT " ", json_integer_value(json_object_get(arrayElement, "progress")));
				printf("status=%s ", json_string_value(json_object_get(arrayElement, "status")));
				printf("created=%s ", json_string_value(json_object_get(arrayElement, "created")));
				printf("id=%s\n", json_string_value(json_object_get(arrayElement, "id")));
			}

			printf("\n");
			if (arraySize>0)
			{
				printf("Select image to spawn for new servers?: ");
				char str[32];
				Gets(str, sizeof(str));
				if (str[0])
				{
					int idx = atoi(str);
					if (idx>=1 && static_cast<unsigned>(idx) <= arraySize)
					{
						json_t *arrayElement = json_array_get(domainsArray, idx-1);
						strcpy_s(spawningImageId, json_string_value(json_object_get(arrayElement, "id")));
						// Use spawningFlavorId of whatever it was when the server was started, I don't care about flavor
						printf("Using image id %s for new servers\n", spawningImageId);
					}
				}
				else
					printf("Aborted.\n");
			}

			json_decref(root);
		}
		else if (r2rc==R2RC_GOT_RECORDS)
		{
			json_error_t error;
			json_t *root = json_loads(strstr(responseReceived.C_String() + contentOffset, "{"), JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK, &error);
			json_t *domainsArray = json_object_get(root, "records");
			size_t arraySize = json_array_size(domainsArray);
			size_t i;
			for (i=0; i < arraySize; i++)
			{
				json_t *arrayElement = json_array_get(domainsArray, i);
				json_t *elementNameJson = json_object_get(arrayElement, "name");
				const char *elementNameStr = json_string_value(elementNameJson);
				if (strcmp(elementNameStr, patcherHostSubdomainURL)==0)
				{
					arrayElement = json_array_get(domainsArray, i);
					json_t *elementIPJson = json_object_get(arrayElement, "data");
					strcpy_s(dnsHostIP, json_string_value(elementIPJson));
					json_t *elementIDJson = json_object_get(arrayElement, "id");
					strcpy_s(patchHostRecordID,json_string_value(elementIDJson));


					timeOfLastDNSHostCheck= MafiaNet::GetTime();
					if (appState==AP_TERMINATE_IF_NOT_DNS_HOST)
					{
						if (strcmp(dnsHostIP, g_rakPeer->GetLocalIP(0))==0)
						{
							// We are dns host. Just keep running
							appState=AP_RUNNING;
						}
						else
						{
							if (timeSinceZeroUsers!=0)
								Terminate();
							else
								appState=AP_TERMINATE_WHEN_ZERO_USERS_REACHED;
						}
					}

					cshState=GOT_RECORDS;
					break;
				}
			}
			if (i==arraySize)
			{
				printf("Can't find %s in record list\n", patcherHostSubdomainURL);
				cshState=CANNOT_FIND_PATCHER_RECORD;
			}
			json_decref(root);
		}
		else if (r2rc==R2RC_GOT_DOMAINS)
		{
			json_error_t error;
			json_t *root = json_loads(strstr(responseReceived.C_String() + contentOffset, "{"), JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK, &error);
			json_t *domainsArray = json_object_get(root, "domains");
			size_t arraySize = json_array_size(domainsArray);
			size_t i;
			for (i=0; i < arraySize; i++)
			{
				json_t *arrayElement = json_array_get(domainsArray, i);
				json_t *elementNameJson = json_object_get(arrayElement, "name");
				const char *elementNameStr = json_string_value(elementNameJson);
				if (strcmp(elementNameStr, "raknetpatcher.com")==0)
				{
					// Primary domain
					json_t *idNameJson = json_object_get(arrayElement, "id");
					raknetPatcherDomainId = (int) json_integer_value(idNameJson);
					RakString url("%s/domains/%i/records", rackspaceDNSURL, raknetPatcherDomainId );
					rackspace2->AddOperation(url,Rackspace2::OT_GET,0, true);

					cshState=GETTING_RECORDS;

					break;
				}
			}
			json_decref(root);
			if (i==arraySize)
			{
				printf("Can't find raknetpatcher.com in domain list\n");
				cshState=CANNOT_FIND_PATCHER_DOMAIN;
			}
		}
		else
		{
			if (r2rc==R2RC_UNAUTHORIZED)
			{
				printf("Unauthorized\n");

				// Try to reauthenticate
				rackspace2->Authenticate(rackspaceAuthenticationURL, rackspaceCloudUsername, apiAccessKey);
			}
			else
			{
				printf("General response failure: %s\n", responseReceived.C_String());
			}
		}

	}

	virtual bool UpdateTCP() {
		rackspace2->Update();
		return false;
	}

	void Terminate(void)
	{
		appState=AP_TERMINATING;

		// http://docs.rackspace.com/servers/api/v2/cs-devguide/content/Delete_Server-d1e2883.html
		RakString url("%s/servers/%s", rackspaceServersURL, thisServerDetail.id);
		rackspace2->AddOperation(url,Rackspace2::OT_DELETE,0, true);
	}
	void GetCustomImages(void)
	{
		// curl -k https://ord.servers.api.rackspacecloud.com/v2/570016/images?type=SNAPSHOT --header "X-Auth-Token: 97d8335b-7b9e-4a74-96a1-2a3a043c1000" --header "Content-Type: application/json"

		// List images
		// http://docs.rackspace.com/servers/api/v2/cs-devguide/content/List_Images-d1e4435.html
		RakString url("%s/images/detail?type=SNAPSHOT", rackspaceServersURL);
		rackspace2->AddOperation(url,Rackspace2::OT_GET,0, true);
	}
	void ImageThisServer(void)
	{
		// Create image
		// date
		// http://docs.rackspace.com/servers/api/v2/cs-devguide/content/Create_Image-d1e4655.html
		time_t aclock;
		time( &aclock );   // Get time in seconds
		tm newtime;
		localtime_s( &newtime, &aclock );   // Convert time to struct tm form 
		char text[1024];
		sprintf_s(text, "%s_%s", patcherHostSubdomainURL, asctime( &newtime ));

		RakString url("%s/servers/%s/action", rackspaceServersURL, thisServerDetail.id);
		json_t *jsonObjectRoot = json_object();
		json_t *jsonObjectCreateImage = json_object();
		json_t *jsonMetaData = json_object();
		json_object_set(jsonObjectCreateImage, "name", json_string(text));
		json_object_set(jsonObjectRoot, "createImage", jsonObjectCreateImage);
		json_object_set(jsonObjectCreateImage, "metadata", jsonMetaData);
		json_object_set(jsonMetaData, "ImageType",  json_string("snapshot"));

		printf("Imaging server with name %s\n", text);
		rackspace2->AddOperation(url,Rackspace2::OT_POST,jsonObjectRoot, true);
	}

	void SpawnServers(unsigned count)
	{
		if (count==0)
			return;
		RakAssert(count <= 4);

		// http://docs.rackspace.com/servers/api/v2/cs-devguide/content/CreateServers.html

		RakString url("%s/servers", rackspaceServersURL);

		time_t aclock;
		time( &aclock );   // Get time in seconds
		tm newtime;
		localtime_s( &newtime, &aclock );   // Convert time to struct tm form 
		char text[1024];
		for (unsigned int i=0; i < count; i++)
		{
			json_t *jsonObjectRoot = json_object();
			json_t *jsonObjectServer = json_object();
			json_object_set(jsonObjectRoot, "server", jsonObjectServer);
			sprintf_s(text, "%s_%s_%u", patcherHostSubdomainURL, asctime( &newtime ), i);
			json_object_set(jsonObjectServer, "name", json_string(text));
			json_object_set(jsonObjectServer, "imageRef", json_string(spawningImageId));
			json_object_set(jsonObjectServer, "flavorRef", json_string(spawningFlavorId));
			rackspace2->AddOperation(url,Rackspace2::OT_POST,jsonObjectRoot, true);
		}
	}


	void GetThisServer(void)
	{
		// http://docs.rackspace.com/servers/api/v2/cs-devguide/content/List_Servers-d1e2078.html
		cshState=GETTING_SERVERS;

		RakString url("%s/servers/detail?status=ACTIVE", rackspaceServersURL);
		//RakString url("%s/servers/detail", rackspaceServersURL);
		rackspace2->AddOperation(url,Rackspace2::OT_GET,0, true);
	}

	void GetDomainRecords(void) {
		cshState=GETTING_DOMAINS;
		RakString url("%s/domains/?name=%s", rackspaceDNSURL, "raknetpatcher.com");
		rackspace2->AddOperation(url,Rackspace2::OT_GET,0, true);
	}

	bool AuthenticateWithRackspaceBlocking(void) {
		rackspace2->SetEventCallback(this);
		rackspace2->Authenticate(rackspaceAuthenticationURL, rackspaceCloudUsername, apiAccessKey);
		cshState=AUTHENTICATING;

		while (cshState==AUTHENTICATING)
		{
			UpdateTCP();
			RakSleep(30);
		}
		if (cshState!=AUTHENTICATED)
			return false;

		GetThisServer();
		while (cshState==GETTING_SERVERS)
		{
			UpdateTCP();
			RakSleep(30);
		}
		if (cshState!=GOT_MY_SERVER)
		{
			return false;
		}

		// http://docs.rackspace.com/cdns/api/v1.0/cdns-devguide/content/list_domains.html
		GetDomainRecords();
		while (cshState==GETTING_DOMAINS || cshState==GETTING_RECORDS)
		{
			UpdateTCP();
			RakSleep(30);
		}
		if (cshState!=GOT_RECORDS)
			return false;

		return true;
	}
	void UpdateHostIPAsynch(SystemAddress sa) {
		if (appState!=AP_RUNNING)
			return;

		// RakString url("%s/domains/domainId/records/recordId");
		// rackspace2->AddOperation()

		// http://docs.rackspace.com/cdns/api/v1.0/cdns-devguide/content/Modify_Records-d1e5033.html
		//rackspace2;

		RakString url("%s/domains/%i/records/%s", rackspaceDNSURL, raknetPatcherDomainId, patchHostRecordID);

		json_t *jsonObject = json_object();
		json_object_set(jsonObject, "data", json_string(sa.ToString(false)));
		printf("Setting DNS to %s\n", sa.ToString(false));
		// TESTING HACK
		// json_object_set(jsonObject, "data", json_string("198.61.202.20"));
		json_object_set(jsonObject, "name", json_string(patcherHostSubdomainURL));
		rackspace2->AddOperation(url,Rackspace2::OT_PUT,jsonObject, true);
	}
	
	virtual void PrintHelp(void)
	{
		printf("Distributed authenticated CloudServer using DNS based host migration.\n");
		printf("Parameter1: serverToServerPassword\n");
		printf("Parameter2: serverPort\n");
		printf("Parameter3: allowedIncomingConnections\n");
		printf("Parameter4: allowedOutgoingConnections\n");
		printf("Parameter5: patcherHostRecordURL\n");
		printf("Parameter6: patcherHostDomainURL\n");
		printf("Parameter7: rackspaceAuthenticationURL\n");
		printf("Parameter8: rackspaceCloudUsername\n");
		printf("Parameter9: apiAccessKey\n");
		printf("Parameter10: databasePassword\n");
		printf("Parameter11: workerThreadCount\n");
		printf("Parameter12: sqlConnectionObjectCount\n");
		printf("Parameter13: allowDownloadingUnmodifiedFiles\n");
		printf("Example:\n");
		printf("AutopatcherServer_SelfScaling s2sPassword 60000 1024 128 game1.mycompanypatcher.com mycompanypatcher.com https://identity.api.rackspacecloud.com/v2.0 rackspaceUsername rackspacePw dbPassword 3 6 1");
	}
	virtual bool ParseCommandLineParameters(int argc, char **argv) {

		if (argc!=14)
			PrintHelp();

		serverToServerPassword=argv[1];
		const int intServerPort = atoi(argv[2]);
		if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
			return false;
		}
		rakPeerPort = static_cast<unsigned short>(intServerPort);
		int intConnections = atoi(argv[3]);
		if ((intConnections < 0) || (intConnections > std::numeric_limits<unsigned short>::max())) {
			printf("Specified allowed incoming connections %d is outside valid bounds [0, %u]", intConnections, std::numeric_limits<unsigned short>::max());
			return false;
		}
		allowedIncomingConnections = static_cast<unsigned short>(intConnections);
		intConnections = atoi(argv[4]);
		if ((intConnections < 0) || (intConnections > std::numeric_limits<unsigned short>::max())) {
			printf("Specified allowed outgoing connections %d is outside valid bounds [0, %u]", intConnections, std::numeric_limits<unsigned short>::max());
			return false;
		}
		allowedOutgoingConnections = static_cast<unsigned short>(intConnections);
		strcpy_s(patcherHostSubdomainURL, argv[5]);
		strcpy_s(patcherHostDomainURL, argv[6]);
		strcpy_s(rackspaceAuthenticationURL, argv[7]);
		strcpy_s(rackspaceCloudUsername, argv[8]);
		strcpy_s(apiAccessKey, argv[9]);
		strcpy_s(databasePassword, argv[10]);
		workerThreadCount=atoi(argv[11]);
		sqlConnectionObjectCount=atoi(argv[12]);
		allowDownloadingUnmodifiedFiles=atoi(argv[13]);




	/*
	2 worker threads, 4 mb read: 5 minutes, 55 seconds
	4 worker threads, 4 mb read: 5 minutes, 7 seconds
	8 worker threads, 4 mb read: 4 minutes, 48 seconds
	2 worker threads, 2 mb read: 8 minutes, 31 seconds
	8 worker threads, 2 mb read: 4 minutes, 30 seconds, but one failed to start
	4 worker threads, 8 mb read: 4 minutes, 55 seconds
	4 worker threads, 16 mb read: 4 minutes, 48 seconds
	3 worker threads, 16 mb read: 4 minutes, 19 seconds
	2 worker threads, 16 mb read: 5 minutes, 18 seconds
	3 worker threads, 32 mb read: 4 minutes, 18 seconds
	*/


		return true;
	}
	// Call when you get ID_FCM2_NEW_HOST
	virtual void OnFCMNewHost(Packet *packet, RakPeerInterface *rakPeer) {
		RakAssert(packet->data[0]==ID_FCM2_NEW_HOST);
		MafiaNet::BitStream bsIn(packet->data, packet->length, false);
		bsIn.IgnoreBytes(sizeof(MessageID));
		RakNetGUID oldHost;
		bsIn.Read(oldHost);
		RakNetGUID newHost = packet->guid;
		if (newHost==rakPeer->GetMyGUID() && oldHost!=newHost)
		{
			for (unsigned int i=0; i < rakPeer->GetNumberOfAddresses(); i++)
			{
				SystemAddress sa = rakPeer->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS,i);
				if (sa.IsLANAddress()==false)
				{
					printf("Assuming host. Updating DNS\n");
					UpdateHostIPAsynch(sa);
					break;
				}
			}
		}
	}

	const char *GetCloudHostAddress(void) const {return dnsHostIP;}
	MafiaNet::Time GetTimeOfLastDNSHostCheck(void) {return timeOfLastDNSHostCheck;}
	void SetTimeOfLastDNSHostCheck(MafiaNet::Time t) {timeOfLastDNSHostCheck=t;}

	virtual int OnJoinCloudResult(
		Packet *packet,
		MafiaNet::RakPeerInterface *rakPeer,
		MafiaNet::CloudServer *cloudServer,
		MafiaNet::CloudClient *cloudClient,
		MafiaNet::FullyConnectedMesh2 *fullyConnectedMesh2,
		MafiaNet::TwoWayAuthentication *twoWayAuthentication,
		MafiaNet::ConnectionGraph2 *connectionGraph2,
		const char *rakPeerIpOrDomain,
		char myPublicIP[32]
	) {
		if (packet->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
		{

			for (unsigned int i=0; i < rakPeer->GetNumberOfAddresses(); i++)
			{
				SystemAddress sa = rakPeer->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS,i);
				if (sa.IsLANAddress()==false)
				{
					printf("Failed connection. Changing DNS to point to this system.\n");
					UpdateHostIPAsynch(sa);
					break;
				}
			}
		}
		else if (packet->data[0]==ID_ALREADY_CONNECTED && packet->systemAddress.IsLANAddress())
		{
			for (unsigned int i=0; i < rakPeer->GetNumberOfAddresses(); i++)
			{
				SystemAddress sa = rakPeer->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS,i);
				if (sa.IsLANAddress()==false)
				{
					printf("Connected to self-LAN address. Changing DNS to point to this system.\n");

					UpdateHostIPAsynch(sa);
					return CloudServerHelper::OnJoinCloudResult(packet, rakPeer, cloudServer, cloudClient, fullyConnectedMesh2, twoWayAuthentication, connectionGraph2, sa.ToString(false), myPublicIP);
				}
			}
		}

		return CloudServerHelper::OnJoinCloudResult(packet, rakPeer, cloudServer, cloudClient, fullyConnectedMesh2, twoWayAuthentication, connectionGraph2, rakPeerIpOrDomain, myPublicIP);
	}

	virtual void OnConnectionCountChange(RakPeerInterface *rakPeer, CloudClient *cloudClient)
	{
		MafiaNet::BitStream bs;
		CloudKey cloudKey("CloudConnCount",0);
		bs.Write(autopatcherLoad);
		cloudClient->Post(&cloudKey, bs.GetData(), bs.GetNumberOfBytesUsed(), rakPeer->GetMyGUID());
	}

	char patcherHostSubdomainURL[128];
	char patcherHostDomainURL[128];
	char dnsHostIP[128];
	char rackspaceAuthenticationURL[128];
	char rackspaceDNSURL[128];
	char rackspaceServersURL[128];
	char rackspaceCloudUsername[128];
	char apiAccessKey[128];
	Rackspace2 *rackspace2;
	char patchHostRecordID[128];
protected:

	int raknetPatcherDomainId;
	MafiaNet::Time timeOfLastDNSHostCheck;

	struct ServerDetail
	{
		char publicIPV4[32];
		char privateIPV4[32];
		char flavorId[128];
		char hostId[128];
		char id[128];
		char imageId[128];
		char name[128];
	} thisServerDetail;

	char spawningImageId[128];
	char spawningFlavorId[128];

} *cloudServerHelper;

class AutopatcherLoadNotifier : public MafiaNet::AutopatcherServerLoadNotifier
{
	void AutopatcherLoadNotifier::OnQueueUpdate(SystemAddress remoteSystem,
		AutopatcherServerLoadNotifier::RequestType requestType,
		AutopatcherServerLoadNotifier::QueueOperation queueOperation,
		AutopatcherServerLoadNotifier::AutopatcherState *autopatcherState)
	{
		// unused parameters
		(void)remoteSystem;
		(void)requestType;
		(void)queueOperation;

		// #med - consider changing autopatcherLoad to unsigned int
		autopatcherLoad=static_cast<unsigned short>(autopatcherState->requestsQueued+autopatcherState->requestsWorking);

		//AutopatcherServerLoadNotifier_Printf::OnQueueUpdate(remoteSystem, requestType, queueOperation, autopatcherState);
	}

	void AutopatcherLoadNotifier::OnGetChangelistCompleted(
		SystemAddress remoteSystem,
		AutopatcherServerLoadNotifier::GetChangelistResult getChangelistResult,
		AutopatcherServerLoadNotifier::AutopatcherState *autopatcherState)
	{
		// unused parameters
		(void)remoteSystem;
		(void)getChangelistResult;

		autopatcherLoad=static_cast<unsigned short>(autopatcherState->requestsQueued+autopatcherState->requestsWorking);

		//AutopatcherServerLoadNotifier_Printf::OnGetChangelistCompleted(remoteSystem, getChangelistResult, autopatcherState);
	}

	void AutopatcherLoadNotifier::OnGetPatchCompleted(SystemAddress remoteSystem,
		AutopatcherServerLoadNotifier::PatchResult patchResult,
		AutopatcherServerLoadNotifier::AutopatcherState *autopatcherState)
	{
		// unused parameters
		(void)remoteSystem;
		(void)patchResult;

		autopatcherLoad=static_cast<unsigned short>(autopatcherState->requestsQueued+autopatcherState->requestsWorking);

		//AutopatcherServerLoadNotifier_Printf::OnGetPatchCompleted(remoteSystem, patchResult, autopatcherState);
	}
};

void SetMaxConcurrentUsers(int i)
{
	autopatcherServer->SetMaxConurrentUsers(i);
}


//char WORKING_DIRECTORY[MAX_PATH];
//char PATH_TO_XDELTA_EXE[MAX_PATH];

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

		char WORKING_DIRECTORY[MAX_PATH];
		GetTempPathA(MAX_PATH, WORKING_DIRECTORY);
		if (WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='\\' || WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=='/')
			WORKING_DIRECTORY[strlen(WORKING_DIRECTORY)-1]=0;

		const char *PATH_TO_XDELTA_EXE = "c:/xdelta3-3.0.6-win32.exe";

		bool shortContent = contentLengthOld < 33554432 && contentLengthNew < 33554432;
		if (shortContent || PATH_TO_XDELTA_EXE[0]==0)
		{
			if (shortContent==false)
			{
				printf("Warning: PATH_TO_XDELTA_EXE needed but not set.\ncontentLengthOld=%i\ncontentLengthNew=%i\n", contentLengthOld, contentLengthNew );

			}
			// Use bsdiff, which does a good job but takes a lot of memory based on the size of the file
			*patchAlgorithm=0;
			bool b = MakePatchBSDiff(fpOld, contentLengthOld, fpNew, contentLengthNew, patch, patchLength);
			fclose(fpOld);
			fclose(fpNew);
			return b == false ? -1 : 0;
		}
		else
		{
			*patchAlgorithm=1;
			fclose(fpOld);
			fclose(fpNew);

			char buff[128];
			MafiaNet::TimeUS time = MafiaNet::GetTimeUS();
#if defined(_WIN32)
			sprintf_s(buff, "%I64u", time);
#else
			sprintf_s(buff, "%llu", (long long unsigned int) time);
#endif

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

			// ShellExecute is blocking, but if it writes a file to disk that file is not always immediately accessible after it returns. And this only happens in release, and only when not running in the debugger
			//ShellExecute(nullptr, "open", PATH_TO_XDELTA_EXE, commandLine, WORKING_DIRECTORY, SW_SHOWNORMAL);

			char pathToPatch[MAX_PATH];
			sprintf_s(pathToPatch, "%s/patchServer_%s.tmp", WORKING_DIRECTORY, buff);
			FILE *fpPatch;
			errno_t error = fopen_s(&fpPatch, pathToPatch, "r+b");
			MafiaNet::TimeUS stopWaiting = time + 60000000 * 5;
			while (error!=0 && MafiaNet::GetTimeUS() < stopWaiting)
			{
				RakSleep(1000);
				error = fopen_s(&fpPatch, pathToPatch, "r+b");
			}
			if (error!=0)
			{
				char buff2[1024];
				strerror_s(buff2, errno);
				printf("\nERROR: Could not open %s.\nerr=%i (%s)\narguments=%s\n", pathToPatch, errno, buff2, commandLine);
				return 1;
			}
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
		}
	}
};

int main(int argc, char **argv)
{
#if OPEN_SSL_CLIENT_SUPPORT==0
#error RakNet must be compiled with OPEN_SSL_CLIENT_SUPPORT=1 to connect to Rackspace
	return 0;
#endif

	cloudServerHelper = MafiaNet::OP_NEW<CloudServerHelper_RackspaceCloudDNS>(_FILE_AND_LINE_);
	if (!cloudServerHelper->ParseCommandLineParameters(argc, argv))
	{
		return 1;
	}

	g_rakPeer = MafiaNet::RakPeerInterface::GetInstance();


	MafiaNet::Time timeForNextLoadCheck= MafiaNet::GetTime()+LOAD_CHECK_INTERVAL;

	if (!cloudServerHelper->AuthenticateWithRackspaceBlocking())
	{
		printf("Exiting due to failed startup with Rackspace\n");
		return 1;
	}

	// This does not work unless it comes after AuthenticateWithRackspaceBlocking
	MafiaNet::PacketizedTCP packetizedTCP;
	if (packetizedTCP.Start(LISTEN_PORT_TCP_PATCHER,cloudServerHelper->allowedIncomingConnections)==false)
	{
		printf("Failed to start TCP. Is the port already in use?");
		return 1;
	}

	appState=AP_RUNNING;
	timeSinceZeroUsers= MafiaNet::GetTime();
	printf("Server starting... ");
	autopatcherServer = MafiaNet::OP_NEW<AutopatcherServer>(_FILE_AND_LINE_);
	// MafiaNet::FLP_Printf progressIndicator;
	MafiaNet::FileListTransfer fileListTransfer;
	AutopatcherPostgreRepository2_WithXDelta *connectionObject;
	connectionObject = MafiaNet::OP_NEW_ARRAY<AutopatcherPostgreRepository2_WithXDelta>(sqlConnectionObjectCount, _FILE_AND_LINE_);
	// MafiaNet::AutopatcherRepositoryInterface **connectionObjectAddresses[sqlConnectionObjectCount];
	AutopatcherRepositoryInterface **connectionObjectAddresses = MafiaNet::OP_NEW_ARRAY<AutopatcherRepositoryInterface *>(sqlConnectionObjectCount, _FILE_AND_LINE_);

	for (int i=0; i < sqlConnectionObjectCount; i++)
		connectionObjectAddresses[i]=&connectionObject[i];
	autopatcherServer->SetFileListTransferPlugin(&fileListTransfer);
	// PostgreSQL is fast, so this may not be necessary, or could use fewer threads
	// This is used to read increments of large files concurrently, thereby serving users downloads as other users read from the DB
	fileListTransfer.StartIncrementalReadThreads(sqlConnectionObjectCount);
	autopatcherServer->SetMaxConurrentUsers(workerThreadCount); // More users than this get queued up
	AutopatcherLoadNotifier autopatcherLoadNotifier;
	autopatcherServer->SetLoadManagementCallback(&autopatcherLoadNotifier);
	packetizedTCP.AttachPlugin(autopatcherServer);
	packetizedTCP.AttachPlugin(&fileListTransfer);

	autopatcherServer->SetAllowDownloadOfOriginalUnmodifiedFiles(allowDownloadingUnmodifiedFiles!=0);

	// ---- PLUGINS -----
	// Used to load balance clients, allow for client to client discovery
	MafiaNet::CloudServer cloudServer;
	// Used to update the local cloudServer
	MafiaNet::CloudClient cloudClient;
	// Used to determine the host of the server fully connected mesh, as well as to connect servers automatically
	MafiaNet::FullyConnectedMesh2 fullyConnectedMesh2;
	// Used for servers to verify each other - otherwise any system could pose as a server
	// Could also be used to verify and restrict clients if paired with the MessageFilter plugin
	MafiaNet::TwoWayAuthentication twoWayAuthentication;
	// Used to tell servers about each other
	MafiaNet::ConnectionGraph2 connectionGraph2;

	g_rakPeer->AttachPlugin(&cloudServer);
	g_rakPeer->AttachPlugin(&cloudClient);
	g_rakPeer->AttachPlugin(&fullyConnectedMesh2);
	g_rakPeer->AttachPlugin(&twoWayAuthentication);
	g_rakPeer->AttachPlugin(&connectionGraph2);
	
	if (!cloudServerHelper->StartRakPeer(g_rakPeer))
		return 1;

	MafiaNet::CloudServerHelperFilter sampleFilter; // Keeps clients from updating stuff to the server they are not supposed to
	sampleFilter.serverGuid= g_rakPeer->GetMyGUID();
	cloudServerHelper->SetupPlugins(&cloudServer, &sampleFilter, &cloudClient, &fullyConnectedMesh2, &twoWayAuthentication,&connectionGraph2, cloudServerHelper->serverToServerPassword);

	int ret;
	do 
	{
		ret = cloudServerHelper->JoinCloud(g_rakPeer, &cloudServer, &cloudClient, &fullyConnectedMesh2, &twoWayAuthentication, &connectionGraph2, cloudServerHelper->GetCloudHostAddress());
	} while (ret==2);
	if (ret==1)
		return 1;

	printf("started.\n");

	printf("Enter database password:\n");
	char connectionString[256];
	char username[256];
	bool connectionSuccess=false;
	while (connectionSuccess==false)
	{
		strcpy_s(username, "postgres");
		strcpy_s(connectionString, "user=");
		strcat_s(connectionString, username);
		strcat_s(connectionString, " password=");
		strcat_s(connectionString, databasePassword);
		int conIdx;
		for (conIdx=0; conIdx < sqlConnectionObjectCount; conIdx++)
		{
			if (connectionObject[conIdx].Connect(connectionString)==false)
			{
				printf("Database connection failed.\n");
				printf("Try a different password? (y/n)\n");
				if (_getch()!='y')
				{
					return 1;
				}
				else
				{
					connectionSuccess=false;
					printf("Enter password: ");
					Gets(databasePassword, sizeof(databasePassword));
					break;
				}
			}
		}
		if (conIdx==sqlConnectionObjectCount)
			connectionSuccess=true;
	}

	printf("Database connection suceeded.\n");
	printf("Starting threads\n");
	// 4 Worker threads, which is CPU intensive
	// A greater number of SQL connections, which read files incrementally for large downloads
	autopatcherServer->StartThreads(workerThreadCount,sqlConnectionObjectCount, connectionObjectAddresses);
	autopatcherServer->CacheMostRecentPatch(0);
	printf("System ready for connections\n");

	printf("(D)rop database\n(C)reate database.\n(U)pdate revision.\n(S)pawn a clone of this server\n(M)ax concurrent users (this server only)\n(I)mage the current state of this server\n(L)ist images\n(T)erminate cloud\n(Q)uit\n");

	int ch;
	MafiaNet::Packet *p;
	while (appState!=AP_TERMINATED)
	{
		MafiaNet::SystemAddress notificationAddress;
		notificationAddress=packetizedTCP.HasCompletedConnectionAttempt();
// 		if (notificationAddress!=MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
// 			printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
		notificationAddress=packetizedTCP.HasNewIncomingConnection();
// 		if (notificationAddress!=MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
// 			printf("ID_NEW_INCOMING_CONNECTION\n");
		notificationAddress=packetizedTCP.HasLostConnection();
// 		if (notificationAddress!=MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
// 			printf("ID_CONNECTION_LOST\n");

		p=packetizedTCP.Receive();
		while (p)
		{
			packetizedTCP.DeallocatePacket(p);
			p=packetizedTCP.Receive();
		}


		if (autopatcherLoad==0)
		{
			if (appState==AP_TERMINATE_WHEN_ZERO_USERS_REACHED)
			{
				cloudServerHelper->Terminate();
			}
			if (timeSinceZeroUsers==0)
				timeSinceZeroUsers= MafiaNet::GetTime();
		}
		else
		{
			timeSinceZeroUsers=0;
		}

		p= g_rakPeer->Receive();
		while (p)
		{
			/*
			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
				printf("ID_NEW_INCOMING_CONNECTION (TCP) from %s\n", p->systemAddress.ToString(true));
			else if (p->data[0]==ID_DISCONNECTION_NOTIFICATION)
				printf("ID_DISCONNECTION_NOTIFICATION (TCP) from %s\n", p->systemAddress.ToString(true));
			else if (p->data[0]==ID_CONNECTION_LOST)
				printf("ID_CONNECTION_LOST (TCP) from %s\n", p->systemAddress.ToString(true));
			else
			*/
			if (p->data[0]==ID_FCM2_NEW_HOST)
			{
				if (appState==AP_RUNNING)
				{
					MafiaNet::BitStream bs(p->data,p->length,false);
					bs.IgnoreBytes(1);
					RakNetGUID oldHost;
					bs.Read(oldHost);

					if (p->guid== g_rakPeer->GetMyGUID())
					{
						if (oldHost!=UNASSIGNED_RAKNET_GUID)
						{
							printf("ID_FCM2_NEW_HOST: Taking over as host from the old host.\n");
						}
						else
						{
							// Room not hosted if we become host the first time since this was done in CreateRoom() already
							printf("ID_FCM2_NEW_HOST: We have become host for the first time.\n");
						}
					}

					timeForNextLoadCheck= MafiaNet::GetTime()+LOAD_CHECK_INTERVAL;
				}
			}
			else if (p->data[0]==ID_USER_PACKET_ENUM)
			{
				MafiaNet::BitStream bsIn(p->data, p->length, false);
				bsIn.IgnoreBytes((sizeof(MessageID)));
				RakString incomingPw;
				bsIn.Read(incomingPw);
				if (incomingPw==cloudServerHelper->serverToServerPassword)
				{
					SetMaxConcurrentUsers(0);

					if (appState==AP_RUNNING)
					{
						if (timeSinceZeroUsers!=0)
							cloudServerHelper->Terminate();
						else
							appState=AP_TERMINATE_WHEN_ZERO_USERS_REACHED;
					}

					// Disconnect from the cloud, and do not accept new connections from users
					// The autopatcher will still run since it is on TCP
					g_rakPeer->Shutdown(1000);
				}

			}
			else if (p->data[0]==ID_CLOUD_GET_RESPONSE)
			{
				MafiaNet::CloudQueryResult cloudQueryResult;
				cloudClient.OnGetReponse(&cloudQueryResult, p);
				unsigned int rowIndex;
				const bool wasCallToGetServers=cloudQueryResult.cloudQuery.keys[0].primaryKey=="CloudConnCount";
				RakAssert(wasCallToGetServers);
				/*
				if (wasCallToGetServers)
					printf("Downloaded server list. %i servers.\n", cloudQueryResult.rowsReturned.Size());
					*/

				MafiaNet::SystemAddress lowestConnectionAddress;

				int totalConnections=0;
				for (rowIndex=0; rowIndex < cloudQueryResult.rowsReturned.Size(); rowIndex++)
				{
					MafiaNet::CloudQueryRow *row = cloudQueryResult.rowsReturned[rowIndex];

					unsigned short connCount;
					MafiaNet::BitStream bsIn(row->data, row->length, false);
					bsIn.Read(connCount);
					printf("%i. Server found at %s with %i connections\n", rowIndex+1, row->serverSystemAddress.ToString(true), connCount);

					totalConnections+=connCount;
				}

				const int MAX_SERVERS_EVER=32;
				int available = cloudQueryResult.rowsReturned.Size() * sqlConnectionObjectCount - totalConnections;
				if (available < 0 &&
					cloudQueryResult.rowsReturned.Size() < MAX_SERVERS_EVER // no more than MAX_SERVERS_EVER servers ever, to control costs
					)
				{
					int newServersNeeded = -available / sqlConnectionObjectCount;
					if (newServersNeeded > 4)
						newServersNeeded = 4; // Do not start more than 4 servers at a time
					if (cloudQueryResult.rowsReturned.Size() + newServersNeeded > MAX_SERVERS_EVER)
					{
						newServersNeeded = MAX_SERVERS_EVER - cloudQueryResult.rowsReturned.Size();
					}
					if (newServersNeeded>0)
						cloudServerHelper->SpawnServers(static_cast<unsigned>(newServersNeeded));
				}

				timeForNextLoadCheck = MafiaNet::GetTime() + LOAD_CHECK_INTERVAL_AFTER_SPAWN;

				cloudClient.DeallocateWithDefaultAllocator(&cloudQueryResult);
			}

			cloudServerHelper->OnPacket(p, g_rakPeer, &cloudClient, &cloudServer, &fullyConnectedMesh2, &twoWayAuthentication, &connectionGraph2);

			g_rakPeer->DeallocatePacket(p);
			p=g_rakPeer->Receive();
		}

		if (timeSinceZeroUsers!=0 && appState==AP_RUNNING && autopatcherServer->GetMaxConurrentUsers()>0)
		{
			// No users currently connected
			MafiaNet::Time curTime = MafiaNet::GetTime();
			MafiaNet::Time diff = curTime - timeSinceZeroUsers;
			if (diff > 0)
			{
				if (fullyConnectedMesh2.IsHostSystem()==false)
				{
					// 2. Shutdown automatically if no users for 12 hours, except if we are the host
					if (diff > 1000 * 60 * 60 * 12)
						cloudServerHelper->Terminate();
				}
				else
				{
					// We are host according to RakPeer. But no users for 24 hours
					// Verify we are the host according to Rackspace DNS records. If not, shutdown
					if (diff > 1000 * 60 * 60 * 24)
					{
						// No users for 24 hours, verify if we are the host according to Rackspace DNS records. If not, shutdown
						MafiaNet::Time diff2;
						diff2 = curTime - cloudServerHelper->GetTimeOfLastDNSHostCheck();
						if (diff2 > 1000 * 60 * 60 * 24)
						{
							appState=AP_TERMINATE_IF_NOT_DNS_HOST;
							cloudServerHelper->SetTimeOfLastDNSHostCheck(curTime);
							cloudServerHelper->GetDomainRecords();
						}
					}
				}
			}
		}

		cloudServerHelper->Update();

		
		// 4. If we are the host, every 1 hour check load among all servers (including self). It takes like 30 minutes to build a new server.
		if (fullyConnectedMesh2.IsHostSystem() && autopatcherServer->GetMaxConurrentUsers()>0)
		{
			MafiaNet::Time curTime = MafiaNet::GetTime();
			if (curTime > timeForNextLoadCheck)
			{
				timeForNextLoadCheck = curTime + LOAD_CHECK_INTERVAL;

				// If the load exceeds such that >=1 new fully loaded server is needed, add that many servers
				// See ID_CLOUD_GET_RESPONSE
				MafiaNet::CloudQuery cloudQuery;
				cloudQuery.keys.Push(MafiaNet::CloudKey("CloudConnCount",0),_FILE_AND_LINE_); // CloudConnCount is defined at the top of CloudServerHelper.cpp
				cloudQuery.subscribeToResults=false;
				cloudClient.Get(&cloudQuery, g_rakPeer->GetMyGUID());
			}
		}

		if (_kbhit())
		{
			ch=_getch();
			if (ch=='q')
				break;
			else if (ch=='c')
			{
				if (connectionObject[0].CreateAutopatcherTables()==false)
					printf("%s", connectionObject[0].GetLastError());
				else
					printf("Created tables.\n");

				if (connectionObject[0].AddApplication(cloudServerHelper->patcherHostSubdomainURL, username)==false)
					printf("%s", connectionObject[0].GetLastError());
				else
					printf("Added application\n");
			}
			else if (ch=='d')
			{
                if (connectionObject[0].DestroyAutopatcherTables()==false)
					printf("%s", connectionObject[0].GetLastError());
			}
			
			else if (ch=='i')
			{
				printf("Image this server?\nWill clone this server so that future servers made from this image will use its current state.\nPress 'y' to image.\n");
				if (_getch()=='y')
				{
					cloudServerHelper->ImageThisServer();
				}
			}
			else if (ch=='s')
			{
				printf("How many servers to spawn? (0-4) ");
				char str[32];
				Gets(str, sizeof(str));
				int num = atoi(str);
				if (num>0)
					cloudServerHelper->SpawnServers(static_cast<unsigned>(num));
			}
			else if (ch=='m')
			{
				char maxConcurrent[64];
				printf("Enter new allowed max concurrent users: ");
				Gets(maxConcurrent, sizeof(maxConcurrent));
				if (maxConcurrent[0])
					SetMaxConcurrentUsers(atoi(maxConcurrent));
			}
			else if (ch=='t')
			{
				printf("Setting max concurrent users to 0 on all servers\n");
				SetMaxConcurrentUsers(0);

				BitStream bsOut;
				bsOut.Write((MessageID)ID_USER_PACKET_ENUM);
				bsOut.Write(cloudServerHelper->serverToServerPassword);
				DataStructures::List<RakNetGUID> remoteServersOut;
				cloudServer.GetRemoteServers(remoteServersOut);
				for (unsigned int i=0; i < remoteServersOut.Size(); i++)
				{
					g_rakPeer->Send(&bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, remoteServersOut[i], false);
					g_rakPeer->CloseConnection(remoteServersOut[i], true);
				}

				printf("Remote servers will shutdown when all users are done\n");
				printf("Disconnected from remote servers\n");
				

				for (unsigned int i=0; i < g_rakPeer->GetNumberOfAddresses(); i++)
				{
					SystemAddress sa = g_rakPeer->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS,i);
					if (sa.IsLANAddress()==false)
					{
						printf("Updating host DNS entry to this server\n");
						cloudServerHelper->UpdateHostIPAsynch(sa);
						break;
					}
				}
			}
			else if (ch=='l')
			{
				cloudServerHelper->GetCustomImages();
			}
			else if (ch=='u')
			{
				// printf("Enter application name: ");
				char appName[512];
				strcpy_s(appName, cloudServerHelper->patcherHostSubdomainURL);
// 				Gets(appName,sizeof(appName));
// 				if (appName[0]==0)
// 					strcpy_s(appName, "TestApp");

				printf("Enter application directory: ");
				char appDir[512];
				Gets(appDir,sizeof(appDir));
				if (appDir[0]==0)
					strcpy_s(appDir, "D:/temp");

				if (connectionObject[0].UpdateApplicationFiles(appName, appDir, username, 0)==false)
				{
					printf("%s", connectionObject[0].GetLastError());
				}
				else
				{
					printf("Update success.\n");
					autopatcherServer->CacheMostRecentPatch(appName);
				}
			}
		}

		RakSleep(30);
	}



	MafiaNet::OP_DELETE_ARRAY(connectionObject, _FILE_AND_LINE_);
	MafiaNet::OP_DELETE_ARRAY(connectionObjectAddresses, _FILE_AND_LINE_);
	packetizedTCP.Stop();
	MafiaNet::RakPeerInterface::DestroyInstance(g_rakPeer);
	MafiaNet::OP_DELETE(cloudServerHelper, _FILE_AND_LINE_);


return 0;
}
