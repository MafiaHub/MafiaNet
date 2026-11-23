# CI runners start with an empty build directory. Bring up the daemon once
# before parallel compiler processes, which can race or time out at startup.
execute_process(COMMAND sccache --start-server RESULT_VARIABLE startup)
if(NOT startup STREQUAL "0")
    message(WARNING "sccache startup failed. Building directly with the compiler.")
    set(MAFIANET_USE_COMPILER_CACHE OFF CACHE BOOL "Use the compiler cache" FORCE)
endif()
