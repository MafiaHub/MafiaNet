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
- Low latency transmission with in-band FEC and packet loss concealment

RakVoice is built into the core MafiaNet library — no extra build option or
separate extension library is needed. Its codec dependencies (Opus, RNNoise)
are fetched and linked into the core library automatically at configure time.

Setup
-----

.. code-block:: cpp

   #include "mafianet/voice.h"

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

   // Enable/disable Voice Activity Detection: nothing is sent while the talker is
   // silent, comfort-noise refreshes included (reduces bandwidth on silence)
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

Receive Ordering and Loss
-------------------------

Voice frames travel unreliable, so they can arrive out of order or not at all. Each carries its sender's sequence number, and RakVoice decodes a speaker's frames strictly in that order.

- A frame behind a gap is held for up to ``RAKVOICE_REORDER_WAIT_MS`` (40ms) for the missing one, or until ``RAKVOICE_REORDER_MAX_HELD`` later frames are held. A late frame that arrives in that time is simply decoded in its place.
- A gap that stays open is filled, so the audio after it keeps its timing. The frame just before the held one is rebuilt from that frame's in-band FEC. Any before it are extrapolated by Opus packet-loss concealment if the last frame was decoded within ``RAKVOICE_CONCEAL_WINDOW_MS``, and are silence otherwise: by then the reader may already have played the gap as silence, and an extrapolation played after that would be the decaying echo of the word before it.
- At most ``RAKVOICE_MAX_CONCEALED_FRAMES`` are filled; the rest of a longer gap is skipped. A frame more than ``RAKVOICE_REORDER_WINDOW`` ahead restarts the stream from itself.
- ``Update()`` settles gaps whose wait has run out, so call it every tick.

A reader should keep its own playout buffer deeper than the reorder wait, and no deeper than about 200ms, which the concealment window is sized to.

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
