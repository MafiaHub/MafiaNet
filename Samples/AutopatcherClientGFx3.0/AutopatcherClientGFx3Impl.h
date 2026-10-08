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

#include "mafianet/types.h"
#include "FxGameDelegate.h"

namespace MafiaNet {

class AutopatcherClient;
class FileListTransfer;
class PacketizedTCP;

// GFxPlayerTinyD3D9.cpp has an instance of this class, and callls the corresponding 3 function
// This keeps the patching code out of the GFx sample as much as possible
class AutopatcherClientGFx3Impl : public FxDelegateHandler
{
public:
	AutopatcherClientGFx3Impl();
	~AutopatcherClientGFx3Impl();
	void Init(const char *_pathToThisExe, GPtr<FxDelegate> pDelegate, GPtr<GFxMovieView> pMovie);
	void Update(void);
	void Shutdown(void);

	// Callback from flash
	static void PressedConnect(const FxDelegateArgs& pparams);
	static void PressedOKBtn(const FxDelegateArgs& pparams);
	static void	PressedPatch(const FxDelegateArgs& pparams);
	static void	OpenSite(const FxDelegateArgs& pparams);

	// Update all callbacks from flash
	void                Accept(CallbackProcessor* cbreg);

	const char* Connect(const char *ip, unsigned short port);
	void UpdateConnectResult(bool isConnected);
	void SaveLastUpdateDate(void);
	void LoadLastUpdateDate(double *out, const char *appDir);
	void GotoMainMenu(void);

	AutopatcherClient *autopatcherClient;
	FileListTransfer *fileListTransfer;
	PacketizedTCP *packetizedTCP;
	SystemAddress serverAddress;
	char pathToThisExe[512];
	char appDirectory[512];
	GPtr<FxDelegate> delegate;
	GPtr<GFxMovieView>      movie;

};

} // namespace MafiaNet
