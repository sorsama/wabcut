# wabcut_interchange — pure C++: one-click import (FCP7 XML, FCPXML, OTIO JSON, EDL, .prproj), ImportReport, media relink,
# Lottie (Bodymovin JSON) as motion graphics (C.13), timeline export to FCP7 XML, FCPXML, OTIO and EDL (X.1), and the
# hand-off to audio post: AAF (MS-CFB structured storage) and OMF 2.0 export with the audio rendered by the host (P.2).
# Owner: Interchange. Public headers under include/wabcut/interchange/, private sources under src/.
find_package(pugixml CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_interchange STATIC
    src/AafExporter.cpp
    src/AafFile.cpp
    src/AafFile.h
    src/AafModel.cpp
    src/AafModel.h
    src/AafReader.cpp
    src/AafReader.h
    src/AudioPost.cpp
    src/AudioPost.h
    src/Bento.cpp
    src/Bento.h
    src/CaptionBroadcast.cpp
    src/CaptionFiles.cpp
    src/CaptionFormats.h
    src/CaptionTtml.cpp
    src/Cea608.cpp
    src/Cea608.h
    src/Cfb.cpp
    src/Cfb.h
    src/Docx.cpp
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
    src/MogrtImporter.cpp
    src/Importers.h
    src/Ir.h
    src/MediaRelinker.cpp
    src/OmfExporter.cpp
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
    src/Xmp.cpp
    src/XmlUtil.h
    src/Zip.cpp
)
target_include_directories(wabcut_interchange PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_interchange PUBLIC cxx_std_23)
# XML, JSON and gzip are implementation details; no public header includes them.
target_link_libraries(wabcut_interchange
    PUBLIC wabcut_model
    PRIVATE pugixml::pugixml ZLIB::ZLIB nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_interchange)
# AAF / OMF files name the product version that wrote them (Identification). Only the exporter sees it, so a version
# bump rebuilds one file.
set_property(SOURCE src/AafExporter.cpp APPEND PROPERTY COMPILE_DEFINITIONS "WABCUT_VERSION=\"${PROJECT_VERSION}\"")
