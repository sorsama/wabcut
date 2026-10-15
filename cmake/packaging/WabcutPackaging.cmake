# CPack: the portable ZIP and, on Windows, the per-user MSI installer. Included by cmake/WabcutInstall.cmake after the
# install rules. See docs/release.md.
#
#   cpack --config build/win-msvc/CPackConfig.cmake            every generator found (ZIP, and WIX with WiX 5)
#   cpack --config build/win-msvc/CPackConfig.cmake -G WIX     the MSI only
#
# Windows packages (build/<preset>/package/):
#   wabcut-<version>-windows-x64.msi            per-user Windows Installer package: no administrator rights, installs
#                                               to %LOCALAPPDATA%\Programs\Wabcut, Start menu entry, Settings > Apps
#                                               entry, opt-in .wabcut association (cmake/packaging/wix/)
#   wabcut-<version>-windows-x64-portable.zip   the same install tree, run from any folder without installing
#
# Why WiX (MSI) and not NSIS: the WiX Toolset is a .NET tool, and the .NET SDK already on the build machines installs it
# per user with `dotnet tool install` (cmake/packaging/Install-WixToolset.ps1), without administrator rights; NSIS
# would be a separately downloaded binary. Windows Installer gives per-user installs natively (no elevation prompt),
# transactional install with rollback, an uninstaller that removes exactly the files and registry values the package
# installed (never user data), silent installs for IT (`msiexec /i ... /qn`), and major upgrades by UpgradeCode.
# WiX 5.0.2 is pinned: it uses the v4 schema CPack writes (CPACK_WIX_VERSION 4) and is plain MS-RL. WiX 6 added the
# Open Source Maintenance Fee to its EULA and WiX 7 refuses to run until each build account accepts it
# (`wix eula accept wix7`), a decision for the project, not for a build script.

include_guard(GLOBAL)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" wabcut_os)
set(wabcut_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(wabcut_arch MATCHES "^(AMD64|amd64|x86_64)$")
    set(wabcut_arch x64)
elseif(wabcut_arch MATCHES "^(ARM64|arm64|aarch64)$")
    set(wabcut_arch arm64)
endif()
set(WABCUT_PACKAGE_BASENAME "wabcut-${PROJECT_VERSION}-${wabcut_os}-${wabcut_arch}")

set(CPACK_PACKAGE_NAME Wabcut)
set(CPACK_PACKAGE_VENDOR "Wabcut contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
# CPack's WiX generator takes a .txt or .rtf license (the first installer page shows it).
configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/packaging/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/packaging/LICENSE.txt")
set(CPACK_PACKAGE_FILE_NAME "${WABCUT_PACKAGE_BASENAME}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON) # per generator in CPackProjectConfig.cmake.in (off for the MSI)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
# Start menu entry (WiX): bin/wabcut.exe as "Wabcut".
set(CPACK_PACKAGE_EXECUTABLES wabcut Wabcut)
set(CPACK_GENERATOR ZIP)

# ---- WiX (Windows Installer) ------------------------------------------------------------------------------------
set(WABCUT_WIX_TESTED_VERSION 5.0.2)
set(WABCUT_WIX_UPGRADE_GUID "9D0324E6-4508-405F-B844-7C96EBEA28D6") # never change: it links every Wabcut MSI
if(WIN32)
    find_program(WABCUT_WIX_EXECUTABLE wix
        HINTS "$ENV{WABCUT_WIX_DIR}" "$ENV{LOCALAPPDATA}/wabcut-tools/wix/${WABCUT_WIX_TESTED_VERSION}"
              "$ENV{USERPROFILE}/.dotnet/tools"
        DOC "WiX Toolset .NET tool for the MSI (cmake/packaging/Install-WixToolset.ps1 installs ${WABCUT_WIX_TESTED_VERSION} per user)")
    set(WABCUT_WIX_VERSION "")
    if(WABCUT_WIX_EXECUTABLE)
        execute_process(COMMAND "${WABCUT_WIX_EXECUTABLE}" --version
            OUTPUT_VARIABLE wabcut_wix_out RESULT_VARIABLE wabcut_wix_rc ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(wabcut_wix_rc EQUAL 0 AND wabcut_wix_out MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)")
            set(WABCUT_WIX_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(WABCUT_WIX_VERSION MATCHES "^[45]\\.")
        list(APPEND CPACK_GENERATOR WIX)
        get_filename_component(WABCUT_WIX_DIR "${WABCUT_WIX_EXECUTABLE}" DIRECTORY)
        message(STATUS "Wabcut: MSI installer with WiX ${WABCUT_WIX_VERSION} (${WABCUT_WIX_EXECUTABLE})")
    elseif(WABCUT_WIX_VERSION)
        message(STATUS "Wabcut: no MSI installer: WiX ${WABCUT_WIX_VERSION} found, but the packaging is written for "
                       "WiX 4/5 (tested ${WABCUT_WIX_TESTED_VERSION}; see docs/release.md)")
    else()
        message(STATUS "Wabcut: no MSI installer (WiX ${WABCUT_WIX_TESTED_VERSION} not found; run "
                       "cmake/packaging/Install-WixToolset.ps1, see docs/release.md). The portable ZIP is built.")
    endif()

    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_INSTALL_SCOPE perUser)
    # %LOCALAPPDATA%\Programs\Wabcut (FOLDERID_UserProgramFiles), the per-user program location. Not %LOCALAPPDATA%\
    # Wabcut, which holds the user's caches, models and crash reports: they never share a folder with program files.
    set(CPACK_WIX_ROOT_FOLDER_ID LocalAppDataFolder)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Programs/Wabcut")
    set(CPACK_WIX_UPGRADE_GUID "${WABCUT_WIX_UPGRADE_GUID}")
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/src/app/resources/brand/icons/wabcut.ico")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER ".") # the shortcut directly in the Start menu's Programs, no folder
    set(CPACK_WIX_UI_REF WixUI_InstallDir)
    set(CPACK_WIX_UI_BANNER "${CMAKE_CURRENT_LIST_DIR}/wix/banner.bmp")
    set(CPACK_WIX_UI_DIALOG "${CMAKE_CURRENT_LIST_DIR}/wix/dialog.bmp")
    set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_LIST_DIR}/wix/WabcutOptions.wxs")
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_LIST_DIR}/wix/wabcut-patch.xml")
    set(CPACK_WIX_PROPERTY_ARPCOMMENTS "Open-source non-linear video editor")
    set(CPACK_WIX_PROPERTY_ARPCONTACT "Wabcut contributors")
endif()

# Per-generator settings, read by cpack (not by CMake): the package names, and the WiX tool on PATH.
configure_file("${CMAKE_CURRENT_LIST_DIR}/CPackProjectConfig.cmake.in"
               "${CMAKE_BINARY_DIR}/packaging/CPackProjectConfig.cmake" @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/packaging/CPackProjectConfig.cmake")

# include(CPack) leaves CPACK_GENERATOR set to the *source* package generators (it reuses the variable to write
# CPackSourceConfig.cmake), so keep the binary list for install.package.
set(WABCUT_PACKAGE_GENERATORS ${CPACK_GENERATOR})
include(CPack)
