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

#include "DSoundVoiceAdapter.h"
#include "slikenet/assert.h"
#include "slikenet/peerinterface.h"
#include <tchar.h>

/// To test sending to myself
//#define _TEST_LOOPBACK

using namespace SLNet;

DSoundVoiceAdapter DSoundVoiceAdapter::instance;

DSoundVoiceAdapter::DSoundVoiceAdapter()
{
	m_rakVoice = 0;
	ds = 0;
	dsC = 0;
	dsbIncoming = 0;
	dsbOutgoing = 0;
	m_mute = false;
	memset(incomingBufferNotifications,0,sizeof(incomingBufferNotifications));
	memset(outgoingBufferNotifications,0,sizeof(outgoingBufferNotifications));
}

DSoundVoiceAdapter* DSoundVoiceAdapter::Instance()
{
	return &instance;
}

bool DSoundVoiceAdapter::SetupAdapter(RakVoice *rakVoice, HWND hwnd, DWORD dwCoopLevel)
{
	// Check if it was already initialized
	RakAssert(ds == 0);
	if (ds != 0)
		return false;

	HRESULT hr;

	if (FAILED(hr = DirectSoundCreate8(nullptr, &ds, nullptr)))
	{
		DXTRACE_ERR_MSGBOX(_T("DirectSoundCreate8, when initiliazing DirectSound"), hr);
		return false;
	}

	if (FAILED(hr = ds->SetCooperativeLevel(hwnd, dwCoopLevel)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSound8::SetCooperativeLevel"), hr);
		Release();
		return false;
	}

	// Check if the rest of the initialization fails, and if so, release any allocated resources
	if (SetupAdapter(rakVoice))
	{
		return true;
	}
	else
	{
		Release();
		return false;
	}
}

bool DSoundVoiceAdapter::SetupAdapter(RakVoice *rakVoice, IDirectSound8 *pDS)
{
	// Check if it was already initialized
	RakAssert(ds == 0);
	if (ds != 0)
		return false;

	// User provided his own device object, so use it and add another reference
	pDS->AddRef();
	ds = pDS;
	
	// Check if the rest of the initialization fails, and if so, release any allocated resources
	if (SetupAdapter(rakVoice))
	{
		return true;
	}
	else
	{
		Release();
		return false;
	}
}

bool DSoundVoiceAdapter::SetupAdapter(RakVoice *rakVoice)
{
	RakAssert(rakVoice);
	// Make sure rakVoice was initialized
	RakAssert((rakVoice->IsInitialized())&&(rakVoice->GetRakPeerInterface()!= nullptr));

	m_rakVoice = rakVoice;

	if (!SetupIncomingBuffer())
		return false;
	
	return SetupOutgoingBuffer();
}

bool DSoundVoiceAdapter::SetupIncomingBuffer()
{

	//
	//
	// Create the buffer for incoming sound
	//
	//

	WAVEFORMATEX wfx; 
	DSBUFFERDESC dsbdesc; 
	LPDIRECTSOUNDBUFFER pDsb = nullptr;
	HRESULT hr; 
	// Set up WAV format structure. 
	memset(&wfx, 0, sizeof(WAVEFORMATEX)); 
	wfx.wFormatTag = WAVE_FORMAT_PCM; 
	wfx.nChannels = 1; 
	wfx.nSamplesPerSec = m_rakVoice->GetSampleRate();
	wfx.nBlockAlign = 2; 
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign; 
	wfx.wBitsPerSample = 16; 
	// Set up DSBUFFERDESC structure. 
	memset(&dsbdesc, 0, sizeof(DSBUFFERDESC)); 
	dsbdesc.dwSize = sizeof(DSBUFFERDESC); 
	dsbdesc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLPOSITIONNOTIFY
		| DSBCAPS_LOCSOFTWARE; // Create in software, because DX documentation says its the best option when using notifications
	dsbdesc.dwBufferBytes = m_rakVoice->GetBufferSizeBytes()*FRAMES_IN_SOUND;
	dsbdesc.lpwfxFormat = &wfx; 
	// Create buffer. 
	if (FAILED(hr = ds->CreateSoundBuffer(&dsbdesc, &pDsb, nullptr)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSound8::CreateSoundBuffer, when creating buffer for incoming sound )"), hr);
		return false;
	}
	hr = pDsb->QueryInterface(IID_IDirectSoundBuffer8, (LPVOID*) &dsbIncoming);
	pDsb->Release();
	if (FAILED(hr))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundBuffer::QueryInterface, when getting IDirectSoundBuffer8 interface for incoming sound"), hr);
		return false;
	}
	//
	// Setup the notification events
	//
	for (int i=0; i<FRAMES_IN_SOUND; i++)
	{
		incomingBufferNotifications[i].dwOffset = i*m_rakVoice->GetBufferSizeBytes();
		if ((incomingBufferNotifications[i].hEventNotify = CreateEvent(nullptr, TRUE, FALSE, nullptr))== nullptr)
		{
			DXTRACE_ERR_MSGBOX(_T("CreateEvent"), GetLastError());
			return false;
		}
	}
	IDirectSoundNotify8 *dsbNotify=0;
	if (FAILED(hr=dsbIncoming->QueryInterface(IID_IDirectSoundNotify8, (LPVOID*) &dsbNotify)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundBuffer8::QueryInterface, when getting IDirectSoundNotify8 interface for incoming sound"), hr);
		return false;
	}
	hr = dsbNotify->SetNotificationPositions(FRAMES_IN_SOUND, incomingBufferNotifications);
	dsbNotify->Release();
	if (FAILED(hr))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundNotify8::SetNotificationPositions, when setting notifications for incoming sound"), hr);
		return false;
	}

	if (FAILED(hr = dsbIncoming->Play(0,0,DSBPLAY_LOOPING)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundBuffer8::Play, when starting incoming sound buffer"), hr);
		return false;
	}

	return true;
}

bool DSoundVoiceAdapter::SetupOutgoingBuffer()
{
	HRESULT hr;

	//
	//
	// Create the buffer for outgoing sound
	//
	//
	if (FAILED(hr=DirectSoundCaptureCreate8(nullptr, &dsC, nullptr)))
	{
		DXTRACE_ERR_MSGBOX(_T("DirectSoundCaptureCreate8"), hr);
		return false;
	}

	// Set up WAV format structure. 
	WAVEFORMATEX wfx; 
	memset(&wfx, 0, sizeof(WAVEFORMATEX)); 
	wfx.wFormatTag = WAVE_FORMAT_PCM; 
	wfx.nChannels = 1; 
	wfx.nSamplesPerSec = m_rakVoice->GetSampleRate();
	wfx.nBlockAlign = 2; 
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign; 
	wfx.wBitsPerSample = 16; 
	// Set up DSCBUFFERDESC structure. 
	DSCBUFFERDESC dscbd;
	memset(&dscbd, 0, sizeof(DSCBUFFERDESC));
	dscbd.dwSize = sizeof(DSCBUFFERDESC);
	dscbd.dwFlags = 0;
	dscbd.dwBufferBytes = m_rakVoice->GetBufferSizeBytes()*FRAMES_IN_SOUND;
	dscbd.dwReserved = 0;
	dscbd.lpwfxFormat = &wfx;
	dscbd.dwFXCount = 0;
	dscbd.lpDSCFXDesc = nullptr;
	// Create capture buffer.
	LPDIRECTSOUNDCAPTUREBUFFER pDscb = nullptr;
	if (FAILED(hr = dsC->CreateCaptureBuffer(&dscbd, &pDscb, nullptr)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundCapture8::CreateCaptureBuffer, when creating buffer for outgoing sound )"), hr);
		return false;
	}
	hr = pDscb->QueryInterface(IID_IDirectSoundCaptureBuffer8, (LPVOID*) &dsbOutgoing);
	pDscb->Release();
	if (FAILED(hr))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundBuffer::QueryInterface, when getting IDirectSoundCaptureBuffer8 interface for outgoing sound"), hr);
		return false;
	}
	//
	// Setup the notification events
	//
	for (int i=0; i<FRAMES_IN_SOUND; i++)
	{
		outgoingBufferNotifications[i].dwOffset = i*m_rakVoice->GetBufferSizeBytes();
		if ((outgoingBufferNotifications[i].hEventNotify = CreateEventEx(0, 0, CREATE_EVENT_MANUAL_RESET, 0))== nullptr)
		{
			DXTRACE_ERR_MSGBOX(_T("CreateEvent"), GetLastError());
			return false;
		}
	}
	IDirectSoundNotify8 *dsbNotify=0;
	if (FAILED(hr=dsbOutgoing->QueryInterface(IID_IDirectSoundNotify8, (LPVOID*) &dsbNotify)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundCaptureBuffer8::QueryInterface, when getting IDirectSoundNotify8 interface for outgoing sound"), hr);
		return false;
	}
	hr = dsbNotify->SetNotificationPositions(FRAMES_IN_SOUND, outgoingBufferNotifications);
	dsbNotify->Release();
	if (FAILED(hr))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundNotify8::SetNotificationPositions, when setting notifications for outgoing sound"), hr);
		return false;
	}

	if (FAILED(hr = dsbOutgoing->Start(DSCBSTART_LOOPING)))
	{
		DXTRACE_ERR_MSGBOX(_T("IDirectSoundCaptureBuffer8::Start, when starting outgoing sound buffer"), hr);
		return false;
	}

	return true;
}

void DSoundVoiceAdapter::Release()
{
	// Release DirectSound buffer used for incoming voice
	if (dsbIncoming)
	{
		dsbIncoming->Stop();
		dsbIncoming->Release();
		dsbIncoming = 0;
	}

	// Release DirectSound buffer used for outgoing voice
	if (dsbOutgoing)
	{
		dsbOutgoing->Stop();
		dsbOutgoing->Release();
		dsbOutgoing = 0;
	}

	// Release DirectSound device object
	if (ds)
	{
		ds->Release();
		ds = 0;
	}

	if (dsC)
	{
		dsC->Release();
		dsC = 0;
	}

	// Release  the notification events
	for (int i=0; i<FRAMES_IN_SOUND;i++)
	{
		if (incomingBufferNotifications[i].hEventNotify!=0 && CloseHandle(incomingBufferNotifications[i].hEventNotify)==0)
		{
			DXTRACE_ERR_MSGBOX(_T("CloseHandle"), GetLastError());
		}
		if (outgoingBufferNotifications[i].hEventNotify!=0 && CloseHandle(outgoingBufferNotifications[i].hEventNotify)==0)
		{
			DXTRACE_ERR_MSGBOX(_T("CloseHandle"), GetLastError());
		}
	}
	memset(incomingBufferNotifications,0, sizeof(incomingBufferNotifications));
	memset(outgoingBufferNotifications,0, sizeof(outgoingBufferNotifications));
}

IDirectSound8* DSoundVoiceAdapter::GetDSDeviceObject()
{
	return ds;
}

void DSoundVoiceAdapter::Update()
{
	HRESULT hr;
	void *audioPtr;
	DWORD audioPtrbytes;

	for (int i=0; i<FRAMES_IN_SOUND; i++)
	{
		//
		// Update incoming sound
		//
		if (WaitForSingleObject(incomingBufferNotifications[i].hEventNotify, 0)==WAIT_OBJECT_0)
		{
			// The lock offset is the buffer right before the one the event refers to
			DWORD dwOffset = (i==0) ? incomingBufferNotifications[FRAMES_IN_SOUND-1].dwOffset : incomingBufferNotifications[i-1].dwOffset;
			hr = dsbIncoming->Lock(dwOffset, m_rakVoice->GetBufferSizeBytes(), &audioPtr, &audioPtrbytes, nullptr, nullptr, 0);
			// #med - should change GetBufferSizeBytes()-interface to unsigned int return
			RakAssert(audioPtrbytes==static_cast<unsigned int>(m_rakVoice->GetBufferSizeBytes()));
			if (SUCCEEDED(hr))
			{
				m_rakVoice->ReceiveFrame(audioPtr);
				dsbIncoming->Unlock(audioPtr, audioPtrbytes, nullptr, 0);
			}
			ResetEvent(incomingBufferNotifications[i].hEventNotify);
		}

		//
		// Update outgoing sound
		//
		if (WaitForSingleObject(outgoingBufferNotifications[i].hEventNotify, 0)==WAIT_OBJECT_0)
		{

			/* If we're set to mute, we don't send anything, and just reset the event */
			if (!m_mute)
			{		
				// The lock offset is the buffer right before the one the event refers to
				DWORD dwOffset = (i==0) ? outgoingBufferNotifications[FRAMES_IN_SOUND-1].dwOffset : outgoingBufferNotifications[i-1].dwOffset;
				hr = dsbOutgoing->Lock(dwOffset, m_rakVoice->GetBufferSizeBytes(), &audioPtr, &audioPtrbytes, nullptr, nullptr, 0);
				// #med - should change GetBufferSizeBytes()-interface to unsigned int return
				RakAssert(audioPtrbytes == static_cast<unsigned int>(m_rakVoice->GetBufferSizeBytes()));
				if (SUCCEEDED(hr))
				{
					BroadcastFrame(audioPtr);
					dsbOutgoing->Unlock(audioPtr, audioPtrbytes, nullptr, 0);
				}
			}
			ResetEvent(outgoingBufferNotifications[i].hEventNotify);
		}

	}
}

void DSoundVoiceAdapter::BroadcastFrame(void *ptr)
{
#ifndef _TEST_LOOPBACK
	unsigned i;

	unsigned int numPeers = m_rakVoice->GetRakPeerInterface()->GetMaximumNumberOfPeers();
	for (i=0; i < numPeers; i++)
	{
		m_rakVoice->SendFrame(m_rakVoice->GetRakPeerInterface()->GetGUIDFromIndex(i), ptr);
	}
#else
	m_rakVoice->SendFrame(SLNet::UNASSIGNED_SYSTEM_ADDRESS, ptr);
#endif

}

void DSoundVoiceAdapter::SetMute(bool mute)
{
	m_mute = mute;
}
