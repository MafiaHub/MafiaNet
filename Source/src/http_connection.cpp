/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

/// \file
/// \brief Contains HTTPConnection, used to communicate with web servers
///

#include "mafianet/native_feature_includes.h"
#include "mafianet/format_string.h"
#if _RAKNET_SUPPORT_HTTPConnection == 1 && _RAKNET_SUPPORT_TCPInterface == 1

#include "mafianet/tcp_interface.h"
#include "mafianet/http_connection.h"
#include "mafianet/sleep.h"
#include <string>
#include "mafianet/assert.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <charconv>
#include <string_view>
#include <stdlib.h>

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(HTTPConnection, HTTPConnection);

HTTPConnection::HTTPConnection()
    : connectionState(CS_NONE)
{
	tcp = 0;
}

void HTTPConnection::Init(TCPInterface *_tcp, const char *_host, unsigned short _port)
{
	tcp = _tcp;
	host = _host ? _host : "";
	port = _port;
}

void HTTPConnection::Post(const char *remote_path, const char *data, const char *_contentType)
{
	OutgoingCommand op;
	op.contentType = _contentType ? _contentType : "";
	op.data = data ? data : "";
	op.remotePath = remote_path ? remote_path : "";
	op.isPost = true;
	outgoingCommand.push_back(op);
	//printf("Adding outgoing post\n");
}

void HTTPConnection::Get(const char *path)
{
	OutgoingCommand op;
	op.remotePath = path ? path : "";
	op.isPost = false;
	outgoingCommand.push_back(op);
}

bool HTTPConnection::HasBadResponse(int *code, std::string *data)
{
	if (badResponses.empty())
		return false;

	// Always consumed: a caller that passes no data pointer used to see the same response forever.
	if (code)
		*code = badResponses.front().code;
	if (data)
		*data = badResponses.front().data;
	badResponses.pop_front();
	return true;
}
void HTTPConnection::CloseConnection()
{
	connectionState = CS_DISCONNECTING;
}
void HTTPConnection::Update(void)
{
	SystemAddress sa;
	sa = tcp->HasCompletedConnectionAttempt();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		//		printf("Connected\n");
		connectionState = CS_CONNECTED;
		server = sa;
		sa = tcp->HasCompletedConnectionAttempt();
	}

	sa = tcp->HasFailedConnectionAttempt();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		//printf("Failed connected\n");
		CloseConnection();
		sa = tcp->HasFailedConnectionAttempt();
	}

	sa = tcp->HasLostConnection();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		//printf("Lost connection\n");
		CloseConnection();
		sa = tcp->HasLostConnection();
	}


	switch (connectionState)
	{
	case CS_NONE:
	{
		if (outgoingCommand.empty())
			return;

		//printf("Connecting\n");
		server = tcp->Connect(host.c_str(), port, false);
		connectionState = CS_CONNECTING;
	}
	break;
	case CS_DISCONNECTING:
	{
		if (tcp->ReceiveHasPackets() == false)
		{
			if (incomingData.empty() == false)
			{
				results.push_back(incomingData);
			}
			incomingData.clear();
			tcp->CloseConnection(server);
			connectionState = CS_NONE;
		}
	}
	break;
	case CS_CONNECTING:
	{
	}
	break;
	case CS_CONNECTED:
	{
		//printf("Connected\n");
		if (outgoingCommand.empty())
		{
			//printf("Closed connection (nothing to do)\n");
			CloseConnection();
			return;
		}

		//printf("Sending request\n");
		currentProcessingCommand = outgoingCommand.front();
		outgoingCommand.pop_front();
		std::string request;
		if (currentProcessingCommand.isPost)
		{
			request = MafiaNet::FormatText("POST %s HTTP/1.0\r\n"
			                               "Host: %s:%i\r\n"
			                               "Content-Type: %s\r\n"
			                               "Content-Length: %u\r\n"
			                               "\r\n"
			                               "%s",
			                               currentProcessingCommand.remotePath.c_str(),
			                               host.c_str(),
			                               port,
			                               currentProcessingCommand.contentType.c_str(),
			                               (unsigned) currentProcessingCommand.data.size(),
			                               currentProcessingCommand.data.c_str());
		}
		else
		{
			// request = MafiaNet::FormatText("GET %s\r\n", host.c_str());
			// http://www.jenkinssoftware.com/forum/index.php?topic=4601.0;topicseen
			request = MafiaNet::FormatText("GET %s HTTP/1.0\r\n"
			                               "Host: %s:%i\r\n"
			                               "\r\n",
			                               currentProcessingCommand.remotePath.c_str(),
			                               host.c_str(),
			                               port);
		}

		//	printf(request.c_str());
		//		request.URLEncode();
		tcp->Send(request.c_str(), (unsigned int) request.size(), server, false);
		connectionState = CS_PROCESSING;
	}
	break;
	case CS_PROCESSING:
	{
	}
	}

	//	if (connectionState==CS_PROCESSING && currentProcessingCommand.data.IsEmpty()==false)
	//		outgoingCommand.push_front(currentProcessingCommand);
}
bool HTTPConnection::HasRead(void) const
{
	return !results.empty();
}
std::string HTTPConnection::Read(void)
{
	if (results.empty())
		return {};

	std::string resultStr = std::move(results.front());
	results.pop_front();
	const auto body = resultStr.find_first_of("\001\002\003%");
	if (body != std::string::npos)
		resultStr.erase(0, body);
	return resultStr;
}
SystemAddress HTTPConnection::GetServerAddress(void) const
{
	return server;
}
void HTTPConnection::ProcessTCPPacket(Packet *packet)
{
	RakAssert(packet);

	// read all the packets possible
	if (packet->systemAddress == server)
	{
		if (incomingData.size() == 0)
		{
			int response_code = atoi((char *) packet->data + strlen("HTTP/1.0 "));

			if (response_code > 299)
			{
				badResponses.push_back(BadResponse(packet->data, response_code));
				//printf("Closed connection (Bad response 2)\n");
				CloseConnection();
				return;
			}
		}

		const std::string_view chunk(reinterpret_cast<const char *>(packet->data), packet->length);
		for (size_t i = 0; i < chunk.size(); ++i)
		{
			unsigned int byte;
			if (chunk[i] == '%' && chunk.size() - i >= 3)
			{
				const char *end = chunk.data() + i + 3;
				auto parsed = std::from_chars(end - 2, end, byte, 16);
				if (parsed.ec == std::errc{} && parsed.ptr == end)
				{
					incomingData.push_back(static_cast<char>(byte));
					i += 2;
					continue;
				}
			}
			incomingData.push_back(chunk[i]);
		}

		//	printf((const char*) packet->data);
		//	printf("\n");

		RakAssert(strlen((char *) packet->data) == packet->length); // otherwise it contains Null bytes

		const char *start_of_body = strstr(incomingData.c_str(), "\r\n\r\n");

		// besides having the server close the connection, they may
		// provide a length header and supply that many bytes
		if (
		    // Why was start_of_body here? Makes the GET command fail
		    // start_of_body &&
		    connectionState == CS_PROCESSING)
		{
			/*
			// The stupid programmer that wrote this originally didn't think that just because the header contains this value doesn't mean you got the whole message
			if (strstr((const char*) packet->data, "\r\nConnection: close\r\n"))
			{
				CloseConnection();
			}
			else
			{
			*/
			long length_of_headers;
			if (start_of_body)
			{
				length_of_headers = (long) (start_of_body + 4 - incomingData.c_str());
				const char *length_header = strstr(incomingData.c_str(), "\r\nLength: ");

				if (length_header)
				{
					long length = atol(length_header + 10) + length_of_headers;

					if ((long) incomingData.size() >= length)
					{
						//printf("Closed connection (Got all data due to length header)\n");
						CloseConnection();
					}
				}
			}
			else
			{
				// No processing needed
			}


			//}
		}
	}
}

bool HTTPConnection::IsBusy(void) const
{
	return connectionState != CS_NONE;
}

int HTTPConnection::GetState(void) const
{
	return connectionState;
}


HTTPConnection::~HTTPConnection(void)
{
	if (tcp)
		tcp->CloseConnection(server);
}


#endif // _RAKNET_SUPPORT_*
