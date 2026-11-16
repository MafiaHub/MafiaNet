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

#include "Lobby2Server_PGSQL.h"
#include "PostgreSQLInterface.h"

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(Lobby2Server_PGSQL, Lobby2Server_PGSQL);

Lobby2ServerCommand Lobby2ServerWorkerThread(Lobby2ServerCommand input, bool *returnOutput, void *perThreadData)
{
	PostgreSQLInterface *postgreSQLInterface = (PostgreSQLInterface *) perThreadData;
	input.returnToSender = input.lobby2Message->ServerDBImpl(&input, postgreSQLInterface);
	*returnOutput = input.returnToSender;
	if (input.deallocMsgWhenDone && input.returnToSender == false)
		delete input.lobby2Message;
	return input;
}

Lobby2Server_PGSQL::Lobby2Server_PGSQL()
{
}
Lobby2Server_PGSQL::~Lobby2Server_PGSQL()
{
	Clear();
}
void Lobby2Server_PGSQL::AddInputFromThread(Lobby2Message *msg, unsigned int targetUserId, MafiaNet::RakString targetUserHandle)
{
	Lobby2ServerCommand command;
	command.lobby2Message = msg;
	command.deallocMsgWhenDone = true;
	command.returnToSender = true;
	command.callerUserId = targetUserId;
	command.callingUserName = targetUserHandle;
	command.server = this;
	AddInputCommand(command);
}
void Lobby2Server_PGSQL::AddInputCommand(Lobby2ServerCommand command)
{
	threadPool.AddInput(Lobby2ServerWorkerThread, command);
}
void Lobby2Server_PGSQL::AddOutputFromThread(Lobby2Message *msg, unsigned int targetUserId, MafiaNet::RakString targetUserHandle)
{
	Lobby2ServerCommand command;
	command.lobby2Message = msg;
	command.deallocMsgWhenDone = true;
	command.returnToSender = true;
	command.callerUserId = targetUserId;
	command.callingUserName = targetUserHandle;
	command.server = this;
	msg->resultCode = L2RC_SUCCESS;
	threadPool.AddOutput(command);
}
bool Lobby2Server_PGSQL::ConnectToDB(const char *conninfo, int numWorkerThreads)
{
	if (numWorkerThreads <= 0)
		return false;

	StopThreads();

	int i;
	PostgreSQLInterface *connection;
	for (i = 0; i < numWorkerThreads; i++)
	{
		connection = new PostgreSQLInterface;
		if (connection->Connect(conninfo) == false)
		{
			delete connection;
			ClearConnections();
			return false;
		}
		connectionPoolMutex.Lock();
		connectionPool.push_back(connection);
		connectionPoolMutex.Unlock();
	}

	threadPool.SetThreadDataInterface(this, 0);
	threadPool.StartThreads(numWorkerThreads, 0, 0, 0);
	return true;
}
void *Lobby2Server_PGSQL::PerThreadFactory(void *context)
{
	(void) context;

	PostgreSQLInterface *p;
	connectionPoolMutex.Lock();
	p = connectionPool.back();
	connectionPool.pop_back();
	connectionPoolMutex.Unlock();
	return p;
}
void Lobby2Server_PGSQL::PerThreadDestructor(void *factoryResult, void *context)
{
	(void) context;

	PostgreSQLInterface *p = (PostgreSQLInterface *) factoryResult;
	delete p;
}
void Lobby2Server_PGSQL::ClearConnections(void)
{
	unsigned int i;
	connectionPoolMutex.Lock();
	for (i = 0; i < connectionPool.size(); i++)
		delete connectionPool[i];
	connectionPool.clear();
	connectionPoolMutex.Unlock();
}
