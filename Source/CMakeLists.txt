# MafiaNet Library
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Batched datagram I/O (recvmmsg/sendmmsg) needs no option: it is a platform
# capability, guarded by a plain #if defined(__linux__), and is always on where
# the syscalls exist. See docs/getting-started/building.rst.

# Source files (explicit list, no GLOB)
set(MAFIANET_SOURCES
    src/_FindFirst.cpp
    src/Base64Encoder.cpp
    src/BitStream.cpp
    src/CCRakNetSlidingWindow.cpp
    src/CCRakNetUDT.cpp
    src/CheckSum.cpp
    src/CloudClient.cpp
    src/CloudCommon.cpp
    src/CloudServer.cpp
    src/ConnectionGraph2.cpp
    src/DataCompressor.cpp
    src/DirectoryDeltaTransfer.cpp
    src/DR_SHA1.cpp
    src/DS_BytePool.cpp
    src/DS_ByteQueue.cpp
    src/DS_HuffmanEncodingTree.cpp
    src/DS_Table.cpp
    src/DynDNS.cpp
    src/EpochTimeToString.cpp
    src/FileList.cpp
    src/FileListTransfer.cpp
    src/FileOperations.cpp
    src/FormatString.cpp
    src/FullyConnectedMesh2.cpp
    src/Getche.cpp
    src/Gets.cpp
    src/GetTime.cpp
    src/GetTimeOfDay.cpp
    src/GridSectorizer.cpp
    src/GuidUtil.cpp
    src/IncrementalReadInterface.cpp
    src/Itoa.cpp
    src/LinuxAdapter.cpp
    src/LinuxStrings.cpp
    src/LocklessTypes.cpp
    src/MessageFilter.cpp
    src/MmsgBatch.cpp
    src/MtuBlackHole.cpp
    src/NatPunchthroughClient.cpp
    src/NatPunchthroughServer.cpp
    src/NatTypeDetectionClient.cpp
    src/NatTypeDetectionCommon.cpp
    src/NatTypeDetectionServer.cpp
    src/NetworkIDManager.cpp
    src/NetworkIDObject.cpp
    src/OsxAdapter.cpp
    src/PacketFileLogger.cpp
    src/PacketizedTCP.cpp
    src/PacketLogger.cpp
    src/PacketOutputWindowLogger.cpp
    src/PeerHandle.cpp
    src/PluginInterface2.cpp
    src/PointGridSectorizer.cpp
    src/Rackspace.cpp
    src/RakMemoryOverride.cpp
    src/RakNetSocket2.cpp
    src/RakNetSocket2_Berkley.cpp
    src/RakNetSocket2_Windows_Linux.cpp
    src/RakNetStatistics.cpp
    src/RakNetTypes.cpp
    src/RakPeer.cpp
    src/RakSleep.cpp
    src/RakString.cpp
    src/RakThread.cpp
    src/RakVoice.cpp
    src/RakWString.cpp
    src/Rand.cpp
    src/RandSync.cpp
    src/ReadyEvent.cpp
    src/RelayPlugin.cpp
    src/ReliabilityLayer.cpp
    src/ReplicaManager3.cpp
    src/Router2.cpp
    src/RPC4Plugin.cpp
    src/SecureHandshake.cpp
    src/SendToThread.cpp
    src/SignaledEvent.cpp
    src/SimpleMutex.cpp
    src/SocketLayer.cpp
    src/StatisticsHistory.cpp
    src/StringCompressor.cpp
    src/StringTable.cpp
    src/SuperFastHash.cpp
    src/TableSerializer.cpp
    src/TCPInterface.cpp
    src/TeamBalancer.cpp
    src/TeamManager.cpp
    src/ThreadsafePacketLogger.cpp
    src/TwoWayAuthentication.cpp
    src/UDPForwarder.cpp
    src/UDPProxyClient.cpp
    src/UDPProxyCoordinator.cpp
    src/UDPProxyServer.cpp
    src/VariableDeltaSerializer.cpp
    src/VariableListDeltaTracker.cpp
    src/VariadicSQLParser.cpp
    src/WSAStartupSingleton.cpp
)

# Extras: utilities that are not game networking (SMTP, HTTP client, telnet and
# console command transports, console packet logger). Built into the separate
# MafiaNetExtras target when MAFIANET_BUILD_EXTRAS is ON; their headers stay in
# include/mafianet/ so include paths are unchanged. See issue #50, stage 2.
set(MAFIANET_EXTRAS_SOURCES
    src/CommandParserInterface.cpp
    src/ConsoleServer.cpp
    src/EmailSender.cpp
    src/HTTPConnection.cpp
    src/HTTPConnection2.cpp
    src/LogCommandParser.cpp
    src/PacketConsoleLogger.cpp
    src/RakNetCommandParser.cpp
    src/RakNetTransport2.cpp
    src/TelnetTransport.cpp
)

set(MAFIANET_CRYPTO_SOURCES
    src/crypto/cryptomanager.cpp
    src/crypto/factory.cpp
    src/crypto/fileencrypter.cpp
    src/crypto/securestring.cpp
)

set(MAFIANET_HEADERS
    include/mafianet/_FindFirst.h
    include/mafianet/RakAlloca.h
    include/mafianet/RakAssert.h
    include/mafianet/AutopatcherPatchContext.h
    include/mafianet/AutopatcherRepositoryInterface.h
    include/mafianet/Base64Encoder.h
    include/mafianet/BitStream.h
    include/mafianet/CCRakNetSlidingWindow.h
    include/mafianet/CCRakNetUDT.h
    include/mafianet/CheckSum.h
    include/mafianet/CloudClient.h
    include/mafianet/CloudCommon.h
    include/mafianet/CloudServer.h
    include/mafianet/RakNetCommandParser.h
    include/mafianet/CommandParserInterface.h
    include/mafianet/ConnectionGraph2.h
    include/mafianet/ConsoleServer.h
    include/mafianet/DataCompressor.h
    include/mafianet/RakNetDefinesOverrides.h
    include/mafianet/RakNetDefines.h
    include/mafianet/DirectoryDeltaTransfer.h
    include/mafianet/DR_SHA1.h
    include/mafianet/DS_BinarySearchTree.h
    include/mafianet/DS_BPlusTree.h
    include/mafianet/DS_BytePool.h
    include/mafianet/DS_ByteQueue.h
    include/mafianet/DS_Hash.h
    include/mafianet/DS_Heap.h
    include/mafianet/DS_HuffmanEncodingTree.h
    include/mafianet/DS_HuffmanEncodingTreeFactory.h
    include/mafianet/DS_HuffmanEncodingTreeNode.h
    include/mafianet/DS_LinkedList.h
    include/mafianet/DS_List.h
    include/mafianet/DS_Map.h
    include/mafianet/DS_MemoryPool.h
    include/mafianet/DS_Multilist.h
    include/mafianet/DS_OrderedChannelHeap.h
    include/mafianet/DS_OrderedList.h
    include/mafianet/DS_Queue.h
    include/mafianet/DS_QueueLinkedList.h
    include/mafianet/DS_RangeList.h
    include/mafianet/DS_Table.h
    include/mafianet/DS_ThreadsafeAllocatingQueue.h
    include/mafianet/DS_Tree.h
    include/mafianet/DS_WeightedGraph.h
    include/mafianet/DynDNS.h
    include/mafianet/EmailSender.h
    include/mafianet/EmptyHeader.h
    include/mafianet/EpochTimeToString.h
    include/mafianet/Export.h
    include/mafianet/FileList.h
    include/mafianet/FileListNodeContext.h
    include/mafianet/FileListTransfer.h
    include/mafianet/FileListTransferCBInterface.h
    include/mafianet/FileOperations.h
    include/mafianet/FormatString.h
    include/mafianet/FullyConnectedMesh2.h
    include/mafianet/Getche.h
    include/mafianet/Gets.h
    include/mafianet/GetTime.h
    include/mafianet/GetTimeOfDay.h
    include/mafianet/GridSectorizer.h
    include/mafianet/GuidUtil.h
    include/mafianet/HTTPConnection.h
    include/mafianet/HTTPConnection2.h
    include/mafianet/IncrementalReadInterface.h
    include/mafianet/InternalPacket.h
    include/mafianet/Itoa.h
    include/mafianet/Kbhit.h
    include/mafianet/LinuxAdapter.h
    include/mafianet/LinuxStrings.h
    include/mafianet/LocklessTypes.h
    include/mafianet/LogCommandParser.h
    include/mafianet/RakMemoryOverride.h
    include/mafianet/MessageFilter.h
    include/mafianet/MessageIdentifiers.h
    include/mafianet/MmsgBatch.h
    include/mafianet/MtuBlackHole.h
    include/mafianet/MTUSize.h
    include/mafianet/NativeFeatureIncludes.h
    include/mafianet/NativeFeatureIncludesOverrides.h
    include/mafianet/NativeTypes.h
    include/mafianet/NatPunchthroughClient.h
    include/mafianet/NatPunchthroughServer.h
    include/mafianet/NatTypeDetectionClient.h
    include/mafianet/NatTypeDetectionCommon.h
    include/mafianet/NatTypeDetectionServer.h
    include/mafianet/NetworkIDManager.h
    include/mafianet/NetworkIDObject.h
    include/mafianet/OsxAdapter.h
    include/mafianet/PacketConsoleLogger.h
    include/mafianet/PacketFileLogger.h
    include/mafianet/PacketizedTCP.h
    include/mafianet/PacketLogger.h
    include/mafianet/PacketOutputWindowLogger.h
    include/mafianet/PacketPool.h
    include/mafianet/PacketPriority.h
    include/mafianet/PeerHandle.h
    include/mafianet/RakPeer.h
    include/mafianet/RakPeerInterface.h
    include/mafianet/PluginInterface2.h
    include/mafianet/PointGridSectorizer.h
    include/mafianet/Rackspace.h
    include/mafianet/RakVoice.h
    include/mafianet/Rand.h
    include/mafianet/RandSync.h
    include/mafianet/ReadyEvent.h
    include/mafianet/RefCountedObj.h
    include/mafianet/RelayPlugin.h
    include/mafianet/ReliabilityLayer.h
    include/mafianet/ReplicaEnums.h
    include/mafianet/ReplicaManager3.h
    include/mafianet/Router2.h
    include/mafianet/RPC4Plugin.h
    include/mafianet/SecureHandshake.h
    include/mafianet/SendToThread.h
    include/mafianet/SignaledEvent.h
    include/mafianet/SimpleMutex.h
    include/mafianet/SimpleTCPServer.h
    include/mafianet/SingleProducerConsumer.h
    include/mafianet/RakSleep.h
    include/mafianet/mafianet.h
    include/mafianet/RakNetSmartPtr.h
    include/mafianet/RakNetSocket2.h
    include/mafianet/SocketDefines.h
    include/mafianet/SocketIncludes.h
    include/mafianet/SocketLayer.h
    include/mafianet/RakNetStatistics.h
    include/mafianet/StatisticsHistory.h
    include/mafianet/RakString.h
    include/mafianet/StringCompressor.h
    include/mafianet/StringTable.h
    include/mafianet/SuperFastHash.h
    include/mafianet/TableSerializer.h
    include/mafianet/TCPInterface.h
    include/mafianet/TeamBalancer.h
    include/mafianet/TeamManager.h
    include/mafianet/TelnetTransport.h
    include/mafianet/RakThread.h
    include/mafianet/ThreadPool.h
    include/mafianet/ThreadsafePacketLogger.h
    include/mafianet/RakNetTime.h
    include/mafianet/RakNetTransport2.h
    include/mafianet/TransportInterface.h
    include/mafianet/TwoWayAuthentication.h
    include/mafianet/RakNetTypes.h
    include/mafianet/UDPForwarder.h
    include/mafianet/UDPProxyClient.h
    include/mafianet/UDPProxyCommon.h
    include/mafianet/UDPProxyCoordinator.h
    include/mafianet/UDPProxyServer.h
    include/mafianet/VariableDeltaSerializer.h
    include/mafianet/VariableListDeltaTracker.h
    include/mafianet/VariadicSQLParser.h
    include/mafianet/Version.h
    include/mafianet/VirtualWorld.h
    include/mafianet/VirtualWorldReplica3.h
    include/mafianet/WindowsIncludes.h
    include/mafianet/WSAStartupSingleton.h
    include/mafianet/RakWString.h
)

# Deprecated lowercase forwarding headers (one release cycle, see docs/changelog.rst)
set(MAFIANET_DEPRECATED_HEADERS
    include/mafianet/alloca.h
    include/mafianet/assert.h
    include/mafianet/commandparser.h
    include/mafianet/defineoverrides.h
    include/mafianet/defines.h
    include/mafianet/guid_util.h
    include/mafianet/linux_adapter.h
    include/mafianet/memoryoverride.h
    include/mafianet/osx_adapter.h
    include/mafianet/peer.h
    include/mafianet/peerinterface.h
    include/mafianet/sleep.h
    include/mafianet/smartptr.h
    include/mafianet/socket2.h
    include/mafianet/statistics.h
    include/mafianet/string.h
    include/mafianet/thread.h
    include/mafianet/time.h
    include/mafianet/transport2.h
    include/mafianet/types.h
    include/mafianet/wstring.h
)

set(MAFIANET_CRYPTO_HEADERS
    include/mafianet/crypto/cryptomanager.h
    include/mafianet/crypto/factory.h
    include/mafianet/crypto/fileencrypter.h
    include/mafianet/crypto/ifileencrypter.h
    include/mafianet/crypto/securestring.h
)

# Combine all sources
set(MAFIANET_ALL_SOURCES
    ${MAFIANET_SOURCES}
    ${MAFIANET_CRYPTO_SOURCES}
)

set(MAFIANET_ALL_HEADERS
    ${MAFIANET_HEADERS}
    ${MAFIANET_DEPRECATED_HEADERS}
    ${MAFIANET_CRYPTO_HEADERS}
)

# Optional plugins, one per _RAKNET_SUPPORT_<Name> flag in NativeFeatureIncludes.h.
# Anything not in this list (BitStream, RakPeer, the reliability layer, the
# socket layer, RakVoice, ...) is the core and always builds. Keep this list in
# sync with the header when adding or removing a plugin.
set(MAFIANET_OPTIONAL_FEATURES
    CloudClient
    CloudServer
    ConnectionGraph2
    ConsoleServer
    DirectoryDeltaTransfer
    DynDNS
    EmailSender
    FileListTransfer
    FullyConnectedMesh2
    HTTPConnection
    HTTPConnection2
    LogCommandParser
    MessageFilter
    NatPunchthroughClient
    NatPunchthroughServer
    NatTypeDetectionClient
    NatTypeDetectionServer
    PacketLogger
    PacketizedTCP
    Rackspace
    RakNetCommandParser
    RakNetTransport
    ReadyEvent
    RelayPlugin
    ReplicaManager3
    Router2
    RPC4Plugin
    StatisticsHistory
    TCPInterface
    TeamBalancer
    TeamManager
    TelnetTransport
    TwoWayAuthentication
    UDPForwarder
    UDPProxyClient
    UDPProxyCoordinator
    UDPProxyServer
)

# Resolve MAFIANET_MINIMAL / MAFIANET_DISABLED_FEATURES (set at the top level)
# into the list of _RAKNET_SUPPORT_<Name>=0 definitions applied to every
# library target. Unknown names are a configure error rather than a silent
# no-op, since a typo here would otherwise ship the plugin it meant to drop.
set(MAFIANET_FEATURE_DEFINITIONS "")
if(MAFIANET_MINIMAL)
    set(_mafianet_disabled ${MAFIANET_OPTIONAL_FEATURES})
else()
    set(_mafianet_disabled ${MAFIANET_DISABLED_FEATURES})
endif()
foreach(_feature IN LISTS _mafianet_disabled)
    if(NOT _feature IN_LIST MAFIANET_OPTIONAL_FEATURES)
        message(FATAL_ERROR
            "MAFIANET_DISABLED_FEATURES: '${_feature}' is not an optional plugin. "
            "Known names: ${MAFIANET_OPTIONAL_FEATURES}")
    endif()
    list(APPEND MAFIANET_FEATURE_DEFINITIONS "_RAKNET_SUPPORT_${_feature}=0")
endforeach()
if(MAFIANET_FEATURE_DEFINITIONS)
    list(LENGTH _mafianet_disabled _mafianet_disabled_count)
    message(STATUS "MafiaNet: compiling out ${_mafianet_disabled_count} optional plugin(s): ${_mafianet_disabled}")
endif()
unset(_mafianet_disabled)

# Common target setup function
function(mafianet_configure_target target_name)
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(${target_name}
        PUBLIC
            OpenSSL::SSL
            OpenSSL::Crypto
        PRIVATE
            Threads::Threads
            $<$<PLATFORM_ID:Windows>:ws2_32>
            # RakVoice (built into the core library) requires the Opus codec and
            # RNNoise noise suppression, fetched in FetchVoiceDependencies. These
            # are bundled into the MafiaNet export set (see Installation below) so
            # installed static-library consumers can resolve the symbols.
            opus
            rnnoise
    )

    target_compile_definitions(${target_name}
        PRIVATE
            $<$<CONFIG:Debug>:_DEBUG>
            $<$<CONFIG:Release>:NDEBUG>
        PUBLIC
            # Plugins compiled out of this build. PUBLIC so a consumer's view of
            # NativeFeatureIncludes.h matches the library it links against.
            ${MAFIANET_FEATURE_DEFINITIONS}
    )

    set_target_properties(${target_name} PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        FOLDER "Libraries"
    )

    # Source groups for IDE organization
    source_group("Source Files" FILES ${MAFIANET_SOURCES})
    source_group("Source Files\\crypto" FILES ${MAFIANET_CRYPTO_SOURCES})
    source_group("Header Files" FILES ${MAFIANET_HEADERS})
    source_group("Header Files\\crypto" FILES ${MAFIANET_CRYPTO_HEADERS})
endfunction()

# Static library
if(MAFIANET_BUILD_STATIC)
    add_library(MafiaNetStatic STATIC
        ${MAFIANET_ALL_SOURCES}
        ${MAFIANET_ALL_HEADERS}
    )
    add_library(MafiaNet::MafiaNetStatic ALIAS MafiaNetStatic)

    mafianet_configure_target(MafiaNetStatic)

    target_compile_definitions(MafiaNetStatic
        PUBLIC
            $<$<PLATFORM_ID:Windows>:_MAFIANET_LIB>
    )
endif()

# Shared library
if(MAFIANET_BUILD_SHARED)
    add_library(MafiaNet SHARED
        ${MAFIANET_ALL_SOURCES}
        ${MAFIANET_ALL_HEADERS}
    )
    add_library(MafiaNet::MafiaNet ALIAS MafiaNet)

    mafianet_configure_target(MafiaNet)

    target_compile_definitions(MafiaNet
        PRIVATE
            MAFIANET_DLL_EXPORT
        PUBLIC
            $<$<PLATFORM_ID:Windows>:_MAFIANET_DLL>
    )

    if(NOT WIN32)
        set_target_properties(MafiaNet PROPERTIES
            OUTPUT_NAME "mafianet"
        )
    endif()
endif()

# Extras library (off by default). Links the core library PUBLIC so consumers
# get the core, its include directories and its feature definitions through
# this one target.
if(MAFIANET_BUILD_EXTRAS)
    add_library(MafiaNetExtras STATIC ${MAFIANET_EXTRAS_SOURCES})
    add_library(MafiaNet::MafiaNetExtras ALIAS MafiaNetExtras)

    if(MAFIANET_BUILD_STATIC)
        target_link_libraries(MafiaNetExtras PUBLIC MafiaNetStatic)
    else()
        target_link_libraries(MafiaNetExtras PUBLIC MafiaNet)
    endif()

    target_include_directories(MafiaNetExtras PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(MafiaNetExtras
        PRIVATE
            $<$<CONFIG:Debug>:_DEBUG>
            $<$<CONFIG:Release>:NDEBUG>
    )
    set_target_properties(MafiaNetExtras PROPERTIES FOLDER "Libraries")
    source_group("Source Files" FILES ${MAFIANET_EXTRAS_SOURCES})
endif()

# Installation
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Determine which targets to install
set(MAFIANET_INSTALL_TARGETS)
if(MAFIANET_BUILD_STATIC)
    list(APPEND MAFIANET_INSTALL_TARGETS MafiaNetStatic)
endif()
if(MAFIANET_BUILD_SHARED)
    list(APPEND MAFIANET_INSTALL_TARGETS MafiaNet)
endif()
if(MAFIANET_BUILD_EXTRAS)
    list(APPEND MAFIANET_INSTALL_TARGETS MafiaNetExtras)
endif()

# Install targets
install(TARGETS ${MAFIANET_INSTALL_TARGETS}
    EXPORT MafiaNetTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# RakVoice links the Opus and RNNoise static libraries into the core library.
# Bundle them into the MafiaNet export so installed static-library consumers can
# resolve opus_*/rnnoise_* symbols (neither dependency ships its own CMake config
# package here). Opus also self-exports to its own OpusTargets set, which is
# harmless: a target may belong to multiple export sets.
install(TARGETS opus rnnoise
    EXPORT MafiaNetTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Generate and install config
configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/MafiaNetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MafiaNetConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MafiaNet
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MafiaNetConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MafiaNetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MafiaNetConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MafiaNet
)

install(EXPORT MafiaNetTargets
    FILE MafiaNetTargets.cmake
    NAMESPACE MafiaNet::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MafiaNet
)
