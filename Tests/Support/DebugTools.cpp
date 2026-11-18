/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "DebugTools.h"
#include <cstdio>

DebugTools::DebugTools(void)
{
}

DebugTools::~DebugTools(void)
{
}

void DebugTools::ShowError(const char *errorString, bool pause, unsigned int lineNum, const char *fileName)
{

	fflush(stdin);

	printf("%s\nFile:%s \nLine: %i\n", errorString, fileName, lineNum);

	if (pause)
	{
		printf("Press enter to continue \n");
		getchar();
	}
}
