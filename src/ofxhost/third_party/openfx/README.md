# OpenFX API headers (vendored)

The OpenFX image effect plug-in API headers, unmodified, from the official repository
<https://github.com/AcademySoftwareFoundation/openfx>, tag `OFX_Release_1.5.1` (commit
`ab779510b2655b4d11a7e01e5c521f9aa8c88976`). BSD 3-Clause (`LICENSE.md`; see `THIRD-PARTY-NOTICES.md` at the root).

Only the `include/*.h` headers are vendored. `ofxColour.h` and its generated OCIO header are left out: the colour-space
suite is not hosted (docs/plugins.md, "OpenFX video plug-ins"). Nothing is downloaded at build time.
