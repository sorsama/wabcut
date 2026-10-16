// A scripted stand-in for wabcut-embed (EmbedProtocol.h), so HelperEmbedder's process handling is tested without ONNX
// Runtime or a model. Vectors are made from what the inputs plainly are: a picture's mean red, green and blue; a
// text's colour words ("red" -> the red axis). Detections (AI-9) are the bounding boxes of plain colour blocks: red
// (255, 0, 0) is a face, green (0, 255, 0) a person and blue (0, 0, 255) a car (COCO class 2). Misbehaviour on request
// (the embed modes apply to detect too):
//
//   --dim N            vectors of N dimensions (default 8)
//   --mode M           normal | crash-at-start | crash-on-embed | hang-on-embed | garbage | wrong-dim | wrong-count |
//                      wrong-protocol | fail-load | crash-once (crashes on the first embed of a process whose
//                      --marker file does not exist yet, creating it) | wrong-row (a face row of 3 numbers)
//   --marker PATH      for crash-once
//   --starts PATH      appends a line per start (tests count restarts)
//   --exits PATH       appends a line when it shuts down cleanly (a shutdown request or the end of its input)
//
// A.4 audio requests (the embed modes apply to them too, and wrong-count answers one frame short): enhance_audio is a
// one-pole low-pass per channel, y[n] = y[n-1] + 0.002 (x[n] - y[n-1]) from y = 0 at the block's start (a stand-in
// with memory, like DeepFilterNet's: chunk seams need pre-roll); separate_audio splits the mix into four stems of
// 0.1, 0.2, 0.3 and 0.4 times it (they sum to the mix).
//
// AI-12 translate_text answers each text as "<target token> <text>" (the text alone without one), a token per word;
// --mode crash-gpu-translation crashes on a translation load for any device but "cpu".
//
// AI-14 segment_frame: the object is the red pixels (r > 150, g and b < 100) of the picture; a prompt needs a positive
// point or a box, and an answer to "track" needs a prompt first. The matte is 255 on red, 0 elsewhere, sampled at the
// requested size; present while any red is in the picture (objectScore +5 / -5), iou 0.9. --mode crash-on-track
// crashes on the third tracked frame of a process, crash-gpu-segmentation on a segmentation load for any device but
// "cpu".

#include <wabcut/ai/search/EmbedProtocol.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <wabcut/platform/DllSearch.h>

using namespace wabcut::ai::search;

namespace {

void send(const embed::Response& response) {
    std::cout << embed::encode(response) << '\n' << std::flush;
}

Vector textVector(const std::string& text, int dim) {
    Vector v(static_cast<std::size_t>(dim), 0.0f);
    const auto has = [&](const char* word) { return text.find(word) != std::string::npos; };
    v[0] = has("red") ? 1.0f : 0.0f;
    v[1] = has("green") ? 1.0f : 0.0f;
    v[2] = has("blue") ? 1.0f : 0.0f;
    if (v[0] + v[1] + v[2] == 0.0f) {
        v[3 % static_cast<std::size_t>(dim)] = 1.0f;
    }
    return v;
}

Vector imageVector(const Image& image, int dim) {
    Vector v(static_cast<std::size_t>(dim), 0.0f);
    double sum[3] = {0, 0, 0};
    for (std::size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
        for (int c = 0; c < 3; ++c) {
            sum[c] += image.rgb[i + static_cast<std::size_t>(c)];
        }
    }
    const double pixels = std::max<double>(1.0, static_cast<double>(image.rgb.size() / 3));
    for (int c = 0; c < 3; ++c) {
        v[static_cast<std::size_t>(c)] = static_cast<float>(sum[c] / pixels / 255.0);
    }
    return v;
}

/// The bounding box of the pixels of one plain colour (x, y, w, h), or nothing.
std::optional<std::array<float, 4>> colourBox(const Image& image, int r, int g, int b) {
    int x0 = image.width;
    int y0 = image.height;
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 3;
            const auto near = [](int value, int target) { return std::abs(value - target) < 48; };
            if (near(image.rgb[at], r) && near(image.rgb[at + 1], g) && near(image.rgb[at + 2], b)) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0) {
        return std::nullopt;
    }
    return std::array<float, 4>{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0 + 1),
                                static_cast<float>(y1 - y0 + 1)};
}

embed::DetectedFrame detectFrame(const Image& image, const embed::Detect& request, bool wrongRow) {
    embed::DetectedFrame frame;
    if (request.faces) {
        if (const auto box = colourBox(image, 255, 0, 0)) {
            const auto [x, y, w, h] = *box;
            std::vector<float> row{x, y, w, h, 0.9f};
            // eyes, nose, mouth corners
            for (const auto& [fx, fy] : {std::pair{0.3f, 0.35f}, {0.7f, 0.35f}, {0.5f, 0.55f}, {0.35f, 0.75f}, {0.65f, 0.75f}}) {
                row.push_back(x + fx * w);
                row.push_back(y + fy * h);
            }
            if (wrongRow) {
                row.resize(3);
            }
            frame.faces.push_back(std::move(row));
        }
    }
    if (request.objects) {
        if (const auto box = colourBox(image, 0, 255, 0)) {
            frame.objects.push_back({(*box)[0], (*box)[1], (*box)[2], (*box)[3], 0.8f, 0.0f});
        }
        if (const auto box = colourBox(image, 0, 0, 255)) {
            frame.objects.push_back({(*box)[0], (*box)[1], (*box)[2], (*box)[3], 0.7f, 2.0f});
        }
    }
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    int dim = 8;
    std::string mode = "normal";
    std::filesystem::path marker;
    std::filesystem::path exits;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key == "--dim") {
            dim = std::atoi(argv[i + 1]);
        } else if (key == "--mode") {
            mode = argv[i + 1];
        } else if (key == "--marker") {
            marker = argv[i + 1];
        } else if (key == "--starts") {
            std::ofstream(argv[i + 1], std::ios::app) << "start\n";
        } else if (key == "--exits") {
            exits = argv[i + 1];
        }
    }
    if (mode == "crash-at-start") {
        std::_Exit(57); // like a crash: no cleanup, no answer
    }
    bool prompted = false; // AI-14: a segmentation prompt came
    int tracked = 0;
    const auto exited = [&] {
        if (!exits.empty()) {
            std::ofstream(exits, std::ios::app) << "exit\n";
        }
        return 0;
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        auto request = embed::decodeRequest(line);
        if (!request) {
            send(embed::Failure{"", "protocol", request.error().message});
            continue;
        }
        if (std::holds_alternative<embed::Shutdown>(*request)) {
            return exited();
        }
        if (std::holds_alternative<embed::Hello>(*request)) {
            send(embed::HelloReply{mode == "wrong-protocol" ? 99 : embed::kProtocolVersion, "fake", "1", {"FakeExecutionProvider"}});
            continue;
        }
        if (const auto* load = std::get_if<embed::Load>(&*request)) {
            if (mode == "crash-gpu-translation" && !load->translation.empty() && load->device != "cpu") {
                std::_Exit(57); // AI-12: DirectML taking the helper down while it creates the session
            }
            if (mode == "crash-gpu-segmentation" && !load->segmentation.empty() && load->device != "cpu") {
                std::_Exit(57); // AI-14: the same for the segmentation model
            }
            if (mode == "fail-load") {
                send(embed::Failure{load->id, "model", "no such model file"});
            } else {
                send(embed::Loaded{load->id, "FakeCPU", dim});
            }
            continue;
        }
        const bool embedding = std::holds_alternative<embed::EmbedImages>(*request) || std::holds_alternative<embed::EmbedText>(*request) ||
                               std::holds_alternative<embed::Detect>(*request) || std::holds_alternative<embed::EnhanceAudio>(*request) ||
                               std::holds_alternative<embed::SeparateAudio>(*request) || std::holds_alternative<embed::TranslateText>(*request) ||
                               std::holds_alternative<embed::SegmentFrame>(*request);
        if (embedding && mode == "crash-on-embed") {
            std::_Exit(57); // like a crash: no cleanup, no answer
        }
        if (embedding && mode == "crash-once" && !marker.empty() && !std::filesystem::exists(marker)) {
            std::ofstream(marker) << "crashed\n";
            std::_Exit(57); // like a crash: no cleanup, no answer
        }
        if (embedding && mode == "hang-on-embed") {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
        if (embedding && mode == "garbage") {
            std::cout << "{this is not json\n" << std::flush;
            continue;
        }
        if (const auto* enhance = std::get_if<embed::EnhanceAudio>(&*request)) {
            auto samples = embed::readAudioBlock(enhance->audio);
            if (!samples) {
                send(embed::Failure{enhance->id, "io", samples.error().message});
                continue;
            }
            const auto ch = static_cast<std::size_t>(enhance->audio.channels);
            std::vector<float> state(ch, 0.0f);
            for (std::size_t i = 0; i < samples->size(); ++i) {
                float& y = state[i % ch];
                y += 0.002f * ((*samples)[i] - y);
                (*samples)[i] = y;
            }
            std::ofstream(enhance->output, std::ios::binary)
                .write(reinterpret_cast<const char*>(samples->data()), static_cast<std::streamsize>(samples->size() * sizeof(float)));
            send(embed::AudioDone{enhance->id, enhance->audio.frames - (mode == "wrong-count" ? 1 : 0), enhance->audio.channels, 1, 3.0,
                                  "FakeCPU"});
            continue;
        }
        if (const auto* separate = std::get_if<embed::SeparateAudio>(&*request)) {
            auto samples = embed::readAudioBlock(separate->audio);
            if (!samples) {
                send(embed::Failure{separate->id, "io", samples.error().message});
                continue;
            }
            std::vector<float> stems;
            for (const float w : {0.1f, 0.2f, 0.3f, 0.4f}) {
                for (const float s : *samples) {
                    stems.push_back(s * w);
                }
            }
            std::ofstream(separate->output, std::ios::binary)
                .write(reinterpret_cast<const char*>(stems.data()), static_cast<std::streamsize>(stems.size() * sizeof(float)));
            send(embed::AudioDone{separate->id, separate->audio.frames, 2, mode == "wrong-count" ? 3 : 4, 5.0, "FakeCPU"});
            continue;
        }
        if (const auto* seg = std::get_if<embed::SegmentFrame>(&*request)) {
            auto pictures = embed::readImageBatch(seg->images);
            if (!pictures) {
                send(embed::Failure{seg->id, "io", pictures.error().message});
                continue;
            }
            if (seg->mode == "track" && !prompted) {
                send(embed::Failure{seg->id, "bad_request", "track before a prompt"});
                continue;
            }
            if (seg->mode == "track" && mode == "crash-on-track" && ++tracked >= 3) {
                std::_Exit(57); // like a crash mid-propagation
            }
            prompted = prompted || seg->mode == "prompt";
            const Image& picture = pictures->front();
            const auto red = [&](int x, int y) {
                const std::uint8_t* p = &picture.rgb[(static_cast<std::size_t>(y) * picture.width + x) * 3];
                return p[0] > 150 && p[1] < 100 && p[2] < 100;
            };
            std::vector<std::uint8_t> alpha(static_cast<std::size_t>(seg->width) * seg->height, 0);
            bool any = false;
            for (int y = 0; y < seg->height; ++y) {
                for (int x = 0; x < seg->width; ++x) {
                    const int px = std::min(picture.width - 1, static_cast<int>((x + 0.5) * picture.width / seg->width));
                    const int py = std::min(picture.height - 1, static_cast<int>((y + 0.5) * picture.height / seg->height));
                    if (red(px, py)) {
                        alpha[static_cast<std::size_t>(y) * seg->width + x] = 255;
                        any = true;
                    }
                }
            }
            std::ofstream(seg->output, std::ios::binary).write(reinterpret_cast<const char*>(alpha.data()), static_cast<std::streamsize>(alpha.size()));
            embed::Segmented reply;
            reply.id = seg->id;
            reply.iou = 0.9f;
            reply.objectScore = any ? 5.0f : -5.0f;
            reply.present = any;
            reply.width = seg->width;
            reply.height = seg->height;
            reply.milliseconds = 4.0;
            reply.device = "FakeCPU";
            send(reply);
            continue;
        }
        if (const auto* detect = std::get_if<embed::Detect>(&*request)) {
            auto pictures = embed::readImageBatch(detect->images);
            if (!pictures) {
                send(embed::Failure{detect->id, "io", pictures.error().message});
                continue;
            }
            embed::Detections reply{detect->id, {}, 2.0};
            for (const Image& picture : *pictures) {
                reply.frames.push_back(detectFrame(picture, *detect, mode == "wrong-row"));
            }
            if (mode == "wrong-count") {
                reply.frames.pop_back();
            }
            send(reply);
            continue;
        }
        const int outDim = mode == "wrong-dim" ? dim + 1 : dim;
        if (const auto* images = std::get_if<embed::EmbedImages>(&*request)) {
            auto pictures = embed::readImageBatch(images->images);
            if (!pictures) {
                send(embed::Failure{images->id, "io", pictures.error().message});
                continue;
            }
            embed::Embeddings reply{images->id, {}, 1.0};
            for (const Image& picture : *pictures) {
                reply.vectors.push_back(imageVector(picture, outDim));
            }
            if (mode == "wrong-count") {
                reply.vectors.pop_back();
            }
            send(reply);
        } else if (const auto* translate = std::get_if<embed::TranslateText>(&*request)) {
            embed::Translations reply;
            reply.id = translate->id;
            reply.device = "FakeCPU";
            reply.milliseconds = 1.0;
            for (const std::string& text : translate->texts) {
                reply.texts.push_back(translate->target.empty() ? text : translate->target + " " + text);
                reply.tokens += 1 + static_cast<int>(std::count(text.begin(), text.end(), ' '));
            }
            send(reply);
        } else if (const auto* texts = std::get_if<embed::EmbedText>(&*request)) {
            embed::Embeddings reply{texts->id, {}, 1.0};
            for (const std::string& text : texts->texts) {
                reply.vectors.push_back(textVector(text, outDim));
            }
            send(reply);
        }
    }
    return exited();
}
