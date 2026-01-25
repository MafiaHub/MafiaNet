RakVoice
========

RakVoice provides voice chat functionality for multiplayer games.

Overview
--------

RakVoice enables real-time voice communication between players using:

- Speex codec for compression
- Automatic silence detection
- Configurable quality settings
- Low latency transmission

Setup
-----

.. code-block:: cpp

   #include "mafianet/RakVoice.h"

   MafiaNet::RakVoice* rakVoice = MafiaNet::RakVoice::GetInstance();
   peer->AttachPlugin(rakVoice);

   // Initialize with sample rate and buffer size
   rakVoice->Init(8000, 160);  // 8kHz, 160 samples per frame

Recording Audio
---------------

.. code-block:: cpp

   // In your audio callback (e.g., from PortAudio, DirectSound)
   void AudioCallback(short* samples, int numSamples) {
       // Send audio to all connected peers
       for (int i = 0; i < peer->NumberOfConnections(); i++) {
           MafiaNet::SystemAddress addr = peer->GetSystemAddressFromIndex(i);
           rakVoice->SendFrame(addr, samples);
       }
   }

Playing Received Audio
----------------------

.. code-block:: cpp

   // In your audio playback loop
   void PlaybackLoop() {
       short samples[160];

       for (int i = 0; i < peer->NumberOfConnections(); i++) {
           MafiaNet::SystemAddress addr = peer->GetSystemAddressFromIndex(i);

           // Get received audio
           if (rakVoice->ReceiveFrame(addr, samples)) {
               // Play samples through your audio system
               PlayAudio(samples, 160);
           }
       }
   }

Configuration
-------------

.. code-block:: cpp

   // Set encoder complexity (0-10, higher = better quality, more CPU)
   rakVoice->SetEncoderComplexity(5);

   // Enable/disable voice activity detection
   rakVoice->SetVAD(true);

   // Set noise suppression
   rakVoice->SetNoiseFilter(true);

Audio Backends
--------------

RakVoice doesn't include audio capture/playback. Use:

- **PortAudio** - Cross-platform
- **DirectSound** - Windows (see ``Samples/RakVoiceDSound/``)
- **FMOD** - Cross-platform (see ``Samples/RakVoiceFMOD/``)
- **OpenAL** - Cross-platform

Sample Code
-----------

See ``Samples/RakVoice/``, ``Samples/RakVoiceDSound/``, and ``Samples/RakVoiceFMOD/`` for complete examples.

See Also
--------

* :doc:`overview` - Plugin basics
* :doc:`../getting-started/samples` - Sample applications
