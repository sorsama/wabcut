# wabcut_interchange — pure C++: one-click import (FCP7 XML, FCPXML, OTIO JSON, EDL, .prproj), ImportReport, media relink,
# Lottie (Bodymovin JSON) as motion graphics (C.13), and timeline export to FCP7 XML, FCPXML, OTIO and EDL (X.1).
# Owner: Interchange. Public headers under include/wabcut/interchange/, private sources under src/.
find_package(pugixml CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_interchange STATIC
    src/EdlImporter.cpp
    src/EdlExporter.cpp
    src/EffectMap.cpp
    src/EffectMap.h
    src/Export.cpp
    src/ExportDescribe.cpp
    src/Exporters.h
    src/Fcp7Exporter.cpp
    src/Fcp7Importer.cpp
    src/FcpXmlExporter.cpp
    src/FcpXmlImporter.cpp
    src/FileUrl.cpp
    src/Gzip.cpp
    src/Import.cpp
    src/ImportReport.cpp
    src/ImportSummary.cpp
    src/ImporterCommon.h
    src/LottieImporter.cpp
    src/Importers.h
    src/Ir.h
    src/MediaRelinker.cpp
    src/OtioExporter.cpp
    src/OtioImporter.cpp
    src/PremiereProject.cpp
    src/TextUtil.cpp
    src/TextUtil.h
    src/TimeMath.cpp
    src/TimeMath.h
    src/TimelineBuilder.cpp
    src/TimelineBuilder.h
    src/XmlUtil.cpp
    src/XmlUtil.h
)
target_include_directories(wabcut_interchange PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_interchange PUBLIC cxx_std_23)
# XML, JSON and gzip are implementation details; no public header includes them.
target_link_libraries(wabcut_interchange
    PUBLIC wabcut_model
    PRIVATE pugixml::pugixml ZLIB::ZLIB nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_interchange)
