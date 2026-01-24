/*
 * This file was taken from RakNet 4.082.
 * Please see licenses/RakNet license.txt for the underlying license and related copyright.
 *
 * Modified work: Copyright (c) 2017-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 * This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 * license found in the license.txt file in the root directory of this source tree.
 */

#include "DX9_BackbufferGrabber.h"

DX9_BackbufferGrabber::DX9_BackbufferGrabber()
{
	pDestSurface=0;
	pRenderTargetSurface=0;
	deviceUsedToInit=0;
	width=0;
	height=0;
	needsUnlock=false;
}
DX9_BackbufferGrabber::~DX9_BackbufferGrabber()
{
	if (pDestSurface)
		pDestSurface->Release();
	if (pRenderTargetSurface)
		pRenderTargetSurface->Release();
}
void DX9_BackbufferGrabber::InitBackbufferGrabber(LPDIRECT3DDEVICE9 pd3dDevice, int _width, int _height)
{
	if (width==_width && height==_height && pDestSurface && pRenderTargetSurface)
		return;

	if (pDestSurface)
		pDestSurface->Release();
	if (pRenderTargetSurface)
		pRenderTargetSurface->Release();

	width=_width;
	height=_height;
	deviceUsedToInit=pd3dDevice;

	HRESULT hr;
	// KevinJ: Surface to copy to in system memory
	hr = deviceUsedToInit->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM, &pDestSurface, nullptr);
	if (hr!=S_OK) return;
	// Surface to downsize to
	hr = deviceUsedToInit->CreateRenderTarget(
		width,
		height,
		D3DFMT_A8R8G8B8,
		D3DMULTISAMPLE_NONE,
		0,
		false,
		&pRenderTargetSurface,
		nullptr
		);
}
void DX9_BackbufferGrabber::LockBackbufferCopy(MafiaNet::RGBImageBlob *blob)
{
	LPDIRECT3DDEVICE9 pd3dDevice;
	pd3dDevice=deviceUsedToInit;

	IDirect3DSurface9 * pBackBuffer;
	HRESULT hr;
	hr = deviceUsedToInit->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
	hr = deviceUsedToInit->StretchRect(pBackBuffer, nullptr, pRenderTargetSurface, nullptr, D3DTEXF_NONE);
	hr = deviceUsedToInit->GetRenderTargetData(pRenderTargetSurface,pDestSurface);

	//	MafiaNet::TimeMS t1 = MafiaNet::GetTimeMS();
	D3DLOCKED_RECT lockedRect;
	hr = pDestSurface->LockRect(&lockedRect,0,D3DLOCK_DONOTWAIT|D3DLOCK_READONLY|D3DLOCK_NOSYSLOCK);
	if (hr==D3D_OK)
	{
		blob->data=(unsigned char*)(lockedRect.pBits);
		// #high - consider changing data types or add error handling
		blob->imageHeight=static_cast<uint16_t>(height);
		blob->imageWidth=static_cast<uint16_t>(width);
		blob->input_components=4;
		blob->linePitch=static_cast<uint16_t>(lockedRect.Pitch);
		needsUnlock=true;
	}
}
void DX9_BackbufferGrabber::ReleaseBackbufferCopy(void)
{
	if (needsUnlock)
	{
		pDestSurface->UnlockRect();
		needsUnlock=false;
	}
}
