/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/command_parser_interface.h"
#include "mafianet/transport_interface.h"
#include <string.h>
#include "mafianet/assert.h"
#include <stdio.h>


#if defined(_WIN32)
// IP_DONTFRAGMENT is different between winsock 1 and winsock 2.  Therefore, Winsock2.h must be linked againt Ws2_32.lib
// winsock.h must be linked against WSock32.lib.  If these two are mixed up the flag won't work correctly
#include <winsock2.h>

#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "mafianet/linux_strings.h"

#include <algorithm>

using namespace MafiaNet;

namespace
{
// First command not less than command (case-insensitive): where it is, or would be inserted.
std::vector<RegisteredCommand>::const_iterator LowerBoundCommand(const std::vector<RegisteredCommand> &commandList, const char *command)
{
	return std::lower_bound(commandList.begin(), commandList.end(), command, [](const RegisteredCommand &entry, const char *key)
	                        { return RegisteredCommandComp(key, entry) > 0; });
}
} // namespace

const unsigned char CommandParserInterface::VARIABLE_NUMBER_OF_PARAMETERS = 255;

int MafiaNet::RegisteredCommandComp(const char *const &key, const RegisteredCommand &data)
{
	return _stricmp(key, data.command);
}

CommandParserInterface::CommandParserInterface() {}
CommandParserInterface::~CommandParserInterface() {}

void CommandParserInterface::ParseConsoleString(char *str, const char delineator, unsigned char delineatorToggle, unsigned *numParameters, char **parameterList, unsigned parameterListLength)
{
	unsigned strIndex, parameterListIndex;
	unsigned strLen;
	bool replaceDelineator = true;

	strLen = (unsigned) strlen(str);

	// Replace every instance of delineator, \n, \r with 0
	for (strIndex = 0; strIndex < strLen; strIndex++)
	{
		if (str[strIndex] == delineator && replaceDelineator)
			str[strIndex] = 0;

		if (str[strIndex] == '\n' || str[strIndex] == '\r')
			str[strIndex] = 0;

		if (str[strIndex] == delineatorToggle)
		{
			str[strIndex] = 0;
			replaceDelineator = !replaceDelineator;
		}
	}

	// Fill up parameterList starting at each non-0
	for (strIndex = 0, parameterListIndex = 0; strIndex < strLen;)
	{
		if (str[strIndex] != 0)
		{
			parameterList[parameterListIndex] = str + strIndex;
			parameterListIndex++;
			RakAssert(parameterListIndex < parameterListLength);
			if (parameterListIndex >= parameterListLength)
				break;

			strIndex++;
			while (str[strIndex] != 0 && strIndex < strLen)
				strIndex++;
		}
		else
			strIndex++;
	}

	parameterList[parameterListIndex] = 0;
	*numParameters = parameterListIndex;
}
void CommandParserInterface::SendCommandList(TransportInterface *transport, const SystemAddress &systemAddress)
{
	if (!commandList.empty())
	{
		for (size_t i = 0; i < commandList.size(); i++)
		{
			transport->Send(systemAddress, "%s", commandList[i].command);
			if (i < commandList.size() - 1)
				transport->Send(systemAddress, ", ");
		}
		transport->Send(systemAddress, "\r\n");
	}
	else
		transport->Send(systemAddress, "No registered commands\r\n");
}
void CommandParserInterface::RegisterCommand(unsigned char parameterCount, const char *command, const char *commandHelp)
{
	RegisteredCommand rc;
	rc.command = command;
	rc.commandHelp = commandHelp;
	rc.parameterCount = parameterCount;
	const auto position = LowerBoundCommand(commandList, command);
	if (position != commandList.end() && RegisteredCommandComp(command, *position) == 0)
	{
		RakAssert(0); // Command registered twice
		return;
	}
	commandList.insert(position, rc);
}
bool CommandParserInterface::GetRegisteredCommand(const char *command, RegisteredCommand *rc)
{
	const auto position = LowerBoundCommand(commandList, command);
	if (position == commandList.end() || RegisteredCommandComp(command, *position) != 0)
		return false;
	*rc = *position;
	return true;
}
void CommandParserInterface::OnTransportChange(TransportInterface *transport)
{
	(void) transport;
}
void CommandParserInterface::OnNewIncomingConnection(const SystemAddress &systemAddress, TransportInterface *transport)
{
	(void) systemAddress;
	(void) transport;
}
void CommandParserInterface::OnConnectionLost(const SystemAddress &systemAddress, TransportInterface *transport)
{
	(void) systemAddress;
	(void) transport;
}
void CommandParserInterface::ReturnResult(bool res, const char *command, TransportInterface *transport, const SystemAddress &systemAddress)
{
	if (res)
		transport->Send(systemAddress, "%s returned true.\r\n", command);
	else
		transport->Send(systemAddress, "%s returned false.\r\n", command);
}
void CommandParserInterface::ReturnResult(int res, const char *command, TransportInterface *transport, const SystemAddress &systemAddress)
{
	transport->Send(systemAddress, "%s returned %i.\r\n", command, res);
}
void CommandParserInterface::ReturnResult(const char *command, TransportInterface *transport, const SystemAddress &systemAddress)
{
	transport->Send(systemAddress, "Successfully called %s.\r\n", command);
}
void CommandParserInterface::ReturnResult(char *res, const char *command, TransportInterface *transport, const SystemAddress &systemAddress)
{
	transport->Send(systemAddress, "%s returned %s.\r\n", command, res);
}
void CommandParserInterface::ReturnResult(SystemAddress res, const char *command, TransportInterface *transport, const SystemAddress &systemAddress)
{
	char addr[128];
	systemAddress.ToString(false, addr, static_cast<size_t>(128));
	char addr2[128];
	res.ToString(false, addr2, static_cast<size_t>(128));
	transport->Send(systemAddress, "%s returned %s %s:%i\r\n", command, addr, addr2, res.GetPort());
}
