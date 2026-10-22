#include <wabcut/render/EffectLibrary.h>

#include <algorithm>

namespace wabcut::render {

namespace {

using O = LibraryOp;

constexpr std::array<std::string_view, 31> keys(std::initializer_list<std::string_view> list) {
    std::array<std::string_view, 31> out{};
    std::size_t i = 0;
    for (const std::string_view k : list) {
        out[i++] = k;
    }
    return out;
}

constexpr std::array kLibrary{
    // Color family (fx_color.frag).
    EffectLibraryEntry{"wabcut.color.brightnessContrast", O::BrightnessContrast, keys({"brightness", "contrast"})},
    EffectLibraryEntry{"wabcut.adjust.levels", O::Levels,
                       keys({"rgb.blackInput", "rgb.whiteInput", "rgb.blackOutput", "rgb.whiteOutput", "rgb.gamma",
                             "red.blackInput", "red.whiteInput", "red.blackOutput", "red.whiteOutput", "red.gamma",
                             "green.blackInput", "green.whiteInput", "green.blackOutput", "green.whiteOutput",
                             "green.gamma", "blue.blackInput", "blue.whiteInput", "blue.blackOutput", "blue.whiteOutput",
                             "blue.gamma"})},
    EffectLibraryEntry{"wabcut.adjust.procAmp", O::ProcAmp,
                       keys({"brightness", "contrast", "hue", "saturation", "splitScreen", "splitPercent"})},
    EffectLibraryEntry{"wabcut.adjust.extract", O::Extract, keys({"blackInput", "whiteInput", "softness", "invert"})},
    EffectLibraryEntry{"wabcut.channel.invert", O::Invert, keys({"channel", "blend"})},
    EffectLibraryEntry{"wabcut.color.tint", O::Tint,
                       keys({"mapBlack.r", "mapBlack.g", "mapBlack.b", "mapWhite.r", "mapWhite.g", "mapWhite.b", "amount"})},
    EffectLibraryEntry{"wabcut.color.changeColor", O::ChangeColor,
                       keys({"view", "hueTransform", "lightnessTransform", "saturationTransform", "color.r", "color.g",
                             "color.b", "tolerance", "softness", "match", "invert"})},
    EffectLibraryEntry{"wabcut.color.changeToColor", O::ChangeToColor,
                       keys({"from.r", "from.g", "from.b", "to.r", "to.g", "to.b", "change", "changeBy", "tolerance.hue",
                             "tolerance.lightness", "tolerance.saturation", "softness", "viewMatte"})},
    EffectLibraryEntry{"wabcut.color.leaveColor", O::LeaveColor,
                       keys({"amount", "color.r", "color.g", "color.b", "tolerance", "softness", "match"})},
    EffectLibraryEntry{"wabcut.color.balance", O::ColorBalance,
                       keys({"shadow.red", "shadow.green", "shadow.blue", "midtone.red", "midtone.green", "midtone.blue",
                             "highlight.red", "highlight.green", "highlight.blue", "preserveLuminosity"})},
    EffectLibraryEntry{"wabcut.color.videoLimiter", O::VideoLimiter,
                       keys({"clipLevel", "compression", "gamutWarning", "warningColor.r", "warningColor.g",
                             "warningColor.b"})},
    EffectLibraryEntry{"wabcut.image.blackWhite", O::BlackWhite, keys({})},
    EffectLibraryEntry{"wabcut.image.colorPass", O::ColorPass, keys({"color.r", "color.g", "color.b", "similarity", "reverse"})},
    EffectLibraryEntry{"wabcut.image.colorReplace", O::ColorReplace,
                       keys({"target.r", "target.g", "target.b", "replace.r", "replace.g", "replace.b", "similarity",
                             "solid"})},
    EffectLibraryEntry{"wabcut.image.gammaCorrection", O::GammaCorrection, keys({"gamma"})},
    EffectLibraryEntry{"wabcut.stylize.posterize", O::Posterize, keys({"level"})},
    EffectLibraryEntry{"wabcut.stylize.solarize", O::Solarize, keys({"threshold"})},
    EffectLibraryEntry{"wabcut.stylize.threshold", O::Threshold, keys({"level"})},
    // Strobe Light: params[10] is whether the strobe is on at this frame (the evaluator: Strobe Duration, Period,
    // Random Strobe Probability and Seed at the curve time).
    EffectLibraryEntry{"wabcut.stylize.strobeLight", O::StrobeLight,
                       keys({"color.r", "color.g", "color.b", "blend", "duration", "period", "probability", "strobe",
                             "operator", "seed"}),
                       true},
    EffectLibraryEntry{"wabcut.key.alphaAdjust", O::AlphaAdjust, keys({"opacity", "ignoreAlpha", "invertAlpha", "maskOnly"})},
    EffectLibraryEntry{"wabcut.key.luma", O::LumaKey, keys({"threshold", "cutoff"})},
    EffectLibraryEntry{"wabcut.noise.noise", O::Noise, keys({"amount", "colorNoise", "clip"}), true},
    // Distort family (fx_distort.frag).
    EffectLibraryEntry{"wabcut.distort.cornerPin", O::CornerPin,
                       keys({"upperLeft.x", "upperLeft.y", "upperRight.x", "upperRight.y", "lowerLeft.x", "lowerLeft.y",
                             "lowerRight.x", "lowerRight.y"})},
    EffectLibraryEntry{"wabcut.distort.lensDistortion", O::LensDistortion,
                       keys({"curvature", "verticalDecentering", "horizontalDecentering", "verticalPrism",
                             "horizontalPrism", "fillAlpha", "fillColor.r", "fillColor.g", "fillColor.b"})},
    EffectLibraryEntry{"wabcut.distort.magnify", O::Magnify,
                       keys({"shape", "center.x", "center.y", "magnification", "link", "size", "feather", "opacity",
                             "scaling", "blendingMode", "resizeLayer"})},
    EffectLibraryEntry{"wabcut.distort.mirror", O::Mirror, keys({"center.x", "center.y", "angle"})},
    EffectLibraryEntry{"wabcut.distort.offset", O::Offset, keys({"center.x", "center.y", "blend"})},
    EffectLibraryEntry{"wabcut.distort.spherize", O::Spherize, keys({"radius", "center.x", "center.y"})},
    EffectLibraryEntry{"wabcut.distort.twirl", O::Twirl, keys({"angle", "radius", "center.x", "center.y"})},
    EffectLibraryEntry{"wabcut.distort.waveWarp", O::WaveWarp,
                       keys({"waveType", "height", "width", "direction", "speed", "pinning", "phase", "antialiasing"}),
                       true},
    EffectLibraryEntry{"wabcut.distort.turbulentDisplace", O::TurbulentDisplace,
                       keys({"displacement", "amount", "size", "offset.x", "offset.y", "complexity", "evolution",
                             "cycleEvolution", "cycle", "seed", "pinning", "antialiasing"})},
    // Transform: params[14..22] are its anchor, position, scales, skew, skew axis and rotation at the end of the shutter
    // and params[23] = 1 when they are set (the evaluator; motion blur), as Premiere's Shutter Angle.
    EffectLibraryEntry{"wabcut.distort.transform", O::Transform,
                       keys({"anchor.x", "anchor.y", "position.x", "position.y", "uniformScale", "scaleHeight",
                             "scaleWidth", "skew", "skewAxis", "rotation", "opacity", "useCompositionShutter",
                             "shutterAngle", "sampling"})},
    EffectLibraryEntry{"wabcut.transform.horizontalFlip", O::HorizontalFlip, keys({})},
    EffectLibraryEntry{"wabcut.transform.verticalFlip", O::VerticalFlip, keys({})},
    EffectLibraryEntry{"wabcut.transform.edgeFeather", O::EdgeFeather, keys({"amount"})},
    EffectLibraryEntry{"wabcut.stylize.replicate", O::Replicate, keys({"count"})},
    EffectLibraryEntry{"wabcut.perspective.basic3d", O::Basic3D, keys({"swivel", "tilt", "distance", "specular", "wireframe"})},
    EffectLibraryEntry{"wabcut.perspective.bevelEdges", O::BevelEdges,
                       keys({"thickness", "lightAngle", "lightColor.r", "lightColor.g", "lightColor.b", "intensity"})},
    EffectLibraryEntry{"wabcut.blur.directional", O::DirectionalBlur, keys({"direction", "length"})},
    EffectLibraryEntry{"wabcut.blur.camera", O::CameraBlur, keys({"percent"})},
    // Stylize family (fx_stylize.frag).
    EffectLibraryEntry{"wabcut.blur.unsharpMask", O::UnsharpMask, keys({"amount", "radius", "threshold"})},
    EffectLibraryEntry{"wabcut.stylize.emboss", O::Emboss, keys({"direction", "relief", "contrast", "blend"})},
    EffectLibraryEntry{"wabcut.stylize.colorEmboss", O::ColorEmboss, keys({"direction", "relief", "contrast", "blend"})},
    EffectLibraryEntry{"wabcut.stylize.findEdges", O::FindEdges, keys({"invert", "blend"})},
    EffectLibraryEntry{"wabcut.adjust.convolutionKernel", O::ConvolutionKernel,
                       keys({"m11", "m12", "m13", "m21", "m22", "m23", "m31", "m32", "m33", "offset", "scale",
                             "processAlpha"})},
    EffectLibraryEntry{"wabcut.noise.median", O::Median, keys({"radius", "operateOnAlpha"})},
    EffectLibraryEntry{"wabcut.noise.dustScratches", O::DustScratches, keys({"radius", "threshold", "operateOnAlpha"})},
    EffectLibraryEntry{"wabcut.stylize.alphaGlow", O::AlphaGlow,
                       keys({"glow", "brightness", "startColor.r", "startColor.g", "startColor.b", "endColor.r",
                             "endColor.g", "endColor.b", "useEndColor", "fadeOut"})},
    EffectLibraryEntry{"wabcut.perspective.bevelAlpha", O::BevelAlpha,
                       keys({"thickness", "lightAngle", "lightColor.r", "lightColor.g", "lightColor.b", "intensity"})},
    EffectLibraryEntry{"wabcut.perspective.radialShadow", O::RadialShadow,
                       keys({"color.r", "color.g", "color.b", "opacity", "light.x", "light.y", "distance", "softness",
                             "render", "colorInfluence", "shadowOnly", "resizeLayer"})},
    EffectLibraryEntry{"wabcut.stylize.roughenEdges", O::RoughenEdges,
                       keys({"edgeType", "edgeColor.r", "edgeColor.g", "edgeColor.b", "border", "sharpness",
                             "fractalInfluence", "scale", "stretch", "offset.x", "offset.y", "complexity", "evolution",
                             "cycleEvolution", "cycle", "seed"})},
    EffectLibraryEntry{"wabcut.key.colorKey", O::ColorKey,
                       keys({"keyColor.r", "keyColor.g", "keyColor.b", "tolerance", "edgeThin", "edgeFeather"})},
    // Generate family (fx_generate.frag).
    EffectLibraryEntry{"wabcut.generate.fourColorGradient", O::FourColorGradient,
                       keys({"point1.x", "point1.y", "color1.r", "color1.g", "color1.b", "point2.x", "point2.y", "color2.r",
                             "color2.g", "color2.b", "point3.x", "point3.y", "color3.r", "color3.g", "color3.b", "point4.x",
                             "point4.y", "color4.r", "color4.g", "color4.b", "blend", "jitter", "opacity", "blendingMode"})},
    EffectLibraryEntry{"wabcut.generate.ramp", O::Ramp,
                       keys({"start.x", "start.y", "startColor.r", "startColor.g", "startColor.b", "end.x", "end.y",
                             "endColor.r", "endColor.g", "endColor.b", "shape", "scatter", "blend"})},
    EffectLibraryEntry{"wabcut.generate.circle", O::Circle,
                       keys({"center.x", "center.y", "radius", "edge", "thickness", "featherOuter", "featherInner", "invert",
                             "color.r", "color.g", "color.b", "opacity", "blendingMode"})},
    EffectLibraryEntry{"wabcut.generate.grid", O::Grid,
                       keys({"anchor.x", "anchor.y", "sizeFrom", "corner.x", "corner.y", "width", "height", "border",
                             "featherWidth", "featherHeight", "invert", "color.r", "color.g", "color.b", "opacity",
                             "blendingMode"})},
    EffectLibraryEntry{"wabcut.generate.checkerboard", O::Checkerboard,
                       keys({"anchor.x", "anchor.y", "sizeFrom", "corner.x", "corner.y", "width", "height",
                             "featherWidth", "featherHeight", "color.r", "color.g", "color.b", "opacity", "blendingMode"})},
    EffectLibraryEntry{"wabcut.generate.lensFlare", O::LensFlare,
                       keys({"center.x", "center.y", "brightness", "lensType", "blend"})},
    EffectLibraryEntry{"wabcut.transition.blockDissolve", O::BlockDissolve,
                       keys({"completion", "blockWidth", "blockHeight", "feather", "softEdges"})},
    // Gradient Wipe: LayerEffect::matte is the Gradient Layer's picture when it names another track.
    EffectLibraryEntry{"wabcut.transition.gradientWipe", O::GradientWipe,
                       keys({"completion", "softness", "gradientLayer", "placement", "invert"})},
    EffectLibraryEntry{"wabcut.transition.linearWipe", O::LinearWipe, keys({"completion", "angle", "feather"})},
    EffectLibraryEntry{"wabcut.transition.radialWipe", O::RadialWipe,
                       keys({"completion", "startAngle", "center.x", "center.y", "wipe", "feather"})},
    EffectLibraryEntry{"wabcut.transition.venetianBlinds", O::VenetianBlinds,
                       keys({"completion", "direction", "width", "feather"})},
    // The second-picture effects: LayerEffect::matte is what the evaluator prepared (Evaluate.cpp): the echoes, the
    // text drawn on the canvas, the Difference Layer's picture, the matte image.
    EffectLibraryEntry{"wabcut.time.echo", O::Echo, keys({"echoTime", "count", "startingIntensity", "decay", "operator"})},
    EffectLibraryEntry{"wabcut.video.timecode", O::TextOverlay,
                       keys({"position.x", "position.y", "size", "opacity", "fieldSymbol", "format", "source",
                             "timeDisplay", "offset", "startingTimecode", "labelText"})},
    EffectLibraryEntry{"wabcut.video.clipName", O::TextOverlay,
                       keys({"position.x", "position.y", "alignment", "size", "opacity", "display"})},
    EffectLibraryEntry{"wabcut.video.simpleText", O::TextOverlay,
                       keys({"position.x", "position.y", "alignment", "size", "opacity", "background"})},
    EffectLibraryEntry{"wabcut.key.differenceMatte", O::DifferenceMatte,
                       keys({"view", "differenceLayer", "ifSizesDiffer", "tolerance", "softness", "blur"})},
    EffectLibraryEntry{"wabcut.key.imageMatte", O::ImageMatteKey, keys({"compositeUsing", "reverse"})},
};

} // namespace

std::span<const EffectLibraryEntry> effectLibrary() {
    return kLibrary;
}

const EffectLibraryEntry* findLibraryEffect(std::string_view id) {
    const auto it = std::ranges::find_if(kLibrary, [&](const EffectLibraryEntry& e) { return e.id == id; });
    return it != kLibrary.end() ? &*it : nullptr;
}

} // namespace wabcut::render
