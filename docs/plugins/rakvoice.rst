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

RakVoice is built into the core MafiaNet library — no extra build option or
separate extension library is needed. Its codec dependencies (Opus, RNNoise)
are fetched and linked into the core library automatically at configure time.

Setup
-----

.. code-block:: cpp

   #include "mafianet/RakVoice.h"

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

Ordering Channels
-----------------

RakVoice sends on ordering channel 0 unless told otherwise. That is the channel most applications also use for their own sequenced stream, and voice traffic interferes with it in two ways: a frame that overtakes one of the application's sequenced messages makes the receiver discard that message as stale, and a lost open/close control message holds back every later sequenced message on the channel until it is retransmitted.

.. code-block:: cpp

   // Before Init(): frames on one channel, open/close control on another,
   // both away from anything the application sequences itself.
   if (!rakVoice.SetOrderingChannels(4, 5)) {
       // a value outside 0..NUMBER_OF_ORDERED_STREAMS-1 was rejected; both channels are unchanged
   }

The frame channel applies to the relay-mode ``UnreliableSequenced`` send from a client to its relay host. Frames the relay host forwards, and frames sent directly between peers, go out plain ``Unreliable`` and carry no ordering channel: the relay header's per-speaker sequence number orders them and drives packet-loss concealment, so several speakers never compete for one sequenced stream. The control channel carries the ``ReliableOrdered`` channel open, reply and close messages. See :ref:`ordering-channels`.

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

These are fetched via CMake FetchContent and linked into the core MafiaNet
library automatically.

Sample Code
-----------

See ``Samples/RakVoice/`` for a complete example using PortAudio.

See Also
--------

* :doc:`overview` - Plugin basics
* :doc:`../getting-started/samples` - Sample applications
