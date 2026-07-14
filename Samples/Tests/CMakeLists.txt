mafianet_add_sample(NAME Tests FOLDER "Tests")

# Register every legacy test as its own CTest test so each one runs in a fresh
# process. The suite historically ran all tests sequentially inside a single
# binary, so lingering sockets, peers, and GUIDs from one test bled into the
# next — the main source of CI flakiness. Per-process isolation, a per-test
# timeout, and runner-level retries (ctest --repeat until-pass:N in CI) replace
# the ad-hoc retry loops individual tests had grown.
#
# This list must match the testList in Tests.cpp.
set(MAFIANET_LEGACY_TESTS
    EightPeerTest
    MaximumConnectTest
    PeerConnectDisconnectWithCancelPendingTest
    PeerConnectDisconnectTest
    ManyClientsOneServerBlockingTest
    ManyClientsOneServerNonBlockingTest
    ManyClientsOneServerDeallocateBlockingTest
    ReliableOrderedConvertedTest
    DroppedConnectionConvertTest
    ComprehensiveConvertTest
    CrossConnectionConvertTest
    PingTestsTest
    OfflineMessagesConvertTest
    LocalIsConnectedTest
    SecurityFunctionsTest
    ConnectWithSocketTest
    SystemAddressAndGuidTest
    PacketAndLowLevelTestsTest
    MiscellaneousTestsTest
    BitStreamStringTest
    RPC4ContextTest
    VirtualWorldTest
    DisconnectReasonTest
    PeerGuidTest
    PeerHandleTest
    PeerBuilderTest
    ArchiveTest
    DispatcherTest
    TypedSendTest
)

foreach(test_name IN LISTS MAFIANET_LEGACY_TESTS)
    add_test(NAME ${test_name} COMMAND Tests ${test_name})
    # RUN_SERIAL: most of these tests bind fixed loopback ports (e.g. 60000),
    # so two of them running concurrently under `ctest -j` would collide.
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 600
        RUN_SERIAL TRUE
        LABELS integration
    )
endforeach()
