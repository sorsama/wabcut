#pragma once

// wabcut-ofxhost: the OpenFX host (0.7, docs/plugins.md). One loaded plug-in binary (a .ofx file inside a bundle), the
// plug-ins in it described to the editor (scan mode) and instances of them that render pictures (serve mode).
//
// Supported: the Image Effect API 1.x contexts Filter and General (one "Source" clip and the "Output"), CPU rendering in
// Float, Short or Byte pixels as RGBA or RGB, the property, parameter (double, int, boolean, choice, string choice,
// RGB(A), 2D and 3D double and int, string), image effect, memory, multi-thread and message suites. Not supported (the
// plug-in is told so through the host properties, and a plug-in that needs them is listed as unsupported): OpenGL,
// CUDA, Metal and OpenCL render suites, overlay interacts, custom and parametric parameters, tiles, multi-resolution,
// temporal clip access, the draw, colour-space, dialog and progress UI suites (progress is accepted and ignored).

#include <openfx/ofxCore.h>
#include <openfx/ofxImageEffect.h>
#include <openfx/ofxParam.h>
#include <openfx/ofxProperty.h>

#include <wabcut/ofxhost/Protocol.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The property set handle OpenFX hands around: a bag of typed, N-dimensional properties.
class PropertyBag {
public:
    enum class Kind : std::uint8_t { Int, Double, String, Pointer };
    struct Entry {
        Kind kind = Kind::Int;
        std::vector<int> ints;
        std::vector<double> doubles;
        std::vector<std::string> strings;
        std::vector<void*> pointers;
        std::size_t size() const;
        void resize(std::size_t n);
    };

    Entry* find(const std::string& name);
    const Entry* find(const std::string& name) const;
    /// The entry of `name`, made when missing; null when it exists with another kind.
    Entry* ensure(const std::string& name, Kind kind);
    bool has(const std::string& name) const { return m_items.find(name) != m_items.end(); }
    void reset(const std::string& name) { m_items.erase(name); }

    // Host-side conveniences (the host defines the properties it publishes).
    void setInt(const std::string& name, int value, std::size_t index = 0);
    void setDouble(const std::string& name, double value, std::size_t index = 0);
    void setString(const std::string& name, std::string_view value, std::size_t index = 0);
    void setPointer(const std::string& name, void* value, std::size_t index = 0);
    void setInts(const std::string& name, std::initializer_list<int> values);
    void setDoubles(const std::string& name, std::initializer_list<double> values);
    void setStrings(const std::string& name, std::initializer_list<std::string_view> values);

    int getInt(const std::string& name, int fallback = 0, std::size_t index = 0) const;
    double getDouble(const std::string& name, double fallback = 0.0, std::size_t index = 0) const;
    std::string getString(const std::string& name, const std::string& fallback = {}, std::size_t index = 0) const;
    void* getPointer(const std::string& name, std::size_t index = 0) const;
    /// Every value of a numeric property, as doubles (an int property converts).
    std::vector<double> numbers(const std::string& name) const;
    std::vector<std::string> strings(const std::string& name) const;
    std::size_t dimension(const std::string& name) const;

private:
    std::map<std::string, Entry> m_items;
};

struct OfxPropertySetStruct {
    PropertyBag bag;
};

namespace wabcut::ofxhost {

enum class ParamKind : std::uint8_t {
    Double,
    Int,
    Bool,
    Choice,
    StrChoice,
    Rgba,
    Rgb,
    Double2D,
    Int2D,
    Double3D,
    Int3D,
    String,
    Custom,
    Bytes,
    Group,
    Page,
    PushButton,
    Parametric,
    Unknown,
};

ParamKind paramKindOf(std::string_view ofxType);
std::string_view paramKindName(ParamKind kind);
/// Values a parameter of this kind holds (components), 0 for the kinds that hold none.
int paramDimension(ParamKind kind);
bool paramIsNumeric(ParamKind kind);

/// One parameter: its definition (props the plug-in set while describing) and the value of an instance.
struct ParamDef {
    std::string name;
    ParamKind kind = ParamKind::Unknown;
    OfxPropertySetStruct props;
    std::vector<double> numbers; ///< Double, Int, Bool, Choice, RGB(A), 2D, 3D values
    std::string text;            ///< String and StrChoice (the choice's enum text) values
    std::string returned;        ///< what paramGetValue last handed out for a string (valid until the next call)
};

struct ParamSet {
    struct Effect* effect = nullptr; ///< the effect this is the parameter set of
    std::vector<std::unique_ptr<ParamDef>> params;
    ParamDef* find(std::string_view name) const;
};

struct Clip {
    std::string name;
    OfxPropertySetStruct props;
    struct Effect* owner = nullptr;
};

struct Image;

/// A descriptor (describing) or an instance (rendering): the plug-in's effect handle.
struct Effect {
    OfxPropertySetStruct props;
    ParamSet params;
    std::vector<std::unique_ptr<Clip>> clips;
    struct PluginRuntime* plugin = nullptr;
    bool instance = false;
    std::string context;
    // Instance state.
    double time = 0.0;
    int width = 0;
    int height = 0;
    std::string lastMessage; ///< the last error message the plug-in posted
    std::set<Image*> images; ///< images handed out and not released yet
    Effect() { params.effect = this; }
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    Clip* find(std::string_view name) const;
};

/// A plug-in image the host handed to the plug-in: its property set and the pixels behind it.
struct Image {
    OfxPropertySetStruct props;
    std::vector<std::uint8_t> pixels;
    Effect* owner = nullptr;
};

/// One plug-in of a binary.
struct PluginRuntime {
    OfxPlugin* plugin = nullptr;
    std::string id;
    bool loaded = false;
    bool described = false;
    std::string problem;
    Effect descriptor; ///< what describe set
    std::map<std::string, std::unique_ptr<Effect>> contexts; ///< describeInContext's descriptors, by context
    /// The context instances are made in ("filter" or "general"), empty when none is usable.
    std::string context;
    /// The pixel depth and components the host gives it ("Float" | "Short" | "Byte", "RGBA" | "RGB").
    std::string depth;
    std::string components;
    int instances = 0; ///< live instances (a plug-in that allows one at a time)
};

/// A binary loaded by the helper.
class OfxModule {
public:
    static std::unique_ptr<OfxModule> load(const std::filesystem::path& file, std::string& error);
    ~OfxModule();
    OfxModule(const OfxModule&) = delete;
    OfxModule& operator=(const OfxModule&) = delete;

    std::vector<std::unique_ptr<PluginRuntime>>& plugins() { return m_plugins; }
    PluginRuntime* find(std::string_view id);
    /// Describes every plug-in (load, describe, describe in context), so scans and instances know them. Idempotent.
    void describeAll();
    /// The plug-ins as JSON for the scanner (id, name, contexts, parameters, problem).
    nlohmann::json describeJson();

private:
    OfxModule() = default;
    void* m_library = nullptr;
    std::vector<std::unique_ptr<PluginRuntime>> m_plugins;
};

/// A rendering instance of a plug-in.
class OfxInstance {
public:
    static std::unique_ptr<OfxInstance> create(PluginRuntime& plugin, int width, int height, double fps, std::string& error);
    ~OfxInstance();
    OfxInstance(const OfxInstance&) = delete;
    OfxInstance& operator=(const OfxInstance&) = delete;

    struct RenderArgs {
        int width = 0;
        int height = 0;
        double time = 0.0;
        double fps = 24.0;
        double pixelAspect = 1.0;
        bool hasAlpha = true;       ///< the source has an alpha channel (else it is opaque)
        const nlohmann::json* params = nullptr; ///< name -> value (a number, an array, a string)
    };
    /// Renders `input` (float RGBA premultiplied, top-down, width x height) into `output` (same). False with `error` when
    /// the plug-in failed.
    bool render(const RenderArgs& args, const float* input, float* output, std::string& error, OutputAlpha& alpha);
    /// The plug-in said its pictures change from frame to frame even with constant parameters and inputs (clip preferences).
    bool frameVarying() const { return m_frameVarying; }

private:
    OfxInstance() = default;
    bool applyParams(const nlohmann::json& params, double time, std::string& error);
    Effect m_effect;
    PluginRuntime* m_plugin = nullptr;
    bool m_created = false;
    OutputAlpha m_outputAlpha = OutputAlpha::Premultiplied;
    bool m_frameVarying = false;
    /// The last value the editor sent for each parameter (numbers, text): only changes reach the plug-in.
    std::map<std::string, std::pair<std::vector<double>, std::string>> m_lastApplied;
};

/// The host's own handle for fetchSuite.
const OfxHost* hostStruct();
/// The suites' view of an effect handle, and the helper's log of what plug-ins said.
void setLogPrefix(const char* prefix);

} // namespace wabcut::ofxhost
