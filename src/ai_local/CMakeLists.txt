# Out-of-process local inference helpers. Owner: Local-AI engineer (wabcut-infer), AI platform engineer (wabcut-embed).
# Contract: docs/ai.md.
#   wabcut-infer      whisper.cpp transcription helper (WABCUT_WITH_LOCAL_AI; NDJSON protocol: wabcut/ai/InferProtocol.h)
#   wabcut_ai_local   what the CLI/app need around it: an OS HTTP transport for model downloads
#   wabcut-embed      the smart-search embedding helper, SigLIP 2 on ONNX Runtime, and the auto-reframe subject
#                     detectors (YuNet, YOLOX; AI-9), Enhance Speech and stem separation (A.4), caption translation
#                     (OPUS-MT; AI-12) (WABCUT_WITH_ONNX; protocol:
#                     wabcut/ai/search/EmbedProtocol.h)
if(WABCUT_WITH_LOCAL_AI)
    find_package(ggml CONFIG REQUIRED)
    find_package(whisper CONFIG REQUIRED)

    add_library(wabcut_ai_local STATIC src/HttpDownload.cpp)
    target_include_directories(wabcut_ai_local PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(wabcut_ai_local PUBLIC wabcut_ai)
    if(WIN32)
        target_link_libraries(wabcut_ai_local PRIVATE winhttp)
    endif()
    wabcut_target_defaults(wabcut_ai_local)

    add_executable(wabcut-infer infer/main.cpp)
    target_link_libraries(wabcut-infer PRIVATE wabcut_ai whisper ggml::ggml ggml::ggml-base)
    wabcut_target_defaults(wabcut-infer)

    # ggml loads its backends at run time (GGML_BACKEND_DL) from the helper's directory; nothing links them, so the
    # vcpkg app-local copy does not see them. Copy every backend module and the Vulkan loader next to the helper.
    set(_wabcut_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    file(GLOB _wabcut_ggml_modules "${_wabcut_vcpkg_bin}/ggml-cpu-*.dll" "${_wabcut_vcpkg_bin}/ggml-vulkan.dll"
         "${_wabcut_vcpkg_bin}/vulkan-1.dll" "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libggml-*.so")
    if(_wabcut_ggml_modules)
        add_custom_command(TARGET wabcut-infer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_wabcut_ggml_modules} "$<TARGET_FILE_DIR:wabcut-infer>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
                    "$<TARGET_FILE_DIR:wabcut-infer>/THIRD_PARTY_NOTICES-ai-local.md"
            COMMENT "Copying ggml backend modules and notices next to wabcut-infer"
            VERBATIM)
    else()
        message(WARNING "No ggml backend modules found in ${_wabcut_vcpkg_bin}: wabcut-infer will have no compute backend")
    endif()
endif()

if(WABCUT_WITH_ONNX)
    # Microsoft's prebuilt ONNX Runtime (overlay port cmake/ports/onnxruntime-prebuilt): never compiled here.
    find_package(onnxruntime-prebuilt CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(wabcut-embed embed/main.cpp embed/AudioModels.cpp embed/TranslateModels.cpp embed/SegmentModels.cpp)
    target_link_libraries(wabcut-embed PRIVATE wabcut_ai_search wabcut_ai_vision wabcut_ai_audio wabcut_ai_mt onnxruntime-prebuilt::onnxruntime
        nlohmann_json::nlohmann_json)
    wabcut_target_defaults(wabcut-embed)
    # onnxruntime.dll (+ onnxruntime_providers_shared.dll) next to the helper: they are loaded when it starts.
    add_custom_command(TARGET wabcut-embed POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ONNXRUNTIME_PREBUILT_RUNTIME_FILES} "$<TARGET_FILE_DIR:wabcut-embed>"
        COMMENT "Copying ONNX Runtime ${onnxruntime-prebuilt_VERSION} next to wabcut-embed"
        VERBATIM)
    message(STATUS "Wabcut: smart search helper wabcut-embed on ONNX Runtime ${onnxruntime-prebuilt_VERSION} (prebuilt)")
endif()
# GPU transcription is the Vulkan backend. Without it in the build (or, at run time, without a Vulkan driver and
# device) whisper runs on the CPU, and transcription results say so (ai::cpuFallbackNote).
if(_wabcut_ggml_modules AND NOT _wabcut_ggml_modules MATCHES "ggml-vulkan")
    message(STATUS "wabcut-infer: no ggml Vulkan backend in this build; local transcription runs on the CPU (slower)")
endif()
