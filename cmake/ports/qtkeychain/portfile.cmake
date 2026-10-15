# QtKeychain 0.17.0 for Wabcut (overlay port; owner: AI platform). docs/ai.md: "Keys: QtKeychain 0.17 overlay port".
#
# Why an overlay: the registry port qtkeychain-qt6 is 0.14.3 and depends on vcpkg's qtbase. Wabcut uses an external
# Qt (the official installer, WABCUT_QT_DIR), and a second Qt from vcpkg would take hours to build and load a second
# Qt6Core into the process. This port builds against the app's Qt instead:
#   - WABCUT_QT_DIR (or QT_ROOT_DIR, set by install-qt-action in CI) names that Qt;
#   - on Windows vcpkg builds ports in a clean environment, so the variable must be passed in with
#     VCPKG_KEEP_ENV_VARS=WABCUT_QT_DIR (tools/dev-env.ps1 does). The triplet is deliberately left alone: changing it
#     would change the ABI of every cached package;
#   - wabcut-qt.cmake pins the Qt version into the package ABI, and the build stops if the Qt found differs.
# 0.17 names Windows Credential Manager entries "key@service" (0.16 used "key"); Wabcut never stored keys with an older
# QtKeychain, so no migration is needed.

include("${CMAKE_CURRENT_LIST_DIR}/wabcut-qt.cmake")

# Static, also on dynamic triplets: windeployqt takes any DLL named Qt6*.dll for a Qt module and fails when it is not
# in the Qt kit (qt6keychain.dll is not), which would break the app's post-build deploy and the install step. A static
# library leaves nothing to deploy.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

set(qt_dir "$ENV{WABCUT_QT_DIR}")
if(NOT qt_dir)
    set(qt_dir "$ENV{QT_ROOT_DIR}")
endif()
file(TO_CMAKE_PATH "${qt_dir}" qt_dir)
if(NOT qt_dir OR NOT EXISTS "${qt_dir}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR
        "qtkeychain (Wabcut overlay port): no Qt 6 found. Set WABCUT_QT_DIR to the Qt the app uses "
        "(e.g. C:/Qt/${WABCUT_KEYCHAIN_QT_VERSION}/msvc2022_64) and, on Windows, VCPKG_KEEP_ENV_VARS=WABCUT_QT_DIR "
        "so it reaches the port build. '. .\\tools\\dev-env.ps1' sets both. Got: '${qt_dir}'")
endif()

# The Qt version must match the one pinned in wabcut-qt.cmake.
file(STRINGS "${qt_dir}/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" qt_version_line REGEX "set\\(PACKAGE_VERSION ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" qt_version "${qt_version_line}")
if(NOT qt_version STREQUAL WABCUT_KEYCHAIN_QT_VERSION)
    message(FATAL_ERROR
        "qtkeychain (Wabcut overlay port): found Qt '${qt_version}' in ${qt_dir}, but the port is pinned to "
        "${WABCUT_KEYCHAIN_QT_VERSION} (cmake/ports/qtkeychain/wabcut-qt.cmake). Update the pin together with Wabcut's Qt.")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO frankosterfeld/qtkeychain
    REF "${VERSION}"
    SHA512 7a9c6a4f5b19c36283c18b426ee057e08250266411eaa7012af54d2453b9a91edcfba17be2508405fab9b42732bc33202fc708cc59f2ee9e650d978782e49703
    HEAD_REF main
    PATCHES
        windows-credential-fixes.patch
)

set(platform_options "")
if(VCPKG_TARGET_IS_LINUX)
    # The system's libsecret (pkg-config libsecret-1), loaded at run time; vcpkg's would bring a second glib.
    list(APPEND platform_options -DLIBSECRET_SUPPORT=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_PREFIX_PATH=${qt_dir}"
        "-DQt6_DIR=${qt_dir}/lib/cmake/Qt6"
        -DBUILD_WITH_QT5=OFF
        -DBUILD_TRANSLATIONS=OFF
        -DBUILD_TEST_APPLICATION=OFF
        -DBUILD_QTQUICK_DEMO=OFF
        -DBUILD_TESTING=OFF
        -DUSE_COMPAT_NAMING_SCHEME=OFF
        ${platform_options}
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/Qt6Keychain PACKAGE_NAME Qt6Keychain)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/mkspecs"
    "${CURRENT_PACKAGES_DIR}/debug/mkspecs"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
