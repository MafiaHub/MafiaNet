# MafiaNet Library
#
# Copyright (c) 2024, MafiaHub
# Licensed under MIT-style license

# Batched datagram I/O (recvmmsg/sendmmsg) needs no option: it is a platform
# capability, guarded by a plain #if defined(__linux__), and is always on where
# the syscalls exist. See docs/getting-started/building.rst.

# Source files (explicit list, no GLOB)
set(MAFIANET_SOURCES
    src/find_first.cpp
    src/base64_encoder.cpp
    src/bit_stream.cpp
    src/cc_sliding_window.cpp
    src/cc_udt.cpp
    src/check_sum.cpp
    src/cloud_client.cpp
    src/cloud_common.cpp
    src/cloud_server.cpp
    src/connection_graph2.cpp
    src/data_compressor.cpp
    src/directory_delta_transfer.cpp
    src/dr_sha1.cpp
    src/ds_byte_pool.cpp
    src/ds_byte_queue.cpp
    src/ds_huffman_encoding_tree.cpp
    src/ds_table.cpp
    src/dyn_dns.cpp
    src/epoch_time_to_string.cpp
    src/file_list.cpp
    src/file_list_transfer.cpp
    src/file_operations.cpp
    src/format_string.cpp
    src/fully_connected_mesh2.cpp
    src/getche.cpp
    src/gets.cpp
    src/get_time.cpp
    src/get_time_of_day.cpp
    src/grid_sectorizer.cpp
    src/guid_util.cpp
    src/incremental_read_interface.cpp
    src/itoa.cpp
    src/linux_adapter.cpp
    src/linux_strings.cpp
    src/lockless_types.cpp
    src/message_filter.cpp
    src/mmsg_batch.cpp
    src/mtu_black_hole.cpp
    src/nat_punchthrough_client.cpp
    src/nat_punchthrough_server.cpp
    src/nat_type_detection_client.cpp
    src/nat_type_detection_common.cpp
    src/nat_type_detection_server.cpp
    src/network_id_manager.cpp
    src/network_id_object.cpp
    src/osx_adapter.cpp
    src/packet_file_logger.cpp
    src/packetized_tcp.cpp
    src/packet_logger.cpp
    src/packet_output_window_logger.cpp
    src/peer_handle.cpp
    src/plugin_interface2.cpp
    src/point_grid_sectorizer.cpp
    src/rackspace.cpp
    src/memory_override.cpp
    src/socket2.cpp
    src/socket2_berkley.cpp
    src/socket2_windows_linux.cpp
    src/statistics.cpp
    src/types.cpp
    src/peer.cpp
    src/sleep.cpp
    src/string.cpp
    src/thread.cpp
    src/voice.cpp
    src/wstring.cpp
    src/rand.cpp
    src/rand_sync.cpp
    src/ready_event.cpp
    src/relay_plugin.cpp
    src/reliability_layer.cpp
    src/replica_manager3.cpp
    src/router2.cpp
    src/rpc4_plugin.cpp
    src/secure_handshake.cpp
    src/send_to_thread.cpp
    src/signaled_event.cpp
    src/simple_mutex.cpp
    src/socket_layer.cpp
    src/statistics_history.cpp
    src/string_compressor.cpp
    src/string_table.cpp
    src/super_fast_hash.cpp
    src/table_serializer.cpp
    src/tcp_interface.cpp
    src/team_balancer.cpp
    src/team_manager.cpp
    src/threadsafe_packet_logger.cpp
    src/two_way_authentication.cpp
    src/udp_forwarder.cpp
    src/udp_proxy_client.cpp
    src/udp_proxy_coordinator.cpp
    src/udp_proxy_server.cpp
    src/variable_delta_serializer.cpp
    src/variable_list_delta_tracker.cpp
    src/variadic_sql_parser.cpp
    src/wsa_startup_singleton.cpp
)

# Extras: utilities that are not game networking (SMTP, HTTP client, telnet and
# console command transports, console packet logger). Built into the separate
# MafiaNetExtras target when MAFIANET_BUILD_EXTRAS is ON; their headers stay in
# include/mafianet/ so include paths are unchanged. See issue #50, stage 2.
set(MAFIANET_EXTRAS_SOURCES
    src/command_parser_interface.cpp
    src/console_server.cpp
    src/email_sender.cpp
    src/http_connection.cpp
    src/http_connection2.cpp
    src/log_command_parser.cpp
    src/packet_console_logger.cpp
    src/command_parser.cpp
    src/transport2.cpp
    src/telnet_transport.cpp
)

set(MAFIANET_CRYPTO_SOURCES
    src/crypto/cryptomanager.cpp
    src/crypto/factory.cpp
    src/crypto/fileencrypter.cpp
    src/crypto/securestring.cpp
)

set(MAFIANET_HEADERS
    include/mafianet/find_first.h
    include/mafianet/alloca.h
    include/mafianet/assert.h
    include/mafianet/autopatcher_patch_context.h
    include/mafianet/autopatcher_repository_interface.h
    include/mafianet/base64_encoder.h
    include/mafianet/bit_stream.h
    include/mafianet/cc_sliding_window.h
    include/mafianet/cc_udt.h
    include/mafianet/check_sum.h
    include/mafianet/cloud_client.h
    include/mafianet/cloud_common.h
    include/mafianet/cloud_server.h
    include/mafianet/command_parser.h
    include/mafianet/command_parser_interface.h
    include/mafianet/connection_graph2.h
    include/mafianet/console_server.h
    include/mafianet/data_compressor.h
    include/mafianet/define_overrides.h
    include/mafianet/defines.h
    include/mafianet/directory_delta_transfer.h
    include/mafianet/dr_sha1.h
    include/mafianet/ds_binary_search_tree.h
    include/mafianet/ds_byte_pool.h
    include/mafianet/ds_byte_queue.h
    include/mafianet/ds_hash.h
    include/mafianet/ds_heap.h
    include/mafianet/ds_huffman_encoding_tree.h
    include/mafianet/ds_huffman_encoding_tree_factory.h
    include/mafianet/ds_huffman_encoding_tree_node.h
    include/mafianet/ds_linked_list.h
    include/mafianet/ds_list.h
    include/mafianet/ds_map.h
    include/mafianet/ds_memory_pool.h
    include/mafianet/ds_multilist.h
    include/mafianet/ds_ordered_list.h
    include/mafianet/ds_queue.h
    include/mafianet/ds_queue_linked_list.h
    include/mafianet/ds_range_list.h
    include/mafianet/ds_table.h
    include/mafianet/ds_threadsafe_allocating_queue.h
    include/mafianet/dyn_dns.h
    include/mafianet/email_sender.h
    include/mafianet/empty_header.h
    include/mafianet/epoch_time_to_string.h
    include/mafianet/export.h
    include/mafianet/file_list.h
    include/mafianet/file_list_node_context.h
    include/mafianet/file_list_transfer.h
    include/mafianet/file_list_transfer_cb_interface.h
    include/mafianet/file_operations.h
    include/mafianet/format_string.h
    include/mafianet/fully_connected_mesh2.h
    include/mafianet/getche.h
    include/mafianet/gets.h
    include/mafianet/get_time.h
    include/mafianet/get_time_of_day.h
    include/mafianet/grid_sectorizer.h
    include/mafianet/guid_util.h
    include/mafianet/http_connection.h
    include/mafianet/http_connection2.h
    include/mafianet/incremental_read_interface.h
    include/mafianet/internal_packet.h
    include/mafianet/itoa.h
    include/mafianet/kbhit.h
    include/mafianet/linux_adapter.h
    include/mafianet/linux_strings.h
    include/mafianet/lockless_types.h
    include/mafianet/log_command_parser.h
    include/mafianet/memory_override.h
    include/mafianet/message_filter.h
    include/mafianet/message_identifiers.h
    include/mafianet/mmsg_batch.h
    include/mafianet/mtu_black_hole.h
    include/mafianet/mtu_size.h
    include/mafianet/native_feature_includes.h
    include/mafianet/native_feature_includes_overrides.h
    include/mafianet/native_types.h
    include/mafianet/nat_punchthrough_client.h
    include/mafianet/nat_punchthrough_server.h
    include/mafianet/nat_type_detection_client.h
    include/mafianet/nat_type_detection_common.h
    include/mafianet/nat_type_detection_server.h
    include/mafianet/network_id_manager.h
    include/mafianet/network_id_object.h
    include/mafianet/osx_adapter.h
    include/mafianet/packet_console_logger.h
    include/mafianet/packet_file_logger.h
    include/mafianet/packetized_tcp.h
    include/mafianet/packet_logger.h
    include/mafianet/packet_output_window_logger.h
    include/mafianet/packet_pool.h
    include/mafianet/packet_priority.h
    include/mafianet/peer_handle.h
    include/mafianet/peer.h
    include/mafianet/peer_interface.h
    include/mafianet/plugin_interface2.h
    include/mafianet/point_grid_sectorizer.h
    include/mafianet/rackspace.h
    include/mafianet/voice.h
    include/mafianet/rand.h
    include/mafianet/rand_sync.h
    include/mafianet/ready_event.h
    include/mafianet/ref_counted_obj.h
    include/mafianet/relay_plugin.h
    include/mafianet/reliability_layer.h
    include/mafianet/replica_enums.h
    include/mafianet/replica_manager3.h
    include/mafianet/router2.h
    include/mafianet/rpc4_plugin.h
    include/mafianet/secure_handshake.h
    include/mafianet/send_to_thread.h
    include/mafianet/signaled_event.h
    include/mafianet/simple_mutex.h
    include/mafianet/simple_tcp_server.h
    include/mafianet/single_producer_consumer.h
    include/mafianet/sleep.h
    include/mafianet/mafianet.h
    include/mafianet/smart_ptr.h
    include/mafianet/socket2.h
    include/mafianet/socket_defines.h
    include/mafianet/socket_includes.h
    include/mafianet/socket_layer.h
    include/mafianet/statistics.h
    include/mafianet/statistics_history.h
    include/mafianet/string.h
    include/mafianet/string_compressor.h
    include/mafianet/string_table.h
    include/mafianet/super_fast_hash.h
    include/mafianet/table_serializer.h
    include/mafianet/tcp_interface.h
    include/mafianet/team_balancer.h
    include/mafianet/team_manager.h
    include/mafianet/telnet_transport.h
    include/mafianet/thread.h
    include/mafianet/thread_pool.h
    include/mafianet/threadsafe_packet_logger.h
    include/mafianet/time.h
    include/mafianet/transport2.h
    include/mafianet/transport_interface.h
    include/mafianet/two_way_authentication.h
    include/mafianet/types.h
    include/mafianet/udp_forwarder.h
    include/mafianet/udp_proxy_client.h
    include/mafianet/udp_proxy_common.h
    include/mafianet/udp_proxy_coordinator.h
    include/mafianet/udp_proxy_server.h
    include/mafianet/variable_delta_serializer.h
    include/mafianet/variable_list_delta_tracker.h
    include/mafianet/variadic_sql_parser.h
    include/mafianet/version.h
    include/mafianet/virtual_world.h
    include/mafianet/virtual_world_replica3.h
    include/mafianet/windows_includes.h
    include/mafianet/wsa_startup_singleton.h
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
    ${MAFIANET_CRYPTO_HEADERS}
)

# Optional plugins, one per _RAKNET_SUPPORT_<Name> flag in native_feature_includes.h.
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
            # native_feature_includes.h matches the library it links against.
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
