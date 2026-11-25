# Stands in for vcpkg's libsodium port, whose unofficial-sodiumConfig.cmake defines the
# unofficial-sodium::sodium imported target, so CI can check MAFIANET_USE_UNOFFICIAL_SODIUM
# without a vcpkg toolchain: it wraps the system libsodium the same way.
if(NOT TARGET unofficial-sodium::sodium)
  find_path(UNOFFICIAL_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
  find_library(UNOFFICIAL_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
  add_library(unofficial-sodium::sodium UNKNOWN IMPORTED)
  set_target_properties(unofficial-sodium::sodium PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${UNOFFICIAL_SODIUM_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${UNOFFICIAL_SODIUM_LIBRARY}")
endif()
set(unofficial-sodium_FOUND TRUE)
