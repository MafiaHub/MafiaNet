@echo off


REM Copyright (c) 2018, SLikeSoft UG (haftungsbeschränkt)
REM
REM This file is licensed under the MIT-style license found in the license.txt
REM file in the root directory of this source tree.

SETLOCAL ENABLEDELAYEDEXPANSION

REM import mandatory command line parameters first
set rootDir=%~1
set swigPath=%~2

REM verify mandatory parameters
if "%rootDir%" == "" goto syntaxError

REM remove trailing backslashes from input paths
if "%rootDir:~-1%" == "\" set rootDir=%rootDir:~0,-1%
if not "%swigPath%" == "" (
	if "!swigPath:~-1!" == "\" set swigPath=!swigPath:~0,-1!
)

REM normalize swigPath
REM only add trailing \ to path, if a path is specified (otherwise swigCommand should become "swig.exe" only to ensure it's looking it up via PATH)
if not "%swigPath%" == "" set swigPath=%swigPath%\

REM initialize variables to defaults
set rakNetCompatibilityMode=0
set swigCommand="%swigPath%swig.exe"
set sourceDir=%rootDir%\Source
set dependentExtensionDir=%rootDir%\DependentExtensions
set swigDefines=
set swigIncludes=-I"%sourceDir%\include\mafianet" -I"SwigInterfaceFiles"
set namespace=SLNet
set outputDirectory=..\..\bindings\csharp\interfaces
set outputWrapperFilename=..\..\bindings\csharp\wrapper\mafianet_wrapper.cpp

REM check if we have a dependent extension specified in the 3rd argument
set dependentExtension=%3
if not "%dependentExtension:~0,1%" == "-" (
	REM parameter doesn't start with a "-", hence it's a dependent extension
	REM shift the parameters, so that the 3rd parameter becomes the option parameter
	shift

	REM parse/verify the dependent extension parameter
	REM do this right here, so we abort the processing in case of an invalid dependent extension and report such error
	REM (i.e. report such an error before we'd report a potention syntax error with the option parameter
	if "%dependentExtension%" == "SQLITE" (
		set swigIncludes=%swigIncludes% -I"%dependentExtensionDir%\SQLite3Plugin"
		set swigDefines=%swigDefines% -DSWIG_ADDITIONAL_SQL_LITE
	) else if "%dependentExtension%" == "MYSQL_AUTOPATCHER" (
		set swigIncludes=%swigIncludes% -I"%dependentExtensionDir%\Autopatcher"
		set swigDefines=%swigDefines% -DSWIG_ADDITIONAL_AUTOPATCHER_MYSQL
	) else if not "%dependentExtension%" == "" (
		echo Unsupported dependent extension: '%dependentExtension%'
		goto syntaxError
	)
)

REM parse/verify option parameter
:parameterProcessing
if "%3" == "" ( 
	goto processingComplete
) else if "%3" == "--rakNetCompatibility" (
	set rakNetCompatibilityMode=1
) else (
	echo Unsupported option: '%3'
	goto syntaxError
)
shift
goto parameterProcessing
:processingComplete


REM adjust variables for RakNet compatibility mode
if %rakNetCompatibilityMode% == 1 (
	set namespace=RakNet
	set swigDefines=%swigDefines% -DRAKNET_COMPATIBILITY
	set outputDirectory=..\..\bindings\raknet_backwards_compatibility\csharp\interfaces
	set outputWrapperFilename=..\..\bindings\raknet_backwards_compatibility\csharp\wrapper\RakNet_wrap.cxx
)

echo Performing SWIG build

REM clear output folder
del /F /Q %outputDirectory%\*

REM run SWIG
%swigCommand% -c++ -csharp -namespace %namespace% %swigIncludes% %swigDefines% -outdir %outputDirectory% -o %outputWrapperFilename% SwigInterfaceFiles\RakNet.i
if errorlevel 1 goto swigError

echo SWIG build complete
goto :eof

:syntaxError
echo Usage:
echo   MakeSwig.bat ^<mafianet_root_path^> ^<swig_path^>
echo     [^<dependent_extension^>] [--rakNetCompatibility]
echo.
echo   mafianet_root_path  Path to the MafiaNet root directory.
echo   swig_path           Path to the SWIG binary (swig.exe). Use "" to
echo                       indicate using swig.exe from the PATH environment
echo                       variable.
echo   dependent_extension The dependent extension which should be included.
echo                       Supported values: MYSQL_AUTOPATCHER, SQLITE
echo.
echo Options:
echo   --rakNetCompatibility If specified, creates the C# wrapper in RakNet
echo                         compatibility mode.
exit /b 1

:swigError
echo SWIG had an error during build
exit /b 2