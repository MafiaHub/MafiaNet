vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MafiaHub/MafiaNet
    REF "v${VERSION}"
    SHA512 9fe6fe79149f9e77a3cbaf9a9dcd7f87ebaf05f396021d703ebd60a35356ce698fc6009aeac0fad9f93194b12943f4c9bd5996f47fd94148f830aa7aa6246baf
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static"  MAFIANET_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" MAFIANET_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMAFIANET_BUILD_STATIC=${MAFIANET_STATIC}
        -DMAFIANET_BUILD_SHARED=${MAFIANET_SHARED}
        -DMAFIANET_BUILD_SAMPLES=OFF
        -DMAFIANET_BUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME MafiaNet CONFIG_PATH lib/cmake/MafiaNet)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
