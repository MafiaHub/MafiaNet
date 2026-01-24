# MafiaNet Library
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

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
    src/CommandParserInterface.cpp
    src/ConnectionGraph2.cpp
    src/ConsoleServer.cpp
    src/DataCompressor.cpp
    src/DirectoryDeltaTransfer.cpp
    src/DR_SHA1.cpp
    src/DS_BytePool.cpp
    src/DS_ByteQueue.cpp
    src/DS_HuffmanEncodingTree.cpp
    src/DS_Table.cpp
    src/DynDNS.cpp
    src/EmailSender.cpp
    src/EpochTimeToString.cpp
    src/FileList.cpp
    src/FileListTransfer.cpp
    src/FileOperations.cpp
    src/FormatString.cpp
    src/FullyConnectedMesh2.cpp
    src/Getche.cpp
    src/Gets.cpp
    src/GetTime.cpp
    src/gettimeofday.cpp
    src/GridSectorizer.cpp
    src/HTTPConnection.cpp
    src/HTTPConnection2.cpp
    src/IncrementalReadInterface.cpp
    src/Itoa.cpp
    src/linux_adapter.cpp
    src/LinuxStrings.cpp
    src/LocklessTypes.cpp
    src/LogCommandParser.cpp
    src/MessageFilter.cpp
    src/NatPunchthroughClient.cpp
    src/NatPunchthroughServer.cpp
    src/NatTypeDetectionClient.cpp
    src/NatTypeDetectionCommon.cpp
    src/NatTypeDetectionServer.cpp
    src/NetworkIDManager.cpp
    src/NetworkIDObject.cpp
    src/osx_adapter.cpp
    src/PacketConsoleLogger.cpp
    src/PacketFileLogger.cpp
    src/PacketizedTCP.cpp
    src/PacketLogger.cpp
    src/PacketOutputWindowLogger.cpp
    src/PluginInterface2.cpp
    src/PS4Includes.cpp
    src/Rackspace.cpp
    src/RakMemoryOverride.cpp
    src/RakNetCommandParser.cpp
    src/RakNetSocket.cpp
    src/RakNetSocket2.cpp
    src/RakNetSocket2_360_720.cpp
    src/RakNetSocket2_Berkley.cpp
    src/RakNetSocket2_Berkley_NativeClient.cpp
    src/RakNetSocket2_NativeClient.cpp
    src/RakNetSocket2_PS3_PS4.cpp
    src/RakNetSocket2_PS4.cpp
    src/RakNetSocket2_Vita.cpp
    src/RakNetSocket2_Windows_Linux.cpp
    src/RakNetSocket2_Windows_Linux_360.cpp
    src/RakNetStatistics.cpp
    src/RakNetTransport2.cpp
    src/RakNetTypes.cpp
    src/RakPeer.cpp
    src/RakSleep.cpp
    src/RakString.cpp
    src/RakThread.cpp
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
    src/TelnetTransport.cpp
    src/ThreadsafePacketLogger.cpp
    src/TwoWayAuthentication.cpp
    src/UDPForwarder.cpp
    src/UDPProxyClient.cpp
    src/UDPProxyCoordinator.cpp
    src/UDPProxyServer.cpp
    src/VariableDeltaSerializer.cpp
    src/VariableListDeltaTracker.cpp
    src/VariadicSQLParser.cpp
    src/VitaIncludes.cpp
    src/WSAStartupSingleton.cpp
)

set(MAFIANET_CRYPTO_SOURCES
    src/crypto/cryptomanager.cpp
    src/crypto/factory.cpp
    src/crypto/fileencrypter.cpp
    src/crypto/securestring.cpp
)

set(MAFIANET_HEADERS
    include/slikenet/_FindFirst.h
    include/slikenet/alloca.h
    include/slikenet/assert.h
    include/slikenet/AutopatcherPatchContext.h
    include/slikenet/AutopatcherRepositoryInterface.h
    include/slikenet/Base64Encoder.h
    include/slikenet/BitStream.h
    include/slikenet/CCRakNetSlidingWindow.h
    include/slikenet/CCRakNetUDT.h
    include/slikenet/CheckSum.h
    include/slikenet/CloudClient.h
    include/slikenet/CloudCommon.h
    include/slikenet/CloudServer.h
    include/slikenet/commandparser.h
    include/slikenet/CommandParserInterface.h
    include/slikenet/ConnectionGraph2.h
    include/slikenet/ConsoleServer.h
    include/slikenet/DataCompressor.h
    include/slikenet/defineoverrides.h
    include/slikenet/defines.h
    include/slikenet/DirectoryDeltaTransfer.h
    include/slikenet/DR_SHA1.h
    include/slikenet/DS_BinarySearchTree.h
    include/slikenet/DS_BPlusTree.h
    include/slikenet/DS_BytePool.h
    include/slikenet/DS_ByteQueue.h
    include/slikenet/DS_Hash.h
    include/slikenet/DS_Heap.h
    include/slikenet/DS_HuffmanEncodingTree.h
    include/slikenet/DS_HuffmanEncodingTreeFactory.h
    include/slikenet/DS_HuffmanEncodingTreeNode.h
    include/slikenet/DS_LinkedList.h
    include/slikenet/DS_List.h
    include/slikenet/DS_Map.h
    include/slikenet/DS_MemoryPool.h
    include/slikenet/DS_Multilist.h
    include/slikenet/DS_OrderedChannelHeap.h
    include/slikenet/DS_OrderedList.h
    include/slikenet/DS_Queue.h
    include/slikenet/DS_QueueLinkedList.h
    include/slikenet/DS_RangeList.h
    include/slikenet/DS_Table.h
    include/slikenet/DS_ThreadsafeAllocatingQueue.h
    include/slikenet/DS_Tree.h
    include/slikenet/DS_WeightedGraph.h
    include/slikenet/DynDNS.h
    include/slikenet/EmailSender.h
    include/slikenet/EmptyHeader.h
    include/slikenet/EpochTimeToString.h
    include/slikenet/Export.h
    include/slikenet/FileList.h
    include/slikenet/FileListNodeContext.h
    include/slikenet/FileListTransfer.h
    include/slikenet/FileListTransferCBInterface.h
    include/slikenet/FileOperations.h
    include/slikenet/FormatString.h
    include/slikenet/FullyConnectedMesh2.h
    include/slikenet/Getche.h
    include/slikenet/Gets.h
    include/slikenet/GetTime.h
    include/slikenet/gettimeofday.h
    include/slikenet/GridSectorizer.h
    include/slikenet/HTTPConnection.h
    include/slikenet/HTTPConnection2.h
    include/slikenet/IncrementalReadInterface.h
    include/slikenet/InternalPacket.h
    include/slikenet/Itoa.h
    include/slikenet/Kbhit.h
    include/slikenet/linux_adapter.h
    include/slikenet/LinuxStrings.h
    include/slikenet/LocklessTypes.h
    include/slikenet/LogCommandParser.h
    include/slikenet/memoryoverride.h
    include/slikenet/MessageFilter.h
    include/slikenet/MessageIdentifiers.h
    include/slikenet/MTUSize.h
    include/slikenet/NativeFeatureIncludes.h
    include/slikenet/NativeFeatureIncludesOverrides.h
    include/slikenet/NativeTypes.h
    include/slikenet/NatPunchthroughClient.h
    include/slikenet/NatPunchthroughServer.h
    include/slikenet/NatTypeDetectionClient.h
    include/slikenet/NatTypeDetectionCommon.h
    include/slikenet/NatTypeDetectionServer.h
    include/slikenet/NetworkIDManager.h
    include/slikenet/NetworkIDObject.h
    include/slikenet/osx_adapter.h
    include/slikenet/PacketConsoleLogger.h
    include/slikenet/PacketFileLogger.h
    include/slikenet/PacketizedTCP.h
    include/slikenet/PacketLogger.h
    include/slikenet/PacketOutputWindowLogger.h
    include/slikenet/PacketPool.h
    include/slikenet/PacketPriority.h
    include/slikenet/peer.h
    include/slikenet/peerinterface.h
    include/slikenet/PluginInterface2.h
    include/slikenet/PS3Includes.h
    include/slikenet/PS4Includes.h
    include/slikenet/Rackspace.h
    include/slikenet/Rand.h
    include/slikenet/RandSync.h
    include/slikenet/ReadyEvent.h
    include/slikenet/RefCountedObj.h
    include/slikenet/RelayPlugin.h
    include/slikenet/ReliabilityLayer.h
    include/slikenet/ReplicaEnums.h
    include/slikenet/ReplicaManager3.h
    include/slikenet/Router2.h
    include/slikenet/RPC4Plugin.h
    include/slikenet/SecureHandshake.h
    include/slikenet/SendToThread.h
    include/slikenet/SignaledEvent.h
    include/slikenet/SimpleMutex.h
    include/slikenet/SimpleTCPServer.h
    include/slikenet/SingleProducerConsumer.h
    include/slikenet/sleep.h
    include/slikenet/slikenet.h
    include/slikenet/smartptr.h
    include/slikenet/socket.h
    include/slikenet/socket2.h
    include/slikenet/SocketDefines.h
    include/slikenet/SocketIncludes.h
    include/slikenet/SocketLayer.h
    include/slikenet/statistics.h
    include/slikenet/StatisticsHistory.h
    include/slikenet/string.h
    include/slikenet/StringCompressor.h
    include/slikenet/StringTable.h
    include/slikenet/SuperFastHash.h
    include/slikenet/TableSerializer.h
    include/slikenet/TCPInterface.h
    include/slikenet/TeamBalancer.h
    include/slikenet/TeamManager.h
    include/slikenet/TelnetTransport.h
    include/slikenet/thread.h
    include/slikenet/ThreadPool.h
    include/slikenet/ThreadsafePacketLogger.h
    include/slikenet/time.h
    include/slikenet/transport2.h
    include/slikenet/TransportInterface.h
    include/slikenet/TwoWayAuthentication.h
    include/slikenet/types.h
    include/slikenet/UDPForwarder.h
    include/slikenet/UDPProxyClient.h
    include/slikenet/UDPProxyCommon.h
    include/slikenet/UDPProxyCoordinator.h
    include/slikenet/UDPProxyServer.h
    include/slikenet/VariableDeltaSerializer.h
    include/slikenet/VariableListDeltaTracker.h
    include/slikenet/VariadicSQLParser.h
    include/slikenet/version.h
    include/slikenet/VitaIncludes.h
    include/slikenet/WindowsIncludes.h
    include/slikenet/WSAStartupSingleton.h
    include/slikenet/wstring.h
    include/slikenet/XBox360Includes.h
)

set(MAFIANET_CRYPTO_HEADERS
    include/slikenet/crypto/cryptomanager.h
    include/slikenet/crypto/factory.h
    include/slikenet/crypto/fileencrypter.h
    include/slikenet/crypto/ifileencrypter.h
    include/slikenet/crypto/securestring.h
)

# Combine all sources
set(MAFIANET_ALL_SOURCES
    ${MAFIANET_SOURCES}
    ${MAFIANET_CRYPTO_SOURCES}
)

set(MAFIANET_ALL_HEADERS
    ${MAFIANET_HEADERS}
    ${MAFIANET_CRYPTO_HEADERS}
)

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
    )

    target_compile_definitions(${target_name}
        PRIVATE
            $<$<CONFIG:Debug>:_DEBUG>
            $<$<CONFIG:Release>:NDEBUG>
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

# Install targets
install(TARGETS ${MAFIANET_INSTALL_TARGETS}
    EXPORT MafiaNetTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
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
