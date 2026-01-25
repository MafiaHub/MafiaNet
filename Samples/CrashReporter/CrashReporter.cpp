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

// To compile link with Dbghelp.lib
// The callstack in release is the same as usual, which means it isn't all that accurate.
#ifdef WIN32

#include <stdio.h>
#include "mafianet/WindowsIncludes.h"
#pragma warning(push)
// disable warning 4091 (triggers for enum typedefs in DbgHelp.h in Windows SDK 7.1 and Windows SDK 8.1)
#pragma warning(disable:4091)
#include <DbgHelp.h>
#pragma warning(pop)
#include <stdlib.h>
#include <time.h>
#include "SendFileTo.h"
#include "CrashReporter.h"
#include "mafianet/EmailSender.h"
#include "mafianet/FileList.h"
#include "mafianet/FileOperations.h"
#include "mafianet/SimpleMutex.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

using namespace MafiaNet;

CrashReportControls CrashReporter::controls;

// More info at:
// http://www.codeproject.com/debug/postmortemdebug_standalone1.asp
// http://www.codeproject.com/debug/XCrashReportPt3.asp
// http://www.codeproject.com/debug/XCrashReportPt1.asp
// http://www.microsoft.com/msj/0898/bugslayer0898.aspx

LONG ProcessException(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
	char appDescriptor[_MAX_PATH];
	if ((CrashReporter::controls.actionToTake & AOC_SILENT_MODE) == 0)
	{
		sprintf_s(appDescriptor, "%s has crashed.\nGenerate a report?",  CrashReporter::controls.appName);
		if (::MessageBoxA(nullptr, appDescriptor, "Crash Reporter", MB_YESNO )==IDNO)
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	char dumpFilepath[_MAX_PATH];
	char dumpFilename[_MAX_PATH];
	sprintf_s(appDescriptor, "%s %s - %s %s", CrashReporter::controls.appName, CrashReporter::controls.appVersion, __DATE__, __TIME__);

	if ((CrashReporter::controls.actionToTake & AOC_EMAIL_WITH_ATTACHMENT) ||
		(CrashReporter::controls.actionToTake & AOC_WRITE_TO_DISK)
		)
	{
		if (CrashReporter::controls.actionToTake & AOC_WRITE_TO_DISK)
		{
			strcpy_s(dumpFilepath, CrashReporter::controls.pathToMinidump);
			WriteFileWithDirectories(dumpFilepath,0,0);
			AddSlash(dumpFilepath);
		}
		else
		{
			// Write to a temporary directory if the user doesn't want the dump on the harddrive.
			if (!GetTempPathA( _MAX_PATH, dumpFilepath ))
				dumpFilepath[0]=0;
		}
		unsigned i, dumpFilenameLen;
		strcpy_s(dumpFilename, appDescriptor);
		dumpFilenameLen=(unsigned) strlen(appDescriptor);
		for (i=0; i < dumpFilenameLen; i++)
			if (dumpFilename[i]==':' || dumpFilename[i]=='/' || dumpFilename[i]=='\\')
				dumpFilename[i]='.'; // Remove illegal characters from filename
		strcat_s(dumpFilepath, dumpFilename);
		strcat_s(dumpFilepath, ".dmp");

		HANDLE hFile = CreateFileA(dumpFilepath,GENERIC_WRITE, FILE_SHARE_READ, nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile==INVALID_HANDLE_VALUE)
			return EXCEPTION_CONTINUE_SEARCH;

		MINIDUMP_EXCEPTION_INFORMATION eInfo;
		eInfo.ThreadId = GetCurrentThreadId();
		eInfo.ExceptionPointers = ExceptionInfo;
		eInfo.ClientPointers = FALSE;

		if (MiniDumpWriteDump(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			(MINIDUMP_TYPE)CrashReporter::controls.minidumpType,
			ExceptionInfo ? &eInfo : nullptr,
			nullptr,
			nullptr)==false)
			return EXCEPTION_CONTINUE_SEARCH;

		CloseHandle(hFile);
	}

	char silentModeEmailBody[1024];
	char subject[1204];
	if (CrashReporter::controls.actionToTake & AOC_EMAIL_NO_ATTACHMENT)
	{
		strcpy_s(subject, CrashReporter::controls.emailSubjectPrefix);
		strcat_s(subject, appDescriptor);

		if (CrashReporter::controls.actionToTake & AOC_SILENT_MODE)
		{
		sprintf_s(silentModeEmailBody, "%s%s version %s has crashed.\r\nIt was compiled on %s %s.\r\n", CrashReporter::controls.emailBody, CrashReporter::controls.appName,CrashReporter::controls.appVersion, __DATE__, __TIME__);

			if (CrashReporter::controls.actionToTake & AOC_WRITE_TO_DISK)
				sprintf_s(silentModeEmailBody+strlen(silentModeEmailBody), 1024-strlen(silentModeEmailBody), "Minidump written to %s \r\n", dumpFilepath);

			// Silently send email with attachment
			EmailSender emailSender;
			emailSender.Send(CrashReporter::controls.SMTPServer,
				25,
				CrashReporter::controls.SMTPAccountName,
				CrashReporter::controls.emailRecipient,
				CrashReporter::controls.emailSender,
				CrashReporter::controls.emailRecipient,
				subject,
				silentModeEmailBody,
				0,
				false,
				CrashReporter::controls.emailPassword);
		}
		else
		{
			CSendFileTo sendFile;
			sendFile.SendMail(0, 0, 0, subject, CrashReporter::controls.emailBody, CrashReporter::controls.emailRecipient);
		}
	}
	else if (CrashReporter::controls.actionToTake & AOC_EMAIL_WITH_ATTACHMENT)
	{
		strcpy_s(subject, CrashReporter::controls.emailSubjectPrefix);
		strcat_s(subject, dumpFilename);
		strcat_s(dumpFilename, ".dmp");

		if (CrashReporter::controls.actionToTake & AOC_SILENT_MODE)
		{
			sprintf_s(silentModeEmailBody, "%s%s version %s has crashed.\r\nIt was compiled on %s %s.\r\n", CrashReporter::controls.emailBody, CrashReporter::controls.appName,CrashReporter::controls.appVersion, __DATE__, __TIME__);

			if (CrashReporter::controls.actionToTake & AOC_WRITE_TO_DISK)
				sprintf_s(silentModeEmailBody+strlen(silentModeEmailBody), 1024-strlen(silentModeEmailBody), "Minidump written to %s \r\n", dumpFilepath);

			// Silently send email with attachment
			EmailSender emailSender;
			FileList files;
			files.AddFile(dumpFilepath,dumpFilename,FileListNodeContext(0,0,0,0));
			emailSender.Send(CrashReporter::controls.SMTPServer,
				25,
				CrashReporter::controls.SMTPAccountName,
				CrashReporter::controls.emailRecipient,
				CrashReporter::controls.emailSender,
				CrashReporter::controls.emailRecipient,
				subject,
				silentModeEmailBody,
				&files,
				false,
				CrashReporter::controls.emailPassword);
		}
		else
		{
			CSendFileTo sendFile;
			sendFile.SendMail(0, dumpFilepath, dumpFilename, subject, CrashReporter::controls.emailBody, CrashReporter::controls.emailRecipient);
		}
	}

	return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI CrashExceptionFilter( struct _EXCEPTION_POINTERS *ExceptionInfo )
{
	// Mutex here due to http://www.jenkinssoftware.com/raknet/forum/index.php?topic=2305.0;topicseen
	static SimpleMutex crashExceptionFilterMutex;
	crashExceptionFilterMutex.Lock();
	LONG retVal = ProcessException(ExceptionInfo); 
	crashExceptionFilterMutex.Unlock();
	return retVal;
}

void DumpMiniDump(PEXCEPTION_POINTERS excpInfo)
{
	if (excpInfo == nullptr)
	{
		// Generate exception to get proper context in dump
		__try 
		{
			RaiseException(EXCEPTION_BREAKPOINT, 0, 0, nullptr);
		} 
		__except(DumpMiniDump(GetExceptionInformation()),EXCEPTION_EXECUTE_HANDLER) 
		{
		}
	} 
	else
	{
		ProcessException(excpInfo); 
	}
}

// #define _DEBUG_CRASH_REPORTER

void CrashReporter::Start(CrashReportControls *input)
{
	memcpy(&controls, input, sizeof(CrashReportControls));

#ifndef _DEBUG_CRASH_REPORTER
	SetUnhandledExceptionFilter(CrashExceptionFilter);
#endif
}
#endif //WIN32
