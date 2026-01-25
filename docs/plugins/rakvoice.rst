RakVoice
========

RakVoice provides voice chat functionality for multiplayer games.

Overview
--------

RakVoice enables real-time voice communication between players using:

- **Opus codec** for high-quality, low-latency audio compression
- **RNNoise** for neural network-based noise suppression
- Voice Activity Detection (VAD) via Opus DTX
- Variable bitrate (VBR) encoding
- Low latency transmission with packet loss concealment

Setup
-----

.. code-block:: cpp

   #include "RakVoice.h"

   MafiaNet::RakVoice rakVoice;
   peer->AttachPlugin(&rakVoice);

   // Initialize with sample rate and buffer size
   // Supported rates: 8000, 16000, 24000, 48000 Hz
   rakVoice.Init(48000, 960 * sizeof(short));  // 48kHz, 20ms frame

Opening Voice Channels
----------------------

.. code-block:: cpp

   // Request a voice channel with a connected peer
   rakVoice.RequestVoiceChannel(peerGUID);

   // Handle the response in your packet loop:
   // ID_RAKVOICE_OPEN_CHANNEL_REQUEST - incoming channel request
   // ID_RAKVOICE_OPEN_CHANNEL_REPLY - channel opened successfully

Sending Audio
-------------

.. code-block:: cpp

   // In your audio callback (e.g., from PortAudio)
   void AudioCallback(short* samples, int numSamples) {
       // Send audio to a specific peer
       rakVoice.SendFrame(peerGUID, samples);
   }

Receiving Audio
---------------

.. code-block:: cpp

   // In your audio playback callback
   void PlaybackCallback(short* outputBuffer, int numSamples) {
       // Get mixed audio from all channels
       rakVoice.ReceiveFrame(outputBuffer);
   }

Configuration
-------------

.. code-block:: cpp

   // Enable/disable Voice Activity Detection (reduces bandwidth on silence)
   rakVoice.SetVAD(true);  // Default: true

   // Enable/disable RNNoise noise suppression
   rakVoice.SetNoiseFilter(true);  // Default: true

   // Enable/disable Variable Bitrate (better quality/bandwidth ratio)
   rakVoice.SetVBR(true);  // Default: true

   // Set signal type hint for Opus encoder
   rakVoice.SetSignalType(OPUS_SIGNAL_VOICE);  // or OPUS_SIGNAL_MUSIC

Audio Backends
--------------

RakVoice doesn't include audio capture/playback. Use:

- **PortAudio** - Cross-platform (see ``Samples/RakVoice/``)
- **DirectSound** - Windows
- **FMOD** - Cross-platform
- **OpenAL** - Cross-platform

Dependencies
------------

RakVoice bundles the following libraries:

- **Opus 1.5.2** - Audio codec (BSD license)
- **RNNoise** - Noise suppression (BSD license)

These are built automatically as part of the MafiaNet build.

Sample Code
-----------

See ``Samples/RakVoice/`` for a complete example using PortAudio.

See Also
--------

* :doc:`overview` - Plugin basics
* :doc:`../getting-started/samples` - Sample applications
