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

#include "RoomsBrowserGFx3.h"

RoomsBrowserGFx3::RoomsBrowserGFx3()
{

}
RoomsBrowserGFx3::~RoomsBrowserGFx3()
{

}
void RoomsBrowserGFx3::Init(GPtr<FxDelegate> pDelegate, GPtr<GFxMovieView> pMovie)
{
	pDelegate->RegisterHandler(this);
	delegate=pDelegate;
	movie=pMovie;
}
void RoomsBrowserGFx3::Shutdown(void)
{
	if (delegate.GetPtr()!=0)
	{
		delegate->UnregisterHandler(this);
		delegate.Clear();
	}
	movie.Clear();
}

ACTIONSCRIPT_CALLABLE_FUNCTION(RoomsBrowserGFx3, f2c_QueryPlatform)
{
	FxResponseArgs<1> rargs;
	rargs.Add(QueryPlatform());
	// pparams.Respond(rargs);
	
	FxDelegate::Invoke2(movie, pparams[0].GetString(), rargs);
}

ACTIONSCRIPT_CALLABLE_FUNCTION(RoomsBrowserGFx3, f2c_SaveProperty)
{
	SaveProperty(pparams[0].GetString(), pparams[1].GetString());
}
ACTIONSCRIPT_CALLABLE_FUNCTION(RoomsBrowserGFx3, f2c_LoadProperty)
{
	MafiaNet::RakString propertyOut;
	LoadProperty(pparams[0].GetString(), propertyOut);
	FxResponseArgs<1> rargs;
	rargs.Add(propertyOut.C_String());
	// pparams.Respond(rargs);

	FxDelegate::Invoke2(movie, pparams[1].GetString(), rargs);
}
void RoomsBrowserGFx3::Accept(CallbackProcessor* cbreg)
{
	cbreg->Process( "f2c_QueryPlatform", &::f2c_QueryPlatform );
	cbreg->Process( "f2c_SaveProperty", &::f2c_SaveProperty );
	cbreg->Process( "f2c_LoadProperty", &::f2c_LoadProperty );
}
