// A scripted stand-in for wabcut-infer, speaking the real protocol (wabcut/ai/InferProtocol.h), for the
// TranscriptionService tests. No model, no whisper:
//   * vad: speech = 10 ms frames with |sample| > 0.01, gaps under 100 ms closed;
//   * transcribe: for every speech run, one segment with one word per second it touches, named after the
//     ABSOLUTE second of the PCM file ("w12" for the audio at 12..13 s), timed exactly to the run's part of that
//     second, so tests can check the source-time mapping sample for sample.
// Options:
//   --mode ok | crash-on-transcribe | hang-on-transcribe | garbage | slow | exit-at-start | wrong-protocol
//          | flood-line (20 MiB without a newline) | flood-lines (80 MiB of short lines), then wait for end of input
//          | print-environment ("env NAME=value" per variable, then "cwd <path>", then exit)
//   --thai          words are Thai syllables whose bytes are split across tokens mid-character
//   --pid-file <f>  write the process id to <f> at start
//   --runs-file <f> append a line to <f> at every start

#include <wabcut/platform/DllSearch.h>
#include <wabcut/ai/InferProtocol.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
extern char** environ;
#endif

namespace infer = wabcut::ai::infer;

namespace {

void send(const infer::Response& r) {
    const std::string line = infer::encode(r) + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

std::vector<float> readPcm(const infer::PcmRange& range) {
    std::vector<float> samples(static_cast<std::size_t>(range.count));
    std::ifstream in(range.path, std::ios::binary);
    in.seekg(range.offset * 4);
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4));
    samples.resize(static_cast<std::size_t>(in.gcount() / 4));
    return samples;
}

/// Speech runs in samples of `pcm` (local indices): 160-sample frames above the threshold, gaps < 100 ms closed.
std::vector<std::pair<std::int64_t, std::int64_t>> speechRuns(const std::vector<float>& pcm) {
    std::vector<std::pair<std::int64_t, std::int64_t>> runs;
    const auto frames = static_cast<std::int64_t>(pcm.size()) / 160;
    for (std::int64_t f = 0; f < frames; ++f) {
        float peak = 0.0f;
        for (std::int64_t i = f * 160; i < (f + 1) * 160; ++i) {
            peak = std::max(peak, std::fabs(pcm[static_cast<std::size_t>(i)]));
        }
        if (peak <= 0.01f) {
            continue;
        }
        if (!runs.empty() && f * 160 - runs.back().second < 1600) {
            runs.back().second = (f + 1) * 160;
        } else {
            runs.emplace_back(f * 160, (f + 1) * 160);
        }
    }
    return runs;
}

const char* kThai[] = {"สวัส", "ดี", "ครับ", "ตัด", "ต่อ", "วิดี", "โอ"};

} // namespace

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string mode = "ok";
    bool thai = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--thai") {
            thai = true;
        } else if (arg == "--pid-file" && i + 1 < argc) {
            std::ofstream(argv[++i]) << getpid() << "\n";
        } else if (arg == "--runs-file" && i + 1 < argc) {
            std::ofstream(argv[++i], std::ios::app) << "run\n";
        }
    }
    if (mode == "exit-at-start") {
        return 7;
    }
    if (mode == "print-environment") {
#ifdef _WIN32
        char** variables = _environ;
#else
        char** variables = environ;
#endif
        for (char** v = variables; v != nullptr && *v != nullptr; ++v) {
            std::cout << "env " << *v << "\n";
        }
        const std::u8string cwd = std::filesystem::current_path().u8string();
        std::cout << "cwd " << std::string(cwd.begin(), cwd.end()) << "\n";
        std::cout.flush();
        return 0;
    }
    if (mode == "flood-line" || mode == "flood-lines") {
        // A helper that floods its output: the parent must cut it off, not buffer it (ChildProcess bounds).
        const std::string chunk = mode == "flood-line" ? std::string(64 * 1024, 'x') : std::string(1023, 'y') + '\n';
        const std::size_t total = mode == "flood-line" ? std::size_t{20} * 1024 * 1024 : std::size_t{80} * 1024 * 1024;
        for (std::size_t written = 0; written < total && std::fwrite(chunk.data(), 1, chunk.size(), stdout) == chunk.size();
             written += chunk.size()) {
        }
        std::fflush(stdout);
        std::string rest;
        while (std::getline(std::cin, rest)) {
        }
        return 0;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto request = infer::decodeRequest(line);
        if (!request) {
            send(infer::Failure{"", "bad_request", request.error().message});
            continue;
        }
        if (std::holds_alternative<infer::Hello>(*request)) {
            send(infer::HelloReply{mode == "wrong-protocol" ? 99 : infer::kProtocolVersion, "fake", "0.0", {{"FakeCPU", "fake", "cpu"}}});
        } else if (const auto* load = std::get_if<infer::LoadModel>(&*request)) {
            std::ifstream probe(load->path, std::ios::binary);
            if (probe) {
                send(infer::Loaded{load->id, "FakeCPU", "fake model"});
            } else {
                send(infer::Failure{load->id, "model", "no such model file"});
            }
        } else if (const auto* vad = std::get_if<infer::LoadVad>(&*request)) {
            send(infer::Loaded{vad->id, "FakeCPU", "fake VAD"});
        } else if (const auto* v = std::get_if<infer::VadRequest>(&*request)) {
            infer::VadResult result{v->id, {}};
            for (const auto& [b, e] : speechRuns(readPcm(v->pcm))) {
                result.segments.emplace_back(b / 160, e / 160);
            }
            send(result);
        } else if (const auto* t = std::get_if<infer::TranscribeRequest>(&*request)) {
            send(infer::Progress{t->id, 0});
            if (mode == "crash-on-transcribe") {
                std::fflush(stdout);
                std::_Exit(57);
            }
            if (mode == "hang-on-transcribe") {
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
            if (mode == "garbage") {
                std::fputs("this is not json\n", stdout);
                std::fflush(stdout);
                continue;
            }
            if (mode == "slow") {
                for (int p = 0;; p = (p + 1) % 100) {
                    send(infer::Progress{t->id, p});
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            const std::vector<float> pcm = readPcm(t->pcm);
            for (const auto& [b, e] : speechRuns(pcm)) {
                infer::Segment s;
                s.id = t->id;
                s.language = thai ? "th" : "en";
                s.segment.t0 = b / 160;
                s.segment.t1 = e / 160;
                for (std::int64_t second = (t->pcm.offset + b) / 16000; second * 16000 < t->pcm.offset + e; ++second) {
                    const std::int64_t w0 = std::max<std::int64_t>(second * 16000 - t->pcm.offset, b);
                    const std::int64_t w1 = std::min<std::int64_t>((second + 1) * 16000 - t->pcm.offset, e);
                    if (thai) {
                        // One syllable per second, its UTF-8 bytes split over two tokens in the middle of a
                        // character (byte-level BPE does this).
                        const std::string syllable = kThai[second % 7];
                        const std::size_t cut = 1;
                        s.segment.tokens.push_back({syllable.substr(0, cut), w0 / 160, (w0 + w1) / 320, 0.9f});
                        s.segment.tokens.push_back({syllable.substr(cut), (w0 + w1) / 320, w1 / 160, 0.7f});
                    } else {
                        s.segment.tokens.push_back({" w" + std::to_string(second), w0 / 160, w1 / 160, 0.8f});
                    }
                }
                send(s);
            }
            send(infer::Progress{t->id, 100});
            send(infer::Result{t->id, thai ? "th" : "en", 1, {{"wallMs", 1.0}}});
        }
    }
    return 0;
}
