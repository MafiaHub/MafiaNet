// This is a Demo of the Irrlicht Engine (c) 2005-2008 by N.Gebhardt.
// This file is not documented.

/*
 * This file was taken from RakNet 4.082.
 * Please see licenses/RakNet license.txt for the underlying license and related copyright.
 *
 * Modified work: Copyright (c) 2017-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 * This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 * license found in the license.txt file in the root directory of this source tree.
 * Alternatively you are permitted to license the modifications under the zlib/libpng license.
 */

#ifdef _MSC_VER
	#pragma warning(push)
	#pragma warning(disable:4100) // unreferenced formal parameter
	#pragma warning(disable:4127) // conditional expression is constant
	#pragma warning(disable:4244) // type-conversion with possible loss of data
	#pragma warning(disable:4458) // declaration of 'identifier' hides class member
#endif

#include <irrlicht.h>

#ifdef _MSC_VER
	#pragma warning(pop)
#endif

#ifdef _WIN32__
#include "mafianet/WindowsIncludes.h" // Prevent 'fd_set' : 'struct' type redefinition
#include <windows.h>
#endif

#include <stdio.h>
#include "CMainMenu.h"
#include "CDemo.h"

using namespace irr;

#ifdef _WIN32

#pragma comment(lib, "Irrlicht.lib")
INT WINAPI WinMain( HINSTANCE, HINSTANCE, LPSTR, INT )
#else
int main(int, char*[])
#endif
{
	bool fullscreen = false;
	bool music = true;
	bool shadows = false;
	bool additive = false;
	bool vsync = false;
	bool aa = false;
	core::stringw playerName;

#ifndef _IRR_WINDOWS_
	video::E_DRIVER_TYPE driverType = video::EDT_OPENGL;
#else
	video::E_DRIVER_TYPE driverType = video::EDT_DIRECT3D9;
#endif

	CMainMenu menu;

//#ifndef _DEBUG
	if (menu.run(fullscreen, music, shadows, additive, vsync, aa, driverType, playerName))
//#endif
	{
		CDemo demo(fullscreen, music, shadows, additive, vsync, aa, driverType, playerName);
		demo.run();
	}

	return 0;
}

