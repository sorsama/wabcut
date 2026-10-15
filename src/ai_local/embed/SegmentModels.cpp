#include "SegmentModels.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace wabcut::embed_segment {

namespace embed = ai::search::embed;

namespace {

void log(const std::string& text) {
    std::fprintf(stderr, "wabcut-embed: %s\n", text.c_str());
    std::fflush(stderr);
}

std::string pathText(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

embed_audio::Graph openGraph(const std::filesystem::path& file, bool gpu, int threads, const embed_audio::SessionFactory& factory,
                             std::string& device) {
    if (!std::filesystem::is_regular_file(file)) {
        throw std::invalid_argument("model file missing: " + pathText(file));
    }
    embed_audio::Graph g;
    g.file = file;
    g.session = factory(file, gpu, threads, device);
    Ort::AllocatorWithDefaultOptions allocator;
    for (std::size_t i = 0; i < g.session->GetInputCount(); ++i) {
        g.inputs.emplace_back(g.session->GetInputNameAllocated(i, allocator).get());
    }
    for (std::size_t i = 0; i < g.session->GetOutputCount(); ++i) {
        g.outputs.emplace_back(g.session->GetOutputNameAllocated(i, allocator).get());
    }
    return g;
}

void require(const embed_audio::Graph& g, std::initializer_list<const char*> inputs, std::initializer_list<const char*> outputs) {
    for (const char* name : inputs) {
        if (std::ranges::find(g.inputs, std::string(name)) == g.inputs.end()) {
            throw std::runtime_error(pathText(g.file.filename()) + " has no input '" + name + "' (not SAM 2.1's video tracker export?)");
        }
    }
    for (const char* name : outputs) {
        if (std::ranges::find(g.outputs, std::string(name)) == g.outputs.end()) {
            throw std::runtime_error(pathText(g.file.filename()) + " has no output '" + name + "' (not SAM 2.1's video tracker export?)");
        }
    }
}

Ort::MemoryInfo& cpuMemory() {
    static Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return memory;
}

ONNXTensorElementDataType inputType(const embed_audio::Graph& g, const std::string& name, std::vector<std::int64_t>* shape = nullptr) {
    const auto it = std::ranges::find(g.inputs, name);
    const auto info = g.session->GetInputTypeInfo(static_cast<std::size_t>(it - g.inputs.begin()));
    const auto tensor = info.GetTensorTypeAndShapeInfo();
    if (shape) {
        *shape = tensor.GetShape();
    }
    return tensor.GetElementType();
}

Ort::Value floats(std::vector<float>& data, std::vector<std::int64_t> shape) {
    return Ort::Value::CreateTensor<float>(cpuMemory(), data.data(), data.size(), shape.data(), shape.size());
}

/// The graph's outputs `names` for `inputs` (name, value).
std::vector<Ort::Value> run(embed_audio::Graph& g, std::vector<std::pair<std::string, Ort::Value>>& inputs,
                            std::initializer_list<const char*> names) {
    std::vector<const char*> inNames;
    std::vector<Ort::Value> values;
    for (auto& [name, value] : inputs) {
        inNames.push_back(name.c_str()); // `inputs` keeps the names alive until the run is over
        values.push_back(std::move(value));
    }
    std::vector<const char*> outNames(names);
    const auto started = std::chrono::steady_clock::now();
    auto out = g.session->Run(Ort::RunOptions{nullptr}, inNames.data(), values.data(), values.size(), outNames.data(), outNames.size());
    inputs.clear();
    // WABCUT_EMBED_TIMINGS=1: each graph's time on stderr (the helper's log), for profiling.
    static const bool timings = [] {
        const char* v = std::getenv("WABCUT_EMBED_TIMINGS");
        return v != nullptr && *v == '1';
    }();
    if (timings) {
        log(pathText(g.file.filename()) + " " +
            std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()) + " ms");
    }
    return out;
}

std::vector<float> copied(const Ort::Value& v, std::size_t expected, const char* what) {
    const auto info = v.GetTensorTypeAndShapeInfo();
    const std::size_t n = info.GetElementCount();
    if (n != expected || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error(std::string(what) + " has " + std::to_string(n) + " values, not " + std::to_string(expected));
    }
    const float* data = v.GetTensorData<float>();
    return std::vector<float>(data, data + n);
}

float scalar(const Ort::Value& v) {
    const auto info = v.GetTensorTypeAndShapeInfo();
    if (info.GetElementCount() < 1 || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("a score output is not a float");
    }
    return v.GetTensorData<float>()[0];
}

/// [1, C, H, W] -> [H * W, 1, C] (tokens first, as memory attention takes them).
std::vector<float> tokensFirst(const std::vector<float>& nchw, int channels, int tokens) {
    std::vector<float> out(nchw.size());
    for (int c = 0; c < channels; ++c) {
        for (int t = 0; t < tokens; ++t) {
            out[static_cast<std::size_t>(t) * channels + c] = nchw[static_cast<std::size_t>(c) * tokens + t];
        }
    }
    return out;
}

nlohmann::json readJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::invalid_argument("model file missing: " + pathText(file));
    }
    std::ostringstream text;
    text << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        throw std::runtime_error(pathText(file.filename()) + " is not JSON");
    }
    return j;
}

} // namespace

Sam2Tracker::Sam2Tracker(const std::filesystem::path& directory, const std::string& device, int threads, embed_audio::SessionFactory factory)
    : m_directory(directory), m_threads(threads), m_factory(std::move(factory)) {
    const nlohmann::json constants = readJson(directory / "constants.json");
    if (constants.value("image_size", 0) != kImage || constants.value("feat_size", 0) != kFeat || constants.value("mem_dim", 0) != kMemDim ||
        constants.value("num_maskmem", 0) != kMemories || constants.value("max_object_pointers", 0) != kPointers) {
        throw std::runtime_error("constants.json is not SAM 2.1 tiny's video tracker (1024 input, 7 memories, 16 pointers)");
    }
    for (int c = 0; c < 3; ++c) {
        m_mean[static_cast<std::size_t>(c)] = constants.at("image_mean").at(static_cast<std::size_t>(c)).get<float>();
        m_std[static_cast<std::size_t>(c)] = constants.at("image_std").at(static_cast<std::size_t>(c)).get<float>();
    }
    const auto& table = constants.at("memory_temporal_positional_encoding");
    if (!table.is_array() || table.size() != kMemories) {
        throw std::runtime_error("constants.json: the memory temporal encoding is not 7 x 64");
    }
    for (const auto& row : table) {
        if (!row.is_array() || row.size() != kMemDim) {
            throw std::runtime_error("constants.json: the memory temporal encoding is not 7 x 64");
        }
        for (const auto& v : row) {
            m_tpos.push_back(v.get<float>());
        }
    }
    bool opened = false;
#ifdef _WIN32
    if (device != "cpu") {
        try {
            open(true);
            opened = true;
        } catch (const std::invalid_argument&) {
            throw;
        } catch (const std::exception& e) {
            log(std::string("DirectML could not load the segmentation model (") + e.what() + "); using the CPU");
        }
    }
#else
    (void)device;
#endif
    if (!opened) {
        open(false);
    }
}

void Sam2Tracker::open(bool gpu) {
    std::string device;
    m_encoder = openGraph(m_directory / "vision_encoder.onnx", gpu, m_threads, m_factory, device);
    m_decoder = openGraph(m_directory / "mask_decoder.onnx", gpu, m_threads, m_factory, device);
    m_memoryEncoder = openGraph(m_directory / "memory_encoder.onnx", gpu, m_threads, m_factory, device);
    m_memoryAttention = openGraph(m_directory / "memory_attention.onnx", gpu, m_threads, m_factory, device);
    m_pointerPe = openGraph(m_directory / "pointer_tpos.onnx", gpu, m_threads, m_factory, device);
    m_device = device;
    require(m_encoder, {"pixel_values"}, {"feats0", "feats1", "feats2", "feats2_no_mem", "vision_pos_embed"});
    require(m_decoder, {"feats0", "feats1", "feats2_cond", "input_points", "input_labels"},
            {"high_res_mask", "iou", "object_score_logits", "object_pointer"});
    require(m_memoryEncoder, {"feats2", "high_res_mask", "object_score_logits", "binarize"}, {"memory_tokens", "memory_pos"});
    require(m_memoryAttention, {"current_vision_features", "current_vision_position_embeddings", "memory", "memory_pos"},
            {"conditioned_feats"});
    require(m_pointerPe, {"normalized_diffs"}, {"pointer_pos"});
    log("segmentation model " + pathText(m_directory.filename()) + " on " + m_device);
}

template <class Fn>
Sam2Tracker::Result Sam2Tracker::withFallback(Fn&& fn) {
    try {
        return fn();
    } catch (const std::logic_error&) {
        throw;
    } catch (const std::exception& e) {
        if (m_device == "CPU") {
            throw;
        }
        log(std::string("DirectML failed while segmenting (") + e.what() + "); switching to the CPU");
        open(false);
        return fn();
    }
}

Sam2Tracker::Features Sam2Tracker::encode(const std::vector<std::uint8_t>& rgb) {
    std::uint64_t key = 1469598103934665603ull; // FNV-1a over the pixels
    for (const std::uint8_t b : rgb) {
        key = (key ^ b) * 1099511628211ull;
    }
    key |= 1u;
    if (key == m_encodedKey) {
        return m_encoded;
    }
    const std::size_t plane = static_cast<std::size_t>(kImage) * kImage;
    std::vector<float> pixels(plane * 3);
    for (std::size_t i = 0; i < plane; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            pixels[c * plane + i] = (rgb[i * 3 + c] / 255.0f - m_mean[c]) / m_std[c];
        }
    }
    std::vector<std::pair<std::string, Ort::Value>> in;
    in.emplace_back("pixel_values", floats(pixels, {1, 3, kImage, kImage}));
    auto out = run(m_encoder, in, {"feats0", "feats1", "feats2", "feats2_no_mem", "vision_pos_embed"});
    Features f;
    f.feats0 = copied(out[0], static_cast<std::size_t>(32) * 256 * 256, "feats0");
    f.feats1 = copied(out[1], static_cast<std::size_t>(64) * 128 * 128, "feats1");
    f.feats2 = copied(out[2], static_cast<std::size_t>(kHidden) * kTokens, "feats2");
    f.feats2NoMem = copied(out[3], static_cast<std::size_t>(kHidden) * kTokens, "feats2_no_mem");
    f.pos = copied(out[4], static_cast<std::size_t>(kHidden) * kTokens, "vision_pos_embed");
    m_encoded = f;
    m_encodedKey = key;
    return f;
}

Sam2Tracker::Result Sam2Tracker::decode(const Features& f, const std::vector<float>& feats2Cond,
                                        const std::vector<embed::SegmentPoint>& points, std::vector<float>& pointer) {
    std::vector<float> feats0 = f.feats0, feats1 = f.feats1, cond = feats2Cond;
    const auto n = static_cast<std::int64_t>(std::max<std::size_t>(points.size(), 1));
    std::vector<float> coords;
    std::vector<std::int32_t> labels32;
    std::vector<std::int64_t> labels64;
    if (points.empty()) { // a tracked frame: one padding point
        coords = {0.0f, 0.0f};
        labels32 = {-1};
        labels64 = {-1};
    }
    for (const embed::SegmentPoint& p : points) {
        coords.push_back(p.x);
        coords.push_back(p.y);
        labels32.push_back(p.label);
        labels64.push_back(p.label);
    }
    std::vector<std::pair<std::string, Ort::Value>> in;
    in.emplace_back("feats0", floats(feats0, {1, 32, 256, 256}));
    in.emplace_back("feats1", floats(feats1, {1, 64, 128, 128}));
    in.emplace_back("feats2_cond", floats(cond, {1, kHidden, kFeat, kFeat}));
    in.emplace_back("input_points", floats(coords, {1, 1, n, 2}));
    const std::array<std::int64_t, 3> labelShape{1, 1, n};
    if (inputType(m_decoder, "input_labels") == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
        in.emplace_back("input_labels", Ort::Value::CreateTensor<std::int64_t>(cpuMemory(), labels64.data(), labels64.size(), labelShape.data(), 3));
    } else {
        in.emplace_back("input_labels", Ort::Value::CreateTensor<std::int32_t>(cpuMemory(), labels32.data(), labels32.size(), labelShape.data(), 3));
    }
    auto out = run(m_decoder, in, {"high_res_mask", "iou", "object_score_logits", "object_pointer"});
    Result r;
    r.logits = copied(out[0], static_cast<std::size_t>(kImage) * kImage, "high_res_mask");
    r.iou = scalar(out[1]);
    r.objectScore = scalar(out[2]);
    pointer = copied(out[3], kHidden, "object_pointer");
    return r;
}

Sam2Tracker::Memory Sam2Tracker::remember(const Features& f, const Result& r, bool prompted, int frame) {
    std::vector<float> feats2 = f.feats2, mask = r.logits, score{r.objectScore};
    std::vector<std::pair<std::string, Ort::Value>> in;
    in.emplace_back("feats2", floats(feats2, {1, kHidden, kFeat, kFeat}));
    in.emplace_back("high_res_mask", floats(mask, {1, 1, kImage, kImage}));
    in.emplace_back("object_score_logits", floats(score, {1, 1}));
    std::vector<std::int64_t> shape;
    const ONNXTensorElementDataType type = inputType(m_memoryEncoder, "binarize", &shape);
    float binarizeF = prompted ? 1.0f : 0.0f;
    std::int64_t binarizeI = prompted ? 1 : 0;
    bool binarizeB = prompted;
    std::vector<std::int64_t> dims(shape.size(), 1);
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL) {
        in.emplace_back("binarize", Ort::Value::CreateTensor<bool>(cpuMemory(), &binarizeB, 1, dims.data(), dims.size()));
    } else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
        in.emplace_back("binarize", Ort::Value::CreateTensor<std::int64_t>(cpuMemory(), &binarizeI, 1, dims.data(), dims.size()));
    } else {
        in.emplace_back("binarize", Ort::Value::CreateTensor<float>(cpuMemory(), &binarizeF, 1, dims.data(), dims.size()));
    }
    auto out = run(m_memoryEncoder, in, {"memory_tokens", "memory_pos"});
    Memory m;
    m.frame = frame;
    m.tokens = copied(out[0], static_cast<std::size_t>(kTokens) * kMemDim, "memory_tokens");
    m.pos = copied(out[1], static_cast<std::size_t>(kTokens) * kMemDim, "memory_pos");
    return m;
}

std::vector<float> Sam2Tracker::attend(const Features& f, int frame, int frames) {
    // The memory bank: the prompted frame (temporal row 6), then the recent frames k back (row k - 1), padded to 7
    // blocks with the most recent; then 16 object pointers of 4 tokens each (the prompted frame's first), padded the
    // same way.
    struct Block {
        const Memory* memory;
        int row;
    };
    std::vector<Block> blocks{{&m_condition, kMemories - 1}};
    for (const Memory& m : m_recent) {
        const int back = std::abs(frame - m.frame);
        blocks.push_back({&m, std::clamp(back, 1, kMemories - 1) - 1});
    }
    const Block newest = blocks.size() > 1 ? blocks[1] : blocks[0];
    while (static_cast<int>(blocks.size()) < kMemories) {
        blocks.push_back(newest);
    }
    struct Ptr {
        const Pointer* pointer;
        float diff;
    };
    std::vector<Ptr> pointers{{&m_conditionPointer, static_cast<float>(std::abs(frame - m_conditionPointer.frame))}};
    for (const Pointer& p : m_recentPointers) {
        pointers.push_back({&p, static_cast<float>(std::abs(frame - p.frame))});
    }
    const Ptr newestPtr = pointers.size() > 1 ? pointers[1] : pointers[0];
    while (static_cast<int>(pointers.size()) < kPointers) {
        pointers.push_back(newestPtr);
    }
    std::vector<float> diffs;
    const float scale = static_cast<float>(std::max(1, std::min(frames, kPointers) - 1));
    for (const Ptr& p : pointers) {
        diffs.push_back(p.diff / scale);
    }
    std::vector<std::pair<std::string, Ort::Value>> in;
    in.emplace_back("normalized_diffs", floats(diffs, {kPointers}));
    auto pe = run(m_pointerPe, in, {"pointer_pos"});
    const std::vector<float> pointerPos = copied(pe[0], static_cast<std::size_t>(kPointers) * kMemDim, "pointer_pos");

    const std::size_t blockSize = static_cast<std::size_t>(kTokens) * kMemDim;
    const std::size_t total = blockSize * kMemories + static_cast<std::size_t>(kPointers) * 4 * kMemDim;
    std::vector<float> memory(total), memoryPos(total);
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const Memory& m = *blocks[b].memory;
        const float* row = &m_tpos[static_cast<std::size_t>(blocks[b].row) * kMemDim];
        std::copy(m.tokens.begin(), m.tokens.end(), memory.begin() + static_cast<std::ptrdiff_t>(b * blockSize));
        for (std::size_t t = 0; t < static_cast<std::size_t>(kTokens); ++t) {
            for (std::size_t d = 0; d < static_cast<std::size_t>(kMemDim); ++d) {
                memoryPos[b * blockSize + t * kMemDim + d] = m.pos[t * kMemDim + d] + row[d];
            }
        }
    }
    std::size_t o = blockSize * kMemories;
    for (std::size_t p = 0; p < pointers.size(); ++p) {
        const std::vector<float>& v = pointers[p].pointer->value;
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t d = 0; d < static_cast<std::size_t>(kMemDim); ++d) {
                memory[o + d] = v[c * kMemDim + d];
                memoryPos[o + d] = pointerPos[p * kMemDim + d];
            }
            o += kMemDim;
        }
    }
    std::vector<float> current = tokensFirst(f.feats2, kHidden, kTokens);
    std::vector<float> currentPos = tokensFirst(f.pos, kHidden, kTokens);
    const auto rows = static_cast<std::int64_t>(total / kMemDim);
    in.emplace_back("current_vision_features", floats(current, {kTokens, 1, kHidden}));
    in.emplace_back("current_vision_position_embeddings", floats(currentPos, {kTokens, 1, kHidden}));
    in.emplace_back("memory", floats(memory, {rows, 1, kMemDim}));
    in.emplace_back("memory_pos", floats(memoryPos, {rows, 1, kMemDim}));
    auto out = run(m_memoryAttention, in, {"conditioned_feats"});
    return copied(out[0], static_cast<std::size_t>(kHidden) * kTokens, "conditioned_feats");
}

Sam2Tracker::Result Sam2Tracker::prompt(const std::vector<std::uint8_t>& rgb, const std::vector<embed::SegmentPoint>& points, int frame,
                                        int frames) {
    (void)frames;
    return withFallback([&] {
        const Features f = encode(rgb);
        std::vector<float> pointer;
        Result r = decode(f, f.feats2NoMem, points, pointer);
        m_condition = remember(f, r, true, frame);
        m_conditionPointer = Pointer{frame, std::move(pointer)};
        m_recent.clear();
        m_recentPointers.clear();
        m_prompted = true;
        return r;
    });
}

Sam2Tracker::Result Sam2Tracker::track(const std::vector<std::uint8_t>& rgb, int frame, int frames) {
    if (!m_prompted) {
        throw std::logic_error("track before a prompt");
    }
    return withFallback([&] {
        const Features f = encode(rgb);
        const std::vector<float> conditioned = attend(f, frame, frames);
        std::vector<float> pointer;
        Result r = decode(f, conditioned, {}, pointer);
        m_recent.push_front(remember(f, r, false, frame));
        while (static_cast<int>(m_recent.size()) > kMemories - 1) {
            m_recent.pop_back();
        }
        m_recentPointers.push_front(Pointer{frame, std::move(pointer)});
        while (static_cast<int>(m_recentPointers.size()) > kPointers - 1) {
            m_recentPointers.pop_back();
        }
        return r;
    });
}

embed::Response segment(const embed::SegmentFrame& request, Sam2Tracker& model) {
    const auto started = std::chrono::steady_clock::now();
    auto pictures = embed::readImageBatch(request.images);
    if (!pictures) {
        return embed::Failure{request.id, "io", pictures.error().message};
    }
    const ai::search::Image& picture = pictures->front();
    if (picture.width != Sam2Tracker::kImage || picture.height != Sam2Tracker::kImage) {
        return embed::Failure{request.id, "bad_request", "the segmentation model takes 1024 x 1024 pictures"};
    }
    Sam2Tracker::Result r;
    try {
        r = request.mode == "track" ? model.track(picture.rgb, request.frame, request.frames)
                                    : model.prompt(picture.rgb, request.points, request.frame, request.frames);
    } catch (const std::logic_error& e) {
        return embed::Failure{request.id, "bad_request", e.what()};
    } catch (const std::exception& e) {
        return embed::Failure{request.id, "inference", std::string("segmentation failed: ") + e.what()};
    }
    // The matte: the 1024 x 1024 logits (the frame stretched) sampled at the requested size, through the sigmoid.
    const int w = request.width, h = request.height, n = Sam2Tracker::kImage;
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const double sy = std::clamp((y + 0.5) * n / h - 0.5, 0.0, n - 1.0);
        const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, n - 1);
        const double ty = sy - y0;
        for (int x = 0; x < w; ++x) {
            const double sx = std::clamp((x + 0.5) * n / w - 0.5, 0.0, n - 1.0);
            const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, n - 1);
            const double tx = sx - x0;
            const auto at = [&](int xx, int yy) { return static_cast<double>(r.logits[static_cast<std::size_t>(yy) * n + xx]); };
            const double logit = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
            const double a = 1.0 / (1.0 + std::exp(-logit));
            alpha[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0));
        }
    }
    {
        std::ofstream out(request.output, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(alpha.data()), static_cast<std::streamsize>(alpha.size()));
        if (!out) {
            return embed::Failure{request.id, "io", "cannot write the matte"};
        }
    }
    embed::Segmented reply;
    reply.id = request.id;
    reply.iou = r.iou;
    reply.objectScore = r.objectScore;
    reply.present = r.objectScore > 0.0f;
    reply.width = w;
    reply.height = h;
    reply.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    reply.device = model.device();
    return reply;
}

} // namespace wabcut::embed_segment
