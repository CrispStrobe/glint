vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO CrispStrobe/glint
    REF v0.4.1
    SHA512 0  # update with actual hash
)

set(GLINT_MODE "double")
if("fixed-point" IN_LIST FEATURES)
    set(GLINT_MODE "fixed")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DGLINT_MODE=${GLINT_MODE}
)

vcpkg_cmake_build()

# Install headers and libraries
file(INSTALL "${SOURCE_PATH}/include/glint" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/libglint.a"
     DESTINATION "${CURRENT_PACKAGES_DIR}/lib" OPTIONAL)
file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/libglint.a"
     DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib" OPTIONAL)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
