// Grey planes, pyramids and the parallel loop of the motion estimators (Image.h).

#include <wabcut/motion/Image.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace wabcut::motion {

float Plane::at(int x, int y) const {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return (*this)(x, y);
}

float Plane::sample(float x, float y) const {
    // NaN fails every comparison and would pass std::clamp: send it to the edge instead.
    x = x >= 0.0f ? std::min(x, static_cast<float>(width - 1)) : 0.0f;
    y = y >= 0.0f ? std::min(y, static_cast<float>(height - 1)) : 0.0f;
    const int x0 = std::min(static_cast<int>(x), width - 1);
    const int y0 = std::min(static_cast<int>(y), height - 1);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float* r0 = px.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(width);
    const float* r1 = px.data() + static_cast<std::size_t>(y1) * static_cast<std::size_t>(width);
    const float top = r0[x0] + (r0[x1] - r0[x0]) * fx;
    const float bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
    return top + (bottom - top) * fy;
}

namespace {

/// One pyramid step: [1 4 6 4 1] / 16 in both directions, then every other pixel.
Plane halve(const Plane& in) {
    const int w = (in.width + 1) / 2;
    const int h = (in.height + 1) / 2;
    // Horizontal pass on the kept columns only.
    Plane tmp(w, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < w; ++x) {
            const int c = 2 * x;
            tmp(x, y) = (in.at(c - 2, y) + 4.0f * in.at(c - 1, y) + 6.0f * in.at(c, y) + 4.0f * in.at(c + 1, y) +
                         in.at(c + 2, y)) *
                        (1.0f / 16.0f);
        }
    }
    Plane out(w, h);
    for (int y = 0; y < h; ++y) {
        const int c = 2 * y;
        for (int x = 0; x < w; ++x) {
            out(x, y) = (tmp.at(x, c - 2) + 4.0f * tmp.at(x, c - 1) + 6.0f * tmp.at(x, c) + 4.0f * tmp.at(x, c + 1) +
                         tmp.at(x, c + 2)) *
                        (1.0f / 16.0f);
        }
    }
    return out;
}

} // namespace

std::vector<Plane> pyramid(const Plane& image, int levels) {
    std::vector<Plane> out;
    if (image.empty()) {
        return out;
    }
    out.push_back(image);
    for (int l = 1; l < levels; ++l) {
        const Plane& last = out.back();
        if (last.width < 4 || last.height < 4) {
            break;
        }
        out.push_back(halve(last));
    }
    return out;
}

Plane resize(const Plane& image, int width, int height) {
    if (image.empty() || width <= 0 || height <= 0) {
        return {};
    }
    if (width == image.width && height == image.height) {
        return image;
    }
    Plane out(width, height);
    const double sx = static_cast<double>(image.width) / width;
    const double sy = static_cast<double>(image.height) / height;
    if (sx <= 1.0 && sy <= 1.0) {
        for (int y = 0; y < height; ++y) {
            const auto fy = static_cast<float>((y + 0.5) * sy - 0.5);
            for (int x = 0; x < width; ++x) {
                out(x, y) = image.sample(static_cast<float>((x + 0.5) * sx - 0.5), fy);
            }
        }
        return out;
    }
    // Area average: each output pixel is the mean of the input area it covers (partial pixels weighted).
    for (int y = 0; y < height; ++y) {
        const double y0 = y * sy;
        const double y1 = std::min<double>(image.height, (y + 1) * sy);
        for (int x = 0; x < width; ++x) {
            const double x0 = x * sx;
            const double x1 = std::min<double>(image.width, (x + 1) * sx);
            double sum = 0.0;
            double area = 0.0;
            for (int iy = static_cast<int>(y0); iy < static_cast<int>(std::ceil(y1)); ++iy) {
                const double wy = std::min<double>(iy + 1, y1) - std::max<double>(iy, y0);
                if (wy <= 0.0) {
                    continue;
                }
                for (int ix = static_cast<int>(x0); ix < static_cast<int>(std::ceil(x1)); ++ix) {
                    const double wx = std::min<double>(ix + 1, x1) - std::max<double>(ix, x0);
                    if (wx <= 0.0) {
                        continue;
                    }
                    sum += wx * wy * image(std::min(ix, image.width - 1), std::min(iy, image.height - 1));
                    area += wx * wy;
                }
            }
            out(x, y) = area > 0.0 ? static_cast<float>(sum / area) : 0.0f;
        }
    }
    return out;
}

void gradients(const Plane& image, Plane& gx, Plane& gy) {
    gx = Plane(image.width, image.height);
    gy = Plane(image.width, image.height);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            gx(x, y) = 0.5f * (image.at(x + 1, y) - image.at(x - 1, y));
            gy(x, y) = 0.5f * (image.at(x, y + 1) - image.at(x, y - 1));
        }
    }
}

void analysisSize(int width, int height, int maxSide, int& outWidth, int& outHeight) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    const int longer = std::max(width, height);
    if (maxSide <= 0 || longer <= maxSide) {
        outWidth = width;
        outHeight = height;
        return;
    }
    const double k = static_cast<double>(maxSide) / longer;
    outWidth = std::max(1, static_cast<int>(std::lround(width * k)));
    outHeight = std::max(1, static_cast<int>(std::lround(height * k)));
}

void parallelFor(int count, int threads, const std::function<void(int)>& body) {
    if (count <= 0) {
        return;
    }
    if (threads <= 0) {
        threads = static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
    }
    threads = std::min(threads, count);
    if (threads <= 1) {
        for (int i = 0; i < count; ++i) {
            body(i);
        }
        return;
    }
    // Contiguous blocks, one per thread: what each index computes does not depend on the split.
    const auto run = [&](int t) {
        const int begin = static_cast<int>(static_cast<std::int64_t>(count) * t / threads);
        const int end = static_cast<int>(static_cast<std::int64_t>(count) * (t + 1) / threads);
        for (int i = begin; i < end; ++i) {
            body(i);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads - 1));
    for (int t = 1; t < threads; ++t) {
        pool.emplace_back(run, t);
    }
    run(0);
    for (std::thread& t : pool) {
        t.join();
    }
}

} // namespace wabcut::motion
