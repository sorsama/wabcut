// Which data tracks hold a camera's gyro, and which this library reads (Gyro.h).

#include <wabcut/gyro/Gyro.h>

#include <algorithm>
#include <cctype>

namespace wabcut::gyro {

namespace {

bool containsNoCase(std::string_view haystack, std::string_view needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != haystack.end();
}

} // namespace

TrackKind classifyDataTrack(std::string_view fourcc, std::string_view handlerName) {
    if (fourcc == "gpmd") {
        return TrackKind::GoProGpmf;
    }
    if (fourcc == "rtmd") {
        return TrackKind::SonyRtmd;
    }
    // DJI cameras and drones: the sample entry is 'djmd' (per-frame camera metadata, with the IMU on the cameras that
    // record one) or 'dbgi' (debug), the handler name starts with "DJI".
    if (fourcc == "djmd" || fourcc == "dbgi" || containsNoCase(handlerName, "dji")) {
        return TrackKind::DjiMetadata;
    }
    // Google's Camera Motion Metadata, which some cameras (Insta360 exports, phones) write; its axes are not checked against
    // footage here, so it is not read (see Gyro.h).
    if (fourcc == "camm") {
        return TrackKind::OtherCamera;
    }
    return TrackKind::NotTelemetry;
}

std::string_view unreadableReason(TrackKind kind) {
    switch (kind) {
    case TrackKind::DjiMetadata:
        return "DJI records its gyro in a metadata track whose layout DJI has not published and that differs between "
               "models, so Wabcut does not read it";
    case TrackKind::OtherCamera:
        return "this camera's telemetry track is not in a format Wabcut reads";
    case TrackKind::GoProGpmf:
    case TrackKind::SonyRtmd:
    case TrackKind::NotTelemetry:
        break;
    }
    return {};
}

} // namespace wabcut::gyro
