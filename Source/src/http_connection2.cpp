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

#include "mafianet/native_feature_includes.h"
#include <cstring>
#if _RAKNET_SUPPORT_HTTPConnection2 == 1 && _RAKNET_SUPPORT_TCPInterface == 1

#include "mafianet/http_connection2.h"
#include <charconv>
#include "mafianet/tcp_interface.h"
#include "mafianet/assert.h"
#include "mafianet/vector_util.h"

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(HTTPConnection2, HTTPConnection2);

HTTPConnection2::HTTPConnection2()
{
}
HTTPConnection2::~HTTPConnection2()
{
	for (unsigned int i = 0; i < pendingRequests.size(); ++i)
	{
		delete pendingRequests[i];
	}
	for (unsigned int i = 0; i < sentRequests.size(); ++i)
	{
		delete sentRequests[i];
	}
	for (unsigned int i = 0; i < completedRequests.size(); ++i)
	{
		delete completedRequests[i];
	}
}
bool HTTPConnection2::TransmitRequest(const char *stringToTransmit, const char *host, unsigned short port, int ipVersion, SystemAddress useAddress, void *userData)
{
	if (!stringToTransmit || !host)
		return false;
	Request *request = new Request;
	request->host = host;
	request->chunked = false;
	if (useAddress != UNASSIGNED_SYSTEM_ADDRESS)
	{
		request->hostEstimatedAddress = useAddress;
		if (IsConnected(useAddress) == false)
		{
			delete request;
			return false;
		}
	}
	else
	{
		// #med - this should be changed to not extract the port from the passed in host-address (which is overwritten directly below with the provided port anyway)
		if (request->hostEstimatedAddress.FromString(host, '|', ipVersion) == false)
		{
			delete request;
			return false;
		}
	}
	request->hostEstimatedAddress.SetPortHostOrder(port);
	request->port = port;
	request->stringToTransmit = stringToTransmit;
	request->contentLength = -1;
	request->contentOffset = 0;
	request->ipVersion = ipVersion;
	request->userData = userData;

	if (IsConnected(request->hostEstimatedAddress))
	{
		sentRequestsMutex.Lock();
		if (sentRequests.empty())
		{
			request->hostCompletedAddress = request->hostEstimatedAddress;
			sentRequests.push_back(request);
			sentRequestsMutex.Unlock();

			SendRequest(request);
		}
		else
		{
			// Request pending, push it
			pendingRequestsMutex.Lock();
			pendingRequests.push_back(request);
			pendingRequestsMutex.Unlock();

			sentRequestsMutex.Unlock();
		}
	}
	else
	{
		pendingRequestsMutex.Lock();
		pendingRequests.push_back(request);
		pendingRequestsMutex.Unlock();

		if (ipVersion != 6)
		{
			tcpInterface->Connect(host, port, false, AF_INET);
		}
		else
		{
			tcpInterface->Connect(host, port, false, AF_INET6);
		}
	}
	return true;
}
bool HTTPConnection2::GetResponse(std::string &stringTransmitted, std::string &hostTransmitted, std::string &responseReceived, SystemAddress &hostReceived, ptrdiff_t &contentOffset)
{
	void *userData;
	return GetResponse(stringTransmitted, hostTransmitted, responseReceived, hostReceived, contentOffset, &userData);
}
bool HTTPConnection2::GetResponse(std::string &stringTransmitted, std::string &hostTransmitted, std::string &responseReceived, SystemAddress &hostReceived, ptrdiff_t &contentOffset, void **userData)
{
	completedRequestsMutex.Lock();
	if (!completedRequests.empty())
	{
		Request *completedRequest = completedRequests[0];
		SwapRemove(completedRequests, 0);
		completedRequestsMutex.Unlock();

		responseReceived = completedRequest->stringReceived;
		hostReceived = completedRequest->hostCompletedAddress;
		stringTransmitted = completedRequest->stringToTransmit;
		hostTransmitted = completedRequest->host;
		contentOffset = completedRequest->contentOffset;
		*userData = completedRequest->userData;

		delete completedRequest;
		return true;
	}
	else
	{
		completedRequestsMutex.Unlock();
	}
	return false;
}
bool HTTPConnection2::IsBusy(void) const
{
	return !pendingRequests.empty() || !sentRequests.empty();
}
bool HTTPConnection2::HasResponse(void) const
{
	return !completedRequests.empty();
}
int ReadChunkSize(char *txtStart, char **txtEnd)
{
	// 	char lengthStr[32];
	// 	memset(lengthStr, 0, 32);
	// 	memcpy(lengthStr, txtStart, txtEnd - txtStart);
	return strtoul(txtStart, txtEnd, 16);
	// return atoi(lengthStr);
}
void ReadChunkBlock(size_t &currentChunkSize, size_t &bytesReadSoFar, char *txtIn, std::string &txtOut)
{
	size_t bytesToRead;
	size_t sLen;

	do
	{
		bytesToRead = currentChunkSize - bytesReadSoFar;
		sLen = strlen(txtIn);
		if (sLen < bytesToRead)
			bytesToRead = sLen;
		txtOut.append(txtIn, bytesToRead);
		txtIn += bytesToRead;
		bytesReadSoFar += bytesToRead;
		if (*txtIn == 0)
		{
			//	currentChunkSize=0;
			return;
		}
		// char *newLine = strstr(txtIn, "\r\n");
		if (txtIn[0] && txtIn[0] == '\r' && txtIn[1] && txtIn[1] == '\n')
			txtIn += 2; // Newline
		char *newLine;
		currentChunkSize = ReadChunkSize(txtIn, &newLine);
		RakAssert(currentChunkSize < 50000); // Sanity check
		if (currentChunkSize == 0)
			return;
		if (newLine == 0)
			return;
		bytesReadSoFar = 0;
		txtIn = newLine + 2;
	} while (txtIn);
}
PluginReceiveResult HTTPConnection2::OnReceive(Packet *packet)
{
	unsigned int i;

	bool locked = true;
	sentRequestsMutex.Lock();
	for (i = 0; i < sentRequests.size(); i++)
	{
		Request *sentRequest = sentRequests[i];
		if (sentRequest->hostCompletedAddress == packet->systemAddress)
		{
			SwapRemove(sentRequests, i);
			locked = false;
			sentRequestsMutex.Unlock();

			/*
			static FILE * pFile = 0;
			if (pFile==0)
			{
				long lSize;
				char * buffer;
				size_t result;

				pFile = fopen ( "string_received.txt" , "rb" );
				if (pFile==nullptr) {fputs ("File error",stderr); exit (1);}

				// obtain file size:
				fseek (pFile , 0 , SEEK_END);
				lSize = ftell (pFile);
				rewind (pFile);

				// allocate memory to contain the whole file:
				buffer = (char*) malloc (sizeof(char)*lSize);
				if (buffer == nullptr) {fputs ("Memory error",stderr); exit (2);}

				// copy the file into the buffer:
				result = fread (buffer,1,lSize,pFile);
				if (result != lSize) {fputs ("Reading error",stderr); exit (3);}

				packet->data=(unsigned char*) buffer;
				packet->length=lSize;
			}
			*/


			const char *isFirstChunk = strstr((char *) packet->data, "Transfer-Encoding: chunked");
			if (isFirstChunk)
			{
				//printf((char*) packet->data);

				locked = false;
				sentRequestsMutex.Unlock();

				sentRequest->chunked = true;
				char *chunkStrStart = strstr((char *) packet->data, "\r\n\r\n");
				RakAssert(chunkStrStart);

				chunkStrStart += 4; // strlen("\r\n\r\n");
				char *body_header;  // = strstr(chunkStrStart, "\r\n");
				sentRequest->thisChunkSize = ReadChunkSize(chunkStrStart, &body_header);
				sentRequest->bytesReadForThisChunk = 0;
				sentRequest->contentOffset = 0;

				if (sentRequest->thisChunkSize == 0)
				{
					// Done
					completedRequestsMutex.Lock();
					completedRequests.push_back(sentRequest);
					completedRequestsMutex.Unlock();

					// If there is another command waiting for this server, send it
					SendPendingRequestToConnectedSystem(packet->systemAddress);
				}
				else
				{

					// char *offset = strstr((char*) packet->data+1, "2000");

					body_header += 2;
					ReadChunkBlock(sentRequest->thisChunkSize, sentRequest->bytesReadForThisChunk, body_header, sentRequest->stringReceived);

					if (sentRequest->thisChunkSize == 0)
					{
						// Done
						completedRequestsMutex.Lock();
						completedRequests.push_back(sentRequest);
						completedRequestsMutex.Unlock();

						// If there is another command waiting for this server, send it
						SendPendingRequestToConnectedSystem(packet->systemAddress);
					}
					else
					{
						// Not done
						sentRequestsMutex.Lock();
						sentRequests.push_back(sentRequest);
						sentRequestsMutex.Unlock();
					}
				}
			}
			else if (sentRequest->chunked)
			{
				ReadChunkBlock(sentRequest->thisChunkSize, sentRequest->bytesReadForThisChunk, (char *) packet->data, sentRequest->stringReceived);

				if (sentRequest->thisChunkSize == 0)
				{
					// Done
					completedRequestsMutex.Lock();
					completedRequests.push_back(sentRequest);
					completedRequestsMutex.Unlock();

					// If there is another command waiting for this server, send it
					SendPendingRequestToConnectedSystem(packet->systemAddress);
				}
				else
				{
					// Not done
					sentRequestsMutex.Lock();
					sentRequests.push_back(sentRequest);
					sentRequestsMutex.Unlock();
				}
			}
			else
			{
				sentRequest->stringReceived.append(reinterpret_cast<const char *>(packet->data), packet->length);

				if (sentRequest->contentLength == -1)
				{
					const char *length_header = strstr(sentRequest->stringReceived.c_str(), "Content-Length: ");
					if (length_header)
					{
						length_header += sizeof("Content-Length: ") - 1;

						unsigned int clLength;
						for (clLength = 0; length_header[clLength] && length_header[clLength] >= '0' && length_header[clLength] <= '9'; clLength++)
							;
						if (clLength > 0 && (length_header[clLength] == '\r' || length_header[clLength] == '\n'))
						{
							std::from_chars(length_header, length_header + clLength, sentRequest->contentLength);
						}
					}
				}

				// If we know the content length, find \r\n\r\n
				if (sentRequest->contentLength != -1)
				{
					if (sentRequest->contentLength > 0)
					{
						const char *body_header = strstr(sentRequest->stringReceived.c_str(), "\r\n\r\n");
						if (body_header)
						{
							body_header += 4; // strlen("\r\n\r\n");
							size_t slen = sentRequest->stringReceived.size() - static_cast<size_t>(body_header - sentRequest->stringReceived.data());
							//RakAssert(slen <= (size_t) sentRequest->contentLength);
							if (slen >= (size_t) sentRequest->contentLength)
							{
								sentRequest->contentOffset = body_header - sentRequest->stringReceived.c_str();
								completedRequestsMutex.Lock();
								completedRequests.push_back(sentRequest);
								completedRequestsMutex.Unlock();

								// If there is another command waiting for this server, send it
								SendPendingRequestToConnectedSystem(packet->systemAddress);
							}
							else
							{
								sentRequestsMutex.Lock();
								sentRequests.push_back(sentRequest);
								sentRequestsMutex.Unlock();
							}
						}

						else
						{
							sentRequestsMutex.Lock();
							sentRequests.push_back(sentRequest);
							sentRequestsMutex.Unlock();
						}
					}
					else
					{
						sentRequest->contentOffset = -1;
						completedRequestsMutex.Lock();
						completedRequests.push_back(sentRequest);
						completedRequestsMutex.Unlock();

						// If there is another command waiting for this server, send it
						SendPendingRequestToConnectedSystem(packet->systemAddress);
					}
				}
				else
				{
					const char *firstNewlineSet = strstr(sentRequest->stringReceived.c_str(), "\r\n\r\n");
					if (firstNewlineSet != 0)
					{
						ptrdiff_t offset = firstNewlineSet - sentRequest->stringReceived.c_str();
						if (sentRequest->stringReceived.c_str()[offset + 4] == 0)
							sentRequest->contentOffset = -1;
						else
							sentRequest->contentOffset = offset + 4;
						completedRequestsMutex.Lock();
						completedRequests.push_back(sentRequest);
						completedRequestsMutex.Unlock();

						// If there is another command waiting for this server, send it
						SendPendingRequestToConnectedSystem(packet->systemAddress);
					}
					else
					{
						sentRequestsMutex.Lock();
						sentRequests.push_back(sentRequest);
						sentRequestsMutex.Unlock();
					}
				}
			}


			break;
		}
	}

	if (locked == true)
		sentRequestsMutex.Unlock();

	return RR_CONTINUE_PROCESSING;
}

void HTTPConnection2::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
	(void) rakNetGUID;
	(void) isIncoming; // unknown

	SendPendingRequestToConnectedSystem(systemAddress);
}
void HTTPConnection2::SendPendingRequestToConnectedSystem(SystemAddress sa)
{
	if (sa == UNASSIGNED_SYSTEM_ADDRESS)
		return;

	unsigned int requestsSent = 0;

	// Search through requests to find a match for this instance of TCPInterface and SystemAddress
	unsigned int i;
	i = 0;
	pendingRequestsMutex.Lock();
	while (i < pendingRequests.size())
	{
		Request *request = pendingRequests[i];
		if (request->hostEstimatedAddress == sa)
		{
			pendingRequests.erase(pendingRequests.begin() + i);
			// Send this request
			request->hostCompletedAddress = sa;

			sentRequestsMutex.Lock();
			sentRequests.push_back(request);
			sentRequestsMutex.Unlock();

			pendingRequestsMutex.Unlock();

			SendRequest(request);
			requestsSent++;
			pendingRequestsMutex.Lock();
			break;
		}
		else
		{
			i++;
		}
	}
	pendingRequestsMutex.Unlock();

	if (requestsSent == 0)
	{
		pendingRequestsMutex.Lock();
		if (!pendingRequests.empty())
		{
			// Just assign
			Request *request = pendingRequests[0];
			pendingRequests.erase(pendingRequests.begin() + 0);

			request->hostCompletedAddress = sa;

			sentRequestsMutex.Lock();
			sentRequests.push_back(request);
			sentRequestsMutex.Unlock();
			pendingRequestsMutex.Unlock();

			SendRequest(request);
		}
		else
		{
			pendingRequestsMutex.Unlock();
		}
	}
}
void HTTPConnection2::RemovePendingRequest(SystemAddress sa)
{
	unsigned int i;
	i = 0;
	pendingRequestsMutex.Lock();
	// A while loop: the old for loop also advanced past the entry after each removal.
	while (i < pendingRequests.size())
	{
		Request *request = pendingRequests[i];
		if (request->hostEstimatedAddress == sa)
		{
			pendingRequests.erase(pendingRequests.begin() + i);
			delete request;
		}
		else
			i++;
	}

	pendingRequestsMutex.Unlock();
}
void HTTPConnection2::SendNextPendingRequest(void)
{
	// Send a pending request
	pendingRequestsMutex.Lock();
	if (!pendingRequests.empty())
	{
		Request *pendingRequest = pendingRequests.front();
		pendingRequestsMutex.Unlock();

		if (pendingRequest->ipVersion != 6)
		{
			tcpInterface->Connect(pendingRequest->host.c_str(), pendingRequest->port, false, AF_INET);
		}
		else
		{
			tcpInterface->Connect(pendingRequest->host.c_str(), pendingRequest->port, false, AF_INET6);
		}
	}
	else
	{
		pendingRequestsMutex.Unlock();
	}
}

void HTTPConnection2::OnFailedConnectionAttempt(Packet *packet, PI2_FailedConnectionAttemptReason failedConnectionAttemptReason)
{
	(void) failedConnectionAttemptReason;
	if (packet->systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return;

	RemovePendingRequest(packet->systemAddress);

	SendNextPendingRequest();
}
void HTTPConnection2::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) rakNetGUID;

	if (systemAddress == UNASSIGNED_SYSTEM_ADDRESS)
		return;

	// Update sent requests to completed requests
	unsigned int i;
	i = 0;
	sentRequestsMutex.Lock();
	while (i < sentRequests.size())
	{
		if (sentRequests[i]->hostCompletedAddress == systemAddress)
		{
			Request *sentRequest = sentRequests[i];
			if (sentRequest->chunked == false && sentRequest->stringReceived.empty() == false)
			{
				if (strstr(sentRequest->stringReceived.c_str(), "Content-Length: "))
				{
					char *body_header = strstr((char *) sentRequest->stringReceived.c_str(), "\r\n\r\n");
					if (body_header)
					{
						body_header += 4; // strlen("\r\n\r\n");
						sentRequest->contentOffset = body_header - sentRequest->stringReceived.c_str();
					}
					else
					{
						sentRequest->contentOffset = 0;
					}
				}
				else
				{
					sentRequest->contentOffset = 0;
				}
			}


			completedRequestsMutex.Lock();
			completedRequests.push_back(sentRequests[i]);
			completedRequestsMutex.Unlock();

			SwapRemove(sentRequests, i);
		}
		else
		{
			i++;
		}
	}
	sentRequestsMutex.Unlock();

	SendNextPendingRequest();
}
bool HTTPConnection2::IsConnected(SystemAddress sa)
{
	SystemAddress remoteSystems[64];
	unsigned short numberOfSystems = 64;
	tcpInterface->GetConnectionList(remoteSystems, &numberOfSystems);
	for (unsigned int i = 0; i < numberOfSystems; i++)
	{
		if (remoteSystems[i] == sa)
		{
			return true;
		}
	}
	return false;
}
void HTTPConnection2::SendRequest(Request *request)
{
	tcpInterface->Send(request->stringToTransmit.c_str(), (unsigned int) request->stringToTransmit.size(), request->hostCompletedAddress, false);
}

#endif // #if _RAKNET_SUPPORT_HTTPConnection2==1 && _RAKNET_SUPPORT_TCPInterface==1
