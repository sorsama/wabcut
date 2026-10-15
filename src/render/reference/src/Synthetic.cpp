#include <wabcut/render/reference/Synthetic.h>

#include <algorithm>

namespace wabcut::render::reference {

Image colorMatte(int width, int height, const Rgb& encodedColor) {
    return Image::filled(width, height,
                         {static_cast<float>(toLinear(encodedColor[0])), static_cast<float>(toLinear(encodedColor[1])),
                          static_cast<float>(toLinear(encodedColor[2])), 1.0f});
}

Image blackVideo(int width, int height) { return Image::filled(width, height, {0.0f, 0.0f, 0.0f, 1.0f}); }

Image transparentVideo(int width, int height) { return Image(width, height); }

Image syntheticPicture(const MediaItem& item, int width, int height) {
    const SyntheticDescriptor* d = syntheticOf(item);
    if (!d) {
        return {};
    }
    switch (d->kind) {
    case SyntheticKind::ColorMatte:
        return colorMatte(width, height,
                          {syntheticParam(item, "color.r", 1.0), syntheticParam(item, "color.g", 1.0),
                           syntheticParam(item, "color.b", 1.0)});
    case SyntheticKind::BlackVideo:
        return blackVideo(width, height);
    case SyntheticKind::TransparentVideo:
        return transparentVideo(width, height);
    }
    return {};
}

} // namespace wabcut::render::reference
