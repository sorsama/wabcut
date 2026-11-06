#include <wabcut/ai/video/Gaze.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wabcut::ai::video {

namespace {

constexpr double kHalfCrop = kGazeCropSize / 2.0;

// The landmark subset, in table order (see Gaze.h for the mesh indices' meaning).
constexpr std::array<int, kGazeLandmarkSlots> kIndices{
    // the subject's right eye (the picture's left one): the contour from the outer corner along the lower lid to the inner corner and back
    // along the upper lid
    33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246,
    // the subject's left eye (the picture's right one): from the inner corner along the lower lid to the outer corner and back
    362, 382, 381, 380, 374, 373, 390, 249, 263, 466, 388, 387, 386, 385, 384, 398,
    // the irises: centre and four points of the limbus
    468, 469, 470, 471, 472, 473, 474, 475, 476, 477,
    // the nose tip and the cheeks
    1, 234, 454};

// Table slots.
constexpr int kRightCorners[2] = {0, 8};          // outer (the picture's left), inner
constexpr int kRightUpper[7] = {15, 14, 13, 12, 11, 10, 9};
constexpr int kRightLower[7] = {1, 2, 3, 4, 5, 6, 7};
constexpr int kLeftCorners[2] = {16, 24};         // inner (the picture's left), outer
constexpr int kLeftUpper[7] = {25, 26, 27, 28, 29, 30, 31};
constexpr int kLeftLower[7] = {23, 22, 21, 20, 19, 18, 17};
constexpr int kRightIris = 32;
constexpr int kLeftIris = 37;
constexpr int kNose = 42;
constexpr int kCheekA = 43;
constexpr int kCheekB = 44;

std::uint16_t encode(double fraction) {
    return static_cast<std::uint16_t>(std::lround(std::clamp((fraction + 0.5) / 2.0, 0.0, 1.0) * 65535.0));
}
double decode(std::uint16_t v) { return static_cast<double>(v) / 65535.0 * 2.0 - 0.5; }

double bilinear(const Picture& p, double x, double y, std::size_t channel) {
    const double cx = std::clamp(x, 0.0, p.width - 1.0);
    const double cy = std::clamp(y, 0.0, p.height - 1.0);
    const int x0 = static_cast<int>(std::floor(cx));
    const int y0 = static_cast<int>(std::floor(cy));
    const int x1 = std::min(x0 + 1, p.width - 1);
    const int y1 = std::min(y0 + 1, p.height - 1);
    const double fx = cx - x0;
    const double fy = cy - y0;
    const auto at = [&](int ix, int iy) {
        return static_cast<double>(p.rgb[(static_cast<std::size_t>(iy) * static_cast<std::size_t>(p.width) + static_cast<std::size_t>(ix)) * 3 + channel]);
    };
    const double top = at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx;
    const double bottom = at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx;
    return (top * (1.0 - fy) + bottom * fy) / 65535.0;
}

struct Local {
    double x = 0.0;
    double y = 0.0;
};

/// One eye from the table's landmarks: its corners (the picture's left one first), lid contours and iris.
GazeEye eyeOf(const std::vector<LandmarkPoint>& p, const int corners[2], const int upper[7], const int lower[7], int iris, double width, double height) {
    const LandmarkPoint& a = p[static_cast<std::size_t>(corners[0])];
    const LandmarkPoint& b = p[static_cast<std::size_t>(corners[1])];
    const double cx = 0.5 * (a.x + b.x);
    const double cy = 0.5 * (a.y + b.y);
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double half = 0.5 * std::hypot(dx, dy);
    GazeEye eye;
    if (!(half > 1e-6)) {
        return eye; // invalid: zero width
    }
    const double ax = dx / (2.0 * half);
    const double ay = dy / (2.0 * half);
    const auto local = [&](const LandmarkPoint& q) { return Local{(q.x - cx) * ax + (q.y - cy) * ay, -(q.x - cx) * ay + (q.y - cy) * ax}; };
    // A lid is a parabola 1 - u^2 times its height: the least-squares height over the points away from the corners.
    const auto lidHeight = [&](const int slots[7], double sign) {
        double num = 0.0;
        double den = 0.0;
        for (int i = 0; i < 7; ++i) {
            const Local l = local(p[static_cast<std::size_t>(slots[i])]);
            const double u = l.x / half;
            if (std::abs(u) > 0.9) {
                continue;
            }
            const double k = 1.0 - u * u;
            num += sign * l.y * k;
            den += k * k;
        }
        return den > 1e-9 ? std::max(0.0, num / den) : 0.0;
    };
    eye.cx = cx / width;
    eye.cy = cy / height;
    eye.angle = std::atan2(dy, dx);
    eye.halfWidth = half / width;
    eye.upper = lidHeight(upper, -1.0) / width; // the upper lid lies above the corner line: y is down
    eye.lower = lidHeight(lower, 1.0) / width;
    const Local centre = local(p[static_cast<std::size_t>(iris)]);
    eye.irisX = centre.x / width;
    eye.irisY = centre.y / width;
    double radius = 0.0;
    for (int i = 1; i <= 4; ++i) {
        const Local l = local(p[static_cast<std::size_t>(iris + i)]);
        radius += std::hypot(l.x - centre.x, l.y - centre.y);
    }
    eye.irisRadius = radius / 4.0 / width;
    return eye;
}

} // namespace

std::span<const int> gazeLandmarkIndices() { return kIndices; }

GazeCrop gazeCropFor(const FaceBox& box, std::array<double, 2> leftEye, std::array<double, 2> rightEye) {
    GazeCrop crop;
    crop.side = std::max(96.0, 1.5 * std::max(box.w, box.h));
    crop.cx = box.x + box.w / 2.0;
    crop.cy = box.y + box.h / 2.0;
    crop.angle = std::atan2(rightEye[1] - leftEye[1], rightEye[0] - leftEye[0]);
    return crop;
}

std::vector<float> gazeInput(const Picture& picture, const GazeCrop& crop) {
    if (!picture.valid() || !(crop.side > 0.0)) {
        return {};
    }
    const double k = crop.side / kGazeCropSize;
    const double c = std::cos(crop.angle);
    const double s = std::sin(crop.angle);
    std::vector<float> input(static_cast<std::size_t>(kGazeCropSize) * kGazeCropSize * 3);
    for (int v = 0; v < kGazeCropSize; ++v) {
        for (int u = 0; u < kGazeCropSize; ++u) {
            const double lx = (u - kHalfCrop) * k;
            const double ly = (v - kHalfCrop) * k;
            const double x = crop.cx + c * lx - s * ly;
            const double y = crop.cy + s * lx + c * ly;
            float* out = &input[(static_cast<std::size_t>(v) * kGazeCropSize + static_cast<std::size_t>(u)) * 3];
            for (std::size_t ch = 0; ch < 3; ++ch) {
                out[ch] = static_cast<float>(bilinear(picture, x, y, ch));
            }
        }
    }
    return input;
}

std::vector<LandmarkPoint> gazeLandmarksOf(std::span<const float> output, const GazeCrop& crop) {
    if (output.size() != static_cast<std::size_t>(kGazeLandmarkCount) * 3 || !(crop.side > 0.0)) {
        return {};
    }
    const double k = crop.side / kGazeCropSize;
    const double c = std::cos(crop.angle);
    const double s = std::sin(crop.angle);
    std::vector<LandmarkPoint> points(kGazeLandmarkCount);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double lx = (static_cast<double>(output[3 * i]) - kHalfCrop) * k;
        const double ly = (static_cast<double>(output[3 * i + 1]) - kHalfCrop) * k;
        points[i] = {crop.cx + c * lx - s * ly, crop.cy + s * lx + c * ly, static_cast<double>(output[3 * i + 2]) * k};
    }
    return points;
}

Picture gazeTablePicture(std::span<const GazeLandmarkFace> faces, int workWidth, int workHeight) {
    Picture table;
    table.width = kGazeSlotCount;
    table.height = std::max<int>(1, static_cast<int>(faces.size()));
    table.rgb.assign(static_cast<std::size_t>(table.width) * static_cast<std::size_t>(table.height) * 3, 0);
    const double w = std::max(1, workWidth);
    const double h = std::max(1, workHeight);
    const auto put = [&](int row, int slot, std::uint16_t r, std::uint16_t g, std::uint16_t b) {
        std::uint16_t* px = &table.rgb[(static_cast<std::size_t>(row) * static_cast<std::size_t>(table.width) + static_cast<std::size_t>(slot)) * 3];
        px[0] = r;
        px[1] = g;
        px[2] = b;
    };
    for (int row = 0; row < table.height; ++row) {
        const bool has = static_cast<std::size_t>(row) < faces.size() && faces[static_cast<std::size_t>(row)].points.size() == kGazeLandmarkSlots;
        put(row, 47, has ? 65535 : 0, static_cast<std::uint16_t>(std::clamp(workWidth, 0, 65535)), static_cast<std::uint16_t>(std::clamp(workHeight, 0, 65535)));
        if (!has) {
            continue;
        }
        const GazeLandmarkFace& f = faces[static_cast<std::size_t>(row)];
        for (int i = 0; i < kGazeLandmarkSlots; ++i) {
            const LandmarkPoint& pt = f.points[static_cast<std::size_t>(i)];
            put(row, i, encode(pt.x / w), encode(pt.y / h), encode(pt.z / w));
        }
        put(row, 45, encode(f.box.x / w), encode(f.box.y / h), encode(f.box.w / w));
        put(row, 46, encode(f.box.h / h), static_cast<std::uint16_t>(std::lround(std::clamp(f.presence, 0.0, 1.0) * 65535.0)), 0);
    }
    return table;
}

std::expected<GazeTable, Error> gazeTableFromPicture(const Picture& table) {
    if (!table.valid() || table.width != kGazeSlotCount || table.height < 1 || table.height > 16) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the landmark model returned an unusable table"));
    }
    const auto px = [&](int row, int slot) {
        return &table.rgb[(static_cast<std::size_t>(row) * static_cast<std::size_t>(table.width) + static_cast<std::size_t>(slot)) * 3];
    };
    GazeTable out;
    out.workWidth = px(0, 47)[1];
    out.workHeight = px(0, 47)[2];
    if (out.workWidth < 2 || out.workHeight < 2) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the landmark model returned an unusable table"));
    }
    const double w = out.workWidth;
    const double h = out.workHeight;
    for (int row = 0; row < table.height; ++row) {
        if (px(row, 47)[0] == 0) {
            continue; // no face in this row
        }
        GazeLandmarkFace f;
        f.points.resize(kGazeLandmarkSlots);
        for (int i = 0; i < kGazeLandmarkSlots; ++i) {
            const std::uint16_t* p = px(row, i);
            f.points[static_cast<std::size_t>(i)] = {decode(p[0]) * w, decode(p[1]) * h, decode(p[2]) * w};
        }
        const std::uint16_t* bx = px(row, 45);
        const std::uint16_t* by = px(row, 46);
        f.box = {decode(bx[0]) * w, decode(bx[1]) * h, decode(bx[2]) * w, decode(by[0]) * h};
        f.presence = static_cast<double>(by[1]) / 65535.0;
        out.faces.push_back(std::move(f));
    }
    return out;
}

GazeFace gazeFaceOf(const GazeLandmarkFace& face, int workWidth, int workHeight) {
    GazeFace out;
    if (face.points.size() != kGazeLandmarkSlots || workWidth < 1 || workHeight < 1) {
        return out;
    }
    const double w = workWidth;
    const double h = workHeight;
    out.eyes[0] = eyeOf(face.points, kRightCorners, kRightUpper, kRightLower, kRightIris, w, h);
    out.eyes[1] = eyeOf(face.points, kLeftCorners, kLeftUpper, kLeftLower, kLeftIris, w, h);
    const LandmarkPoint& nose = face.points[kNose];
    const double x0 = std::min(face.points[kCheekA].x, face.points[kCheekB].x);
    const double x1 = std::max(face.points[kCheekA].x, face.points[kCheekB].x);
    out.turn = x1 - x0 > 1e-6 ? std::clamp(std::abs(2.0 * (nose.x - x0) / (x1 - x0) - 1.0), 0.0, 1.0) : 1.0;
    out.score = std::clamp(face.presence, 0.0, 1.0);
    out.size = face.box.w / w;
    return out;
}

std::expected<StoredFrame, Error> gazeStoredFrame(const Picture& table, Ticks pts, Ticks duration) {
    auto parsed = gazeTableFromPicture(table);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    std::vector<GazeFace> faces;
    for (const GazeLandmarkFace& f : parsed->faces) {
        faces.push_back(gazeFaceOf(f, parsed->workWidth, parsed->workHeight));
    }
    std::ranges::stable_sort(faces, [](const GazeFace& a, const GazeFace& b) { return a.size > b.size; });
    if (faces.size() > static_cast<std::size_t>(kGazeMaxFaces)) {
        faces.resize(static_cast<std::size_t>(kGazeMaxFaces));
    }
    std::vector<float> values(1 + static_cast<std::size_t>(kGazeFaceFloats) * kGazeMaxFaces, 0.0f);
    values[0] = static_cast<float>(faces.size());
    for (std::size_t i = 0; i < faces.size(); ++i) {
        float* v = &values[1 + i * kGazeFaceFloats];
        std::size_t k = 0;
        for (const GazeEye& e : faces[i].eyes) {
            for (const double d : {e.cx, e.cy, e.angle, e.halfWidth, e.upper, e.lower, e.irisX, e.irisY, e.irisRadius}) {
                v[k++] = static_cast<float>(d);
            }
        }
        v[k++] = static_cast<float>(faces[i].turn);
        v[k++] = static_cast<float>(faces[i].score);
        v[k++] = static_cast<float>(faces[i].size);
    }
    StoredFrame stored;
    stored.width = parsed->workWidth;
    stored.height = parsed->workHeight;
    stored.pts = pts.value;
    stored.duration = duration.value;
    StoredPlane plane;
    plane.width = static_cast<int>(values.size());
    plane.height = 1;
    plane.bytesPerTexel = 4;
    plane.data.resize(values.size() * sizeof(float));
    std::memcpy(plane.data.data(), values.data(), plane.data.size());
    stored.planes.push_back(std::move(plane));
    if (!stored.valid()) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the landmark model returned an unusable table"));
    }
    return stored;
}

std::vector<GazeFace> gazeFacesOfStored(const StoredFrame& frame) {
    std::vector<GazeFace> faces;
    const std::size_t expected = 1 + static_cast<std::size_t>(kGazeFaceFloats) * kGazeMaxFaces;
    if (frame.planes.size() != 1 || frame.planes[0].bytesPerTexel != 4 || frame.planes[0].height != 1 ||
        static_cast<std::size_t>(frame.planes[0].width) != expected || frame.planes[0].data.size() != expected * sizeof(float)) {
        return faces;
    }
    std::vector<float> values(expected);
    std::memcpy(values.data(), frame.planes[0].data.data(), frame.planes[0].data.size());
    const float count = values[0];
    if (!(count >= 0.0f && count <= static_cast<float>(kGazeMaxFaces))) {
        return faces;
    }
    for (int i = 0; i < static_cast<int>(count); ++i) {
        const float* v = &values[1 + static_cast<std::size_t>(i) * kGazeFaceFloats];
        GazeFace f;
        std::size_t k = 0;
        for (GazeEye& e : f.eyes) {
            e.cx = v[k++];
            e.cy = v[k++];
            e.angle = v[k++];
            e.halfWidth = v[k++];
            e.upper = v[k++];
            e.lower = v[k++];
            e.irisX = v[k++];
            e.irisY = v[k++];
            e.irisRadius = v[k++];
        }
        f.turn = v[k++];
        f.score = v[k++];
        f.size = v[k++];
        faces.push_back(f);
    }
    return faces;
}

} // namespace wabcut::ai::video
