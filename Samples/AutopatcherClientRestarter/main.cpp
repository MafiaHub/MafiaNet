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
#if defined(_WIN32)
#include "mafianet/WindowsIncludes.h" // Sleep and CreateProcess
#include <process.h> // system
#else
#include <unistd.h> // usleep
#include <cstdio>
#include <signal.h>  //kill
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

#include <iostream>

using namespace std;

// This is a simple tool to take the output of PatchApplication::restartOutputFilename
// Perform file operations while that application is not running, and then relaunch that application
int main(int argc, char **argv)
{
	// Run commands on argv[1] and launch argv[2];
	// Run commands on argv[1] and launch argv[2];
	if (argc!=2)
	{
		printf("Usage: FileContainingCommands\n");
		return 1;
	}

	bool deleteFile=false;
	FILE *fp;
	if (fopen_s(&fp, argv[1], "rt") != 0)
	{
		printf("Error: Cannot open %s\n", argv[1]);
		return 1;
	}

	char buff[256];
	if (fgets(buff,255,fp)==0)
		return 1;
	buff[strlen(buff)]=0;
	while (buff[0])
	{
		if (strncmp(buff, "#Sleep ", 7)==0)
		{
			int sleepTime=atoi(buff+7);
#ifdef _WIN32
			Sleep(sleepTime);
#else
			usleep(sleepTime * 1000);
#endif
		}
		else if (strncmp(buff, "#DeleteThisFile", 15)==0)
			deleteFile=true;
		else if (strncmp(buff, "#CreateProcess ", 15)==0)
		{
#ifdef _WIN32
			PROCESS_INFORMATION pi;
			STARTUPINFOA si;

			// Set up the start up info struct.
			memset(&si, 0,  sizeof(STARTUPINFOA));
			si.cb = sizeof(STARTUPINFOA);

			// Launch the child process.
			if (!CreateProcessA(
				nullptr,
				buff+15,
				nullptr, nullptr,
				TRUE,
				CREATE_NEW_CONSOLE,
				nullptr, nullptr,
				&si,
				&pi))
				return 1;

			CloseHandle( pi.hProcess );
			CloseHandle( pi.hThread );
#else
            char PathName[255];

            strcpy_s(PathName, buff+15);

            system(PathName);  //This actually runs the application.
#endif
		}
		else
		{
            system(buff);
		}

		if (fgets(buff,255,fp)==0)
			break;
		buff[strlen(buff)]=0;
	}

	fclose(fp);

	// Done!
	if (deleteFile)
	{
#ifdef _WIN32
		_unlink(argv[1]);
#else
		unlink(argv[1]);
#endif
	}
}
