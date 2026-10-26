// Issue #10 (hybrid laptops): which GPU Wabcut previews, decodes and exports on. The ranking and the choice are pure, so
// these feed them adapter lists as DXGI / DXCore would describe a hybrid laptop, an older Windows without the preference
// orders, a desktop with two identical cards; the process state (Settings choice, the preview's fixed choice, the vendor
// order the encoders follow, the decoders' backend order) runs on a fake list too. One check reads this computer's list.

#include <wabcut/media/GpuInfo.h>
#include <wabcut/media/VideoDecoder.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace wabcut::media;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

GpuAdapter adapter(std::string name, std::uint32_t vendorId, std::uint32_t deviceId, int sys, int high, int low,
                   GpuKind kind, std::uint64_t memory) {
    GpuAdapter a;
    a.name = std::move(name);
    a.vendorId = vendorId;
    a.deviceId = deviceId;
    a.vendor = vendorId == 0x10DE ? "NVIDIA" : vendorId == 0x1002 ? "AMD" : vendorId == 0x8086 ? "Intel" : "Microsoft";
    a.systemIndex = sys;
    a.highPerformanceIndex = high;
    a.minimumPowerIndex = low;
    a.kind = kind;
    a.dedicatedMemory = memory;
    a.luid = 0x1000u + static_cast<std::uint64_t>(sys);
    return a;
}

/// The issue's laptop: the Radeon 780M is DXGI adapter 0 (the display is on it), the RTX 5060 Laptop GPU adapter 1, WARP
/// last.
std::vector<GpuAdapter> hybridLaptop() {
    std::vector<GpuAdapter> list{
        adapter("AMD Radeon(TM) 780M Graphics", 0x1002, 0x15BF, 0, 1, 0, GpuKind::Integrated, 512 * 1024 * 1024),
        adapter("NVIDIA GeForce RTX 5060 Laptop GPU", 0x10DE, 0x2D59, 1, 0, 1, GpuKind::Discrete, 8 * kGiB),
        adapter("Microsoft Basic Render Driver", 0x1414, 0x8C, 2, 2, 2, GpuKind::Unknown, 0),
    };
    list[2].software = true;
    assignGpuKeys(list);
    return list;
}

std::vector<std::string> names(const std::vector<GpuAdapter>& list) {
    std::vector<std::string> out;
    for (const GpuAdapter& a : list) {
        out.push_back(a.name);
    }
    return out;
}

const std::string kRadeon = "AMD Radeon(TM) 780M Graphics";
const std::string kRtx = "NVIDIA GeForce RTX 5060 Laptop GPU";

struct ResetGpus {
    ~ResetGpus() {
        setGpuChoice({});
        setGpuAdaptersForTesting(std::nullopt);
    }
};

} // namespace

TEST_CASE("GPU ranking: a hybrid laptop's discrete GPU comes first and software adapters are dropped", "[media][gpu]") {
    const std::vector<GpuAdapter> laptop = hybridLaptop();
    CHECK(names(rankGpuAdapters(laptop, GpuPreference::HighPerformance)) == std::vector<std::string>{kRtx, kRadeon});
    CHECK(names(rankGpuAdapters(laptop, GpuPreference::MinimumPower)) == std::vector<std::string>{kRadeon, kRtx});
    CHECK(names(rankGpuAdapters(laptop, GpuPreference::SystemOrder)) == std::vector<std::string>{kRadeon, kRtx});

    // WARP without its software flag is still recognised by its ids.
    std::vector<GpuAdapter> unflagged = laptop;
    unflagged[2].software = false;
    CHECK(rankGpuAdapters(unflagged, GpuPreference::HighPerformance).size() == 2);
}

TEST_CASE("GPU ranking without the preference orders: discrete before integrated, then video memory", "[media][gpu]") {
    // Windows before 1803 (no IDXGIFactory6): an APU's carve-out can be as large as the discrete card's memory.
    std::vector<GpuAdapter> list{
        adapter("AMD Radeon 780M", 0x1002, 0x15BF, 0, -1, -1, GpuKind::Integrated, 8 * kGiB),
        adapter("NVIDIA GeForce RTX 4060 Laptop GPU", 0x10DE, 0x28A0, 1, -1, -1, GpuKind::Discrete, 8 * kGiB),
    };
    CHECK(rankGpuAdapters(list, GpuPreference::HighPerformance).front().vendor == "NVIDIA");
    CHECK(rankGpuAdapters(list, GpuPreference::MinimumPower).front().vendor == "AMD");
    // No kind either (no DXCore): the larger dedicated memory, then the system order.
    for (GpuAdapter& a : list) {
        a.kind = GpuKind::Unknown;
    }
    list[0].dedicatedMemory = 512 * 1024 * 1024;
    CHECK(rankGpuAdapters(list, GpuPreference::HighPerformance).front().vendor == "NVIDIA");
    CHECK(rankGpuAdapters(list, GpuPreference::MinimumPower).front().vendor == "AMD");
    list[0].dedicatedMemory = list[1].dedicatedMemory;
    CHECK(rankGpuAdapters(list, GpuPreference::HighPerformance).front().vendor == "AMD"); // a tie: adapter 0
    // One adapter's preference index unknown: the whole order falls back to kind and memory.
    list = hybridLaptop();
    list[0].highPerformanceIndex = -1;
    list[0].dedicatedMemory = 16 * kGiB;
    list[0].kind = GpuKind::Unknown;
    list[1].kind = GpuKind::Unknown;
    CHECK(rankGpuAdapters(list, GpuPreference::HighPerformance).front().name == kRadeon);
}

TEST_CASE("GPU keys stay the same across restarts and tell identical cards apart", "[media][gpu]") {
    std::vector<GpuAdapter> list{adapter("NVIDIA GeForce RTX 3060", 0x10DE, 0x2504, 0, 0, 1, GpuKind::Discrete, 12 * kGiB),
                                 adapter("NVIDIA GeForce RTX 3060", 0x10DE, 0x2504, 1, 1, 2, GpuKind::Discrete, 12 * kGiB),
                                 adapter("Intel(R) UHD Graphics 770", 0x8086, 0x4680, 2, 2, 0, GpuKind::Integrated, 128)};
    list[0].subSysId = list[1].subSysId = 0x153610DE;
    list[0].revision = list[1].revision = 0xA1;
    assignGpuKeys(list);
    CHECK(list[0].key == "10DE:2504:153610DE:A1");
    CHECK(list[1].key == "10DE:2504:153610DE:A1#2");
    CHECK(list[2].key == "8086:4680:00000000:00");
    // No PCI ids (Linux, macOS): the vendor.
    GpuAdapter apple;
    apple.vendor = "Apple";
    std::vector<GpuAdapter> mac{apple};
    assignGpuKeys(mac);
    CHECK(mac[0].key == "Apple");
}

TEST_CASE("The Windows Graphics setting for wabcut.exe is read from its registry value", "[media][gpu]") {
    CHECK(parseWindowsGpuSetting("") == WindowsGpuSetting::None);
    CHECK(parseWindowsGpuSetting("GpuPreference=0;") == WindowsGpuSetting::None);
    CHECK(parseWindowsGpuSetting("GpuPreference=1;") == WindowsGpuSetting::PowerSaving);
    CHECK(parseWindowsGpuSetting("SwapEffectUpgradeEnable=1;GpuPreference=2;") == WindowsGpuSetting::HighPerformance);
    CHECK(parseWindowsGpuSetting("AutoHDREnable=1;GpuPreference=2") == WindowsGpuSetting::HighPerformance);
    CHECK(parseWindowsGpuSetting("GpuPreference=2147483649;") == WindowsGpuSetting::Specific);
    CHECK(parseWindowsGpuSetting("GpuPreference=2;SpecificAdapter=4318&9561&0;") == WindowsGpuSetting::Specific);
    CHECK(parseWindowsGpuSetting("GpuPreference=x;") == WindowsGpuSetting::None);
    CHECK(parseWindowsGpuSetting("SwapEffectUpgradeEnable=1;") == WindowsGpuSetting::None);
}

TEST_CASE("GPU choice: Automatic, the Windows setting, Settings, a missing card and the override", "[media][gpu]") {
    const std::vector<GpuAdapter> laptop = hybridLaptop();
    const std::string radeonKey = laptop[0].key;

    GpuSelection s = selectGpu(laptop, {});
    REQUIRE(s.adapter);
    CHECK(s.adapter->name == kRtx);
    CHECK(s.reason == GpuReason::HighPerformance);

    // The user's Windows Graphics setting wins over Automatic.
    s = selectGpu(laptop, {.windows = WindowsGpuSetting::PowerSaving});
    CHECK(s.adapter->name == kRadeon);
    CHECK(s.reason == GpuReason::WindowsPowerSaving);
    s = selectGpu(laptop, {.windows = WindowsGpuSetting::HighPerformance});
    CHECK(s.adapter->name == kRtx);
    CHECK(s.reason == GpuReason::WindowsHighPerformance);
    s = selectGpu(laptop, {.windows = WindowsGpuSetting::Specific}); // Windows lists its pick first
    CHECK(s.adapter->name == kRadeon);
    CHECK(s.reason == GpuReason::WindowsSpecific);

    // A card picked in Settings wins over both; one that is gone falls back to Automatic and says so.
    s = selectGpu(laptop, {.choice = radeonKey, .windows = WindowsGpuSetting::HighPerformance});
    CHECK(s.adapter->name == kRadeon);
    CHECK(s.reason == GpuReason::UserChoice);
    s = selectGpu(laptop, {.choice = "10DE:9999:00000000:00"});
    CHECK(s.adapter->name == kRtx);
    CHECK(s.reason == GpuReason::UserChoiceMissing);
    CHECK(s.missingChoice == "10DE:9999:00000000:00");

    // QT_D3D_ADAPTER_INDEX before start (a system index) wins over everything; the software adapter's index does not.
    s = selectGpu(laptop, {.choice = laptop[1].key, .overrideIndex = 0});
    CHECK(s.adapter->name == kRadeon);
    CHECK(s.reason == GpuReason::Override);
    s = selectGpu(laptop, {.overrideIndex = 2});
    CHECK(s.adapter->name == kRtx);
    CHECK(s.reason == GpuReason::HighPerformance);

    // One GPU, or none.
    s = selectGpu({laptop[1], laptop[2]}, {.windows = WindowsGpuSetting::PowerSaving});
    CHECK(s.adapter->name == kRtx);
    CHECK(s.reason == GpuReason::OnlyGpu);
    s = selectGpu({laptop[2]}, {});
    CHECK_FALSE(s.adapter);
    CHECK(s.reason == GpuReason::NoGpu);

    for (const GpuReason r : {GpuReason::NoGpu, GpuReason::OnlyGpu, GpuReason::HighPerformance, GpuReason::WindowsPowerSaving,
                              GpuReason::WindowsHighPerformance, GpuReason::WindowsSpecific, GpuReason::UserChoice,
                              GpuReason::UserChoiceMissing, GpuReason::Override}) {
        CHECK(gpuReasonFromKey(gpuReasonKey(r)) == r);
    }
    CHECK_FALSE(gpuReasonFromKey("nonsense"));
}

TEST_CASE("GPU state: encoders follow the choice at once, the preview keeps its GPU for the run", "[media][gpu]") {
    ResetGpus reset;
    setGpuChoice({});
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop()});

    const std::vector<GpuAdapter> listed = gpuAdapters();
    CHECK(names(listed) == std::vector<std::string>{kRtx, kRadeon}); // ranked, WARP gone
    CHECK(gpuVendors() == std::vector<std::string>{"NVIDIA", "AMD"});
    const GpuSelection preview = previewGpu();
    REQUIRE(preview.adapter);
    CHECK(preview.adapter->name == kRtx);
    CHECK(previewAdapterIndex() == 1);
    CHECK(previewAdapterLuid() == preview.adapter->luid);
#if defined(_WIN32)
    CHECK(hardwareBackendOrder().front() == "cuda"); // decoding on the preview's NVIDIA GPU
#endif

    // Integrated GPU picked in Settings: the encoder order flips now; preview and decoding wait for the next start.
    const std::string radeonKey = std::ranges::find(listed, kRadeon, &GpuAdapter::name)->key;
    setGpuChoice(radeonKey);
    CHECK(gpuChoice() == radeonKey);
    CHECK(selectedGpu().adapter->name == kRadeon);
    CHECK(gpuVendors() == std::vector<std::string>{"AMD", "NVIDIA"});
    CHECK(previewGpu().adapter->name == kRtx);
#if defined(_WIN32)
    CHECK(hardwareBackendOrder().front() == "cuda");
#endif

    // The next start (a fresh preview decision) uses it everywhere.
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop()});
    CHECK(previewGpu().adapter->name == kRadeon);
    CHECK(previewGpu().reason == GpuReason::UserChoice);
    CHECK(previewAdapterIndex() == 0);
#if defined(_WIN32)
    // Fix round 1 (#10): a chosen GPU decodes on itself or in software - CUDA would wake the RTX the user turned away
    // from.
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"d3d11va"});
#endif

    // Windows says Power saving and the choice is Automatic again: the integrated GPU, because the user said so there.
    setGpuChoice({});
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop(), WindowsGpuSetting::PowerSaving});
    CHECK(windowsGpuSetting() == WindowsGpuSetting::PowerSaving);
    CHECK(previewGpu().reason == GpuReason::WindowsPowerSaving);
    CHECK(gpuVendors().front() == "AMD");
#if defined(_WIN32)
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"d3d11va"}); // the user's battery choice holds for decoding
#endif
    // A developer's QT_D3D_ADAPTER_INDEX=1 before start: the RTX.
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop(), WindowsGpuSetting::PowerSaving, 1});
    CHECK(previewGpu().adapter->name == kRtx);
    CHECK(previewGpu().reason == GpuReason::Override);
    // Fix round 1 (#9): the override holds for the export encoders and the Settings row too, whatever Settings says.
    CHECK(selectedGpu().adapter->name == kRtx);
    CHECK(selectedGpu().reason == GpuReason::Override);
    CHECK(gpuVendors().front() == "NVIDIA");
    setGpuChoice(std::ranges::find(listed, kRadeon, &GpuAdapter::name)->key);
    CHECK(selectedGpu().adapter->name == kRtx);
    CHECK(selectedGpu() == previewGpu());
#if defined(_WIN32)
    // Fix round 2: an override pins decoding to its GPU like a choice (no Quick Sync on another GPU).
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"cuda", "d3d11va"});
#endif
}

TEST_CASE("Automatic decoding keeps every hardware backend; a chosen NVIDIA or Intel GPU keeps its own", "[media][gpu]") {
#if !defined(_WIN32)
    SKIP("the Windows decode backends");
#else
    ResetGpus reset;
    setGpuChoice({});
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop()});
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"cuda", "d3d11va", "qsv"});
    // The RTX chosen by hand: CUDA and d3d11va on it, never Quick Sync on another GPU.
    const std::vector<GpuAdapter> listed = gpuAdapters();
    setGpuChoice(std::ranges::find(listed, kRtx, &GpuAdapter::name)->key);
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop()});
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"cuda", "d3d11va"});
    // An Intel laptop GPU chosen: d3d11va and Quick Sync on it.
    std::vector<GpuAdapter> intelLaptop = hybridLaptop();
    intelLaptop[0].name = "Intel(R) Arc(TM) 140V GPU";
    intelLaptop[0].vendor = "Intel";
    intelLaptop[0].vendorId = 0x8086;
    intelLaptop[0].deviceId = 0x64A0;
    setGpuAdaptersForTesting(GpuTestSetup{intelLaptop});
    const std::vector<GpuAdapter> intelListed = gpuAdapters();
    setGpuChoice(std::ranges::find(intelListed, std::string("Intel(R) Arc(TM) 140V GPU"), &GpuAdapter::name)->key);
    setGpuAdaptersForTesting(GpuTestSetup{intelLaptop});
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"d3d11va", "qsv"});
    // The Windows setting naming a GPU counts as a choice too.
    setGpuChoice({});
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop(), WindowsGpuSetting::Specific});
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"d3d11va"});
    // So does a QT_D3D_ADAPTER_INDEX set before start (fix round 2): the Radeon forced, CUDA would wake the RTX.
    setGpuAdaptersForTesting(GpuTestSetup{hybridLaptop(), WindowsGpuSetting::None, 0});
    CHECK(previewGpu().reason == GpuReason::Override);
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"d3d11va"});
#endif
}

TEST_CASE("Linux GPUs from sysfs: discrete ranks first on a hybrid laptop", "[media][gpu]") {
    constexpr std::uint64_t gib = 1024ull * 1024 * 1024;
    const GpuAdapter intel = describePciGpu(0x8086, 0xA7A0, "0000:00:02.0", 0, true);
    const GpuAdapter nvidia = describePciGpu(0x10DE, 0x28A0, "0000:01:00.0", 0, false);
    const GpuAdapter arc = describePciGpu(0x8086, 0x56A0, "0000:03:00.0", 0, false);
    const GpuAdapter apu = describePciGpu(0x1002, 0x15BF, "0000:c4:00.0", 512ull * 1024 * 1024, true);
    const GpuAdapter radeon = describePciGpu(0x1002, 0x73DF, "0000:03:00.0", 12 * gib, false);
    const GpuAdapter oldRadeon = describePciGpu(0x1002, 0x6798, "0000:02:00.0", 0, false);
    const GpuAdapter oldBootRadeon = describePciGpu(0x1002, 0x9874, "0000:00:01.0", 0, true);
    CHECK(intel.kind == GpuKind::Integrated);
    CHECK(nvidia.kind == GpuKind::Discrete);
    CHECK(arc.kind == GpuKind::Discrete);
    CHECK(apu.kind == GpuKind::Integrated);
    CHECK(radeon.kind == GpuKind::Discrete);
    CHECK(radeon.dedicatedMemory == 12 * gib);
    CHECK(oldRadeon.kind == GpuKind::Unknown);
    CHECK(oldBootRadeon.kind == GpuKind::Integrated);
    CHECK(nvidia.vendor == "NVIDIA");
    CHECK(nvidia.name == "NVIDIA GPU (0000:01:00.0)");
    CHECK(nvidia.systemIndex == -1);

    // Slot order lists the integrated GPU first; the ranking (and so the export encoders) puts the discrete one first.
    std::vector<GpuAdapter> laptop{intel, nvidia};
    assignGpuKeys(laptop);
    CHECK(laptop[0].key != laptop[1].key);
    CHECK(rankGpuAdapters(laptop, GpuPreference::HighPerformance).front().vendor == "NVIDIA");
    CHECK(rankGpuAdapters(laptop, GpuPreference::MinimumPower).front().vendor == "Intel");
    std::vector<GpuAdapter> amd{apu, radeon};
    assignGpuKeys(amd);
    CHECK(rankGpuAdapters(amd, GpuPreference::HighPerformance).front().deviceId == 0x73DF);
    const GpuSelection s = selectGpu(laptop, {});
    REQUIRE(s.adapter);
    CHECK(s.adapter->vendor == "NVIDIA");
}

TEST_CASE("This computer's GPUs: hardware only, each listed once, one of them chosen", "[media][gpu]") {
    ResetGpus reset;
    setGpuAdaptersForTesting(std::nullopt);
    const std::vector<GpuAdapter> listed = gpuAdapters();
    std::vector<std::string> keys;
    for (const GpuAdapter& a : listed) {
        INFO(a.name << " key " << a.key << " system " << a.systemIndex << " high " << a.highPerformanceIndex << " low "
                    << a.minimumPowerIndex << " kind " << static_cast<int>(a.kind) << " memory " << a.dedicatedMemory);
        CHECK_FALSE(a.software);
        CHECK_FALSE(a.key.empty());
        CHECK(std::ranges::find(keys, a.key) == keys.end());
        keys.push_back(a.key);
#if defined(_WIN32)
        CHECK(a.systemIndex >= 0);
        CHECK(a.luid != 0);
#endif
    }
    const GpuSelection selected = selectedGpu();
    CHECK(selected.adapter.has_value() == !listed.empty());
    if (selected.adapter) {
        CHECK(std::ranges::find(listed, selected.adapter->key, &GpuAdapter::key) != listed.end());
        CHECK(gpuVendors().front() == selected.adapter->vendor);
    }
}
