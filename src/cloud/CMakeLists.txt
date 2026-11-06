# wabcut_cloud - review links, cloud projects on the user's own storage and stock media (0.7, docs/cloud.md).
# A storage seam (RemoteStore) with a folder store (OneDrive / Dropbox / Google Drive / a share, synced by the OS client), a WebDAV
# store and an S3-compatible store over the app's network stack (wabcut_ai's IHttpTransport); the editing lock and the revisions of
# a cloud project; the static review page and its links; the stock-media providers. Pure C++ (no Qt): the Qt transport and the
# keychain are in the app. Owner: Core / Project panel.
find_package(pugixml CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(wabcut_cloud STATIC
    src/CloudLock.cpp
    src/CloudProject.cpp
    src/Common.cpp
    src/FileUtil.cpp
    src/FileUtil.h
    src/FolderStore.cpp
    src/HttpCommon.cpp
    src/HttpCommon.h
    src/RemoteStore.cpp
    src/ReviewLink.cpp
    src/ReviewPage.cpp
    src/S3Store.cpp
    src/Stock.cpp
    src/WebDavStore.cpp
)
target_include_directories(wabcut_cloud PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_cloud
    PUBLIC wabcut_model wabcut_ai
    PRIVATE pugixml::pugixml nlohmann_json::nlohmann_json Threads::Threads)
wabcut_target_defaults(wabcut_cloud)
