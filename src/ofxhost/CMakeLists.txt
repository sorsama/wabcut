# OpenFX video effect plug-ins (0.7, docs/plugins.md, "OpenFX video plug-ins"): host .ofx plug-ins on clips like built-in
# effects, out of process (the model is the audio plug-in host, src/plughost):
#
#   wabcut_openfx_headers    the OpenFX API headers, vendored unmodified (third_party/openfx, BSD-3-Clause)
#   wabcut_ofxhost_common    the wire both sides share: Protocol.h (shared-memory pictures, limits); the processes, shared memory
#                            and base64 come from wabcut_plughost_common
#   wabcut-ofxhost           the helper process that loads one .ofx binary, describes its plug-ins (scan mode) and runs
#                            instances: the OpenFX property, parameter, image effect, memory, multi-thread and message suites
#   wabcut_ofx               the editor side: the plug-in catalog (scanner, cache, blocklist, effect descriptors), the host
#                            processes and instances, and the processor that runs a layer's OpenFX effects on its decoded
#                            frame for the playback layer (playback/video/ExternalEffects.h)
#
# Windows only for now (Win32 processes, shared memory and job objects); built with the audio plug-in host.

find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_openfx_headers INTERFACE)
target_include_directories(wabcut_openfx_headers SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openfx/include)

add_library(wabcut_ofxhost_common STATIC
    common/include/wabcut/ofxhost/Protocol.h
    common/src/Protocol.cpp
)
target_include_directories(wabcut_ofxhost_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common/include)
target_link_libraries(wabcut_ofxhost_common PUBLIC wabcut_plughost_common)
wabcut_target_defaults(wabcut_ofxhost_common)

add_executable(wabcut-ofxhost
    helper/OfxHost.h
    helper/OfxModule.cpp
    helper/OfxSuites.cpp
    helper/main.cpp
)
target_link_libraries(wabcut-ofxhost PRIVATE wabcut_ofxhost_common wabcut_openfx_headers nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut-ofxhost)

add_library(wabcut_ofx STATIC
    client/include/wabcut/ofxhost/OfxCatalog.h
    client/include/wabcut/ofxhost/OfxEffects.h
    client/include/wabcut/ofxhost/OfxPluginHost.h
    client/include/wabcut/ofxhost/OfxSystem.h
    client/src/OfxCatalog.cpp
    client/src/OfxEffects.cpp
    client/src/OfxParams.cpp
    client/src/OfxParams.h
    client/src/OfxPictures.cpp
    client/src/OfxPictures.h
    client/src/OfxPluginHost.cpp
    client/src/OfxSystem.cpp
)
target_include_directories(wabcut_ofx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
target_link_libraries(wabcut_ofx PUBLIC wabcut_ofxhost_common wabcut_playback wabcut_media wabcut_render wabcut_model
                                         wabcut_audio_edit
                                  PRIVATE nlohmann_json::nlohmann_json)
# Hosts run the helper from next to themselves: build it with the library.
add_dependencies(wabcut_ofx wabcut-ofxhost)
wabcut_target_defaults(wabcut_ofx)
