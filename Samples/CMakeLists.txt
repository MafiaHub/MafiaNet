# MafiaNet Samples
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Basic samples (no special dependencies)
add_subdirectory(BigPacketTest)
add_subdirectory(BurstTest)
add_subdirectory(ChatExample)
add_subdirectory(CommandConsoleClient)
add_subdirectory(CommandConsoleServer)
add_subdirectory(ComprehensivePCGame)
add_subdirectory(ComprehensiveTest)
add_subdirectory(CrossConnectionTest)
add_subdirectory(DirectoryDeltaTransfer)
add_subdirectory(DroppedConnectionTest)
add_subdirectory(Encryption)
add_subdirectory(FCMHost)
add_subdirectory(FCMHostSimultaneous)
add_subdirectory(FCMVerifiedJoinSimultaneous)
add_subdirectory(FileListTransfer)
add_subdirectory(FlowControlTest)
add_subdirectory(FullyConnectedMesh)
add_subdirectory(IdentityKeyGen)
add_subdirectory(LANServerDiscovery)
add_subdirectory(MasterServer)
add_subdirectory(MessageFilter)
add_subdirectory(MessageSizeTest)
add_subdirectory(NATCompleteClient)
add_subdirectory(NATCompleteServer)
add_subdirectory(OfflineMessagesTest)
add_subdirectory(PacketLogger)
add_subdirectory(PHPDirectoryServer2)
add_subdirectory(Ping)
add_subdirectory(ReadyEvent)
add_subdirectory(RelayPluginTest)
add_subdirectory(ReliableOrderedTest)
add_subdirectory(ReplicaManager3)
add_subdirectory(Router2)
add_subdirectory(RPC)
add_subdirectory(SendEmail)
add_subdirectory(ServerClientTest2)
add_subdirectory(StatisticsHistoryTest)
add_subdirectory(TeamManager)
add_subdirectory(ThreadTest)
add_subdirectory(Timestamping)
add_subdirectory(TwoWayAuthentication)
add_subdirectory(UDPForwarder)
add_subdirectory(VirtualWorld)

# Samples requiring crash reporter extension
add_subdirectory(CrashReporter)

# Platform/third-party dependent samples (conditionally built)

# RakVoice samples require audio libraries
add_subdirectory(RakVoice)

# The following samples are disabled by default as they require
# specific third-party dependencies, platforms, or special configuration:
#
# - CrashRelauncher (Windows-specific)
# - Lobby3 (incomplete)
# - LoopbackPerformanceTest (specialized)
# - RakVoiceDSound (requires DirectSound)
# - RakVoiceFMOD (requires FMOD)
# - RakVoiceFMODAsDLL (requires FMOD)
# - TestDLL (Windows DLL test)
