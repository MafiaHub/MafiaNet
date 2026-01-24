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

#include "slikenet/WindowsIncludes.h"
#include "FxGameDelegate.h"
#include "slikenet/string.h"

#define ACTIONSCRIPT_CALLABLE_HEADER(functionName) virtual void functionName(const FxDelegateArgs& pparams);
#define ACTIONSCRIPT_CALLABLE_FUNCTION(className, functionName) \
	void functionName(const FxDelegateArgs& pparams) \
{ \
	((className*)pparams.GetHandler())->functionName(pparams);  \
} \
	void className::functionName(const FxDelegateArgs& pparams)

// GFxPlayerTinyD3D9.cpp has an instance of this class, and callls the corresponding 3 function
// This keeps the patching code out of the GFx sample as much as possible
class RoomsBrowserGFx3 : public FxDelegateHandler
{
public:
	RoomsBrowserGFx3();
	virtual ~RoomsBrowserGFx3();
	virtual void Init(GPtr<FxDelegate> pDelegate, GPtr<GFxMovieView> pMovie);

	// Update all callbacks from flash
	void                Accept(CallbackProcessor* cbreg);

	void Shutdown(void);

	virtual const char *QueryPlatform(void) const=0;
	virtual void SaveProperty(const char *propertyId, const char *propertyValue)=0;
	virtual void LoadProperty(const char *propertyId, MafiaNet::RakString &propertyOut)=0;

	// Calls from Flash
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_QueryPlatform);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_SaveProperty);
	ACTIONSCRIPT_CALLABLE_HEADER(f2c_LoadProperty);

	GPtr<FxDelegate> delegate;
	GPtr<GFxMovieView>      movie;
};

