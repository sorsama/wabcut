# Third-party notices

Third-party assets bundled with Wabcut. These are files kept in this repository and compiled into the
application, not libraries pulled in at build time (those are listed in the installed
`THIRD-PARTY-NOTICES.txt`, generated from `cmake/THIRD-PARTY-NOTICES.txt.in`).

The fonts are unmodified copies of the upstream files. Each SHA-256 below was checked against the downloaded
file, and each file matches the git blob listed for it at the pinned commit. The SIL Open Font License does not
allow a modified version (subsetting counts) to use a Reserved Font Name, so a subset of IBM Plex must not be
called "Plex".

## Fonts

All font files are in `src/app/resources/fonts/`.

### Manrope

- Copyright: Copyright 2019 The Manrope Project Authors (https://github.com/sharanda/manrope)
- Designer: Mikhail Sharanda
- Version: 4.504 (name table)
- Source: https://github.com/googlefonts/manrope, commit `6f81ebecdf65e4463b798cc07b16a4f8d5216917` (the commit google/fonts records for
  `ofl/manrope` in its `upstream_info.md`)
  - Files: `https://raw.githubusercontent.com/googlefonts/manrope/6f81ebecdf65e4463b798cc07b16a4f8d5216917/fonts/ttf/<file>`, upstream names
    `manrope-regular.ttf`, `manrope-medium.ttf`, `manrope-semibold.ttf`, `manrope-bold.ttf`, `manrope-extrabold.ttf`,
    stored as `Manrope-<Weight>.ttf`.

| File | SHA-256 |
| --- | --- |
| `Manrope-Regular.ttf` | `2d9a9960fd191a7f1d9060768818074dd2b76ba84a64a35efd2c22bf39030903` |
| `Manrope-Medium.ttf` | `42133571c5f19b6d5ded5e3935a92c1dd40721fd8ca2529719eabfa58c123aec` |
| `Manrope-SemiBold.ttf` | `e80a2c37e47ec09bff5a9960257bf9001e87d5fb7bf35719ef4958763a0c70ac` |
| `Manrope-Bold.ttf` | `2da33eb378d59e6314ed7afdfa837cdcb60e41ac8b1f5d3c4909471b95fcf7d9` |
| `Manrope-ExtraBold.ttf` | `2522cd61b754100a42a53024148a134c63efc5d114447474a7f13519966cfad5` |

License: SIL Open Font License 1.1 (full text below and in `src/app/resources/fonts/OFL-Manrope.txt`).

### JetBrains Mono

- Copyright: Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)
- Designers: Philipp Nurullin, Konstantin Bulenkov
- Version: 2.304 (name table), release `v2.304`
- Source: https://github.com/JetBrains/JetBrainsMono, tag `v2.304`, commit `cd5227bd1f61dff3bbd6c814ceaf7ffd95e947d9`
  - Release: https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304
  - Files: `https://raw.githubusercontent.com/JetBrains/JetBrainsMono/cd5227bd1f61dff3bbd6c814ceaf7ffd95e947d9/fonts/ttf/<file>`

| File | SHA-256 |
| --- | --- |
| `JetBrainsMono-Regular.ttf` | `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f` |
| `JetBrainsMono-Medium.ttf` | `31c92d01a8a08528b718a43addf0ad3df0af2ca4b7b3290a452f70f358e14d3d` |
| `JetBrainsMono-SemiBold.ttf` | `1b3bfa1ed5665a4ce3f9feb68d2d4e40e70bf8b4b7d9a3edd418f321b4e166a0` |
| `JetBrainsMono-Bold.ttf` | `5590990c82e097397517f275f430af4546e1c45cff408bde4255dad142479dcb` |

License: SIL Open Font License 1.1 (full text below and in `src/app/resources/fonts/OFL-JetBrains-Mono.txt`).

### IBM Plex Sans

- Copyright: Copyright © 2017 IBM Corp. with Reserved Font Name "Plex"
- Designers: Mike Abbink, Paul van der Laan, Pieter van Rosmalen (Bold Monday, for IBM)
- Version: font version 3.005 (name table), release `@ibm/plex-sans@1.1.0`
- Source: https://github.com/IBM/plex, tag `@ibm/plex-sans@1.1.0`, commit `1da12f02587b630c07e92692d21492d722f53614`
  - Release: https://github.com/IBM/plex/releases/tag/%40ibm/plex-sans%401.1.0
  - Files: `https://raw.githubusercontent.com/IBM/plex/1da12f02587b630c07e92692d21492d722f53614/packages/plex-sans/fonts/complete/ttf/<file>`
  - The files are identical to the ones in the release's `ibm-plex-sans.zip`.

| File | SHA-256 |
| --- | --- |
| `IBMPlexSans-Regular.ttf` | `975dcda37d80f038dcd143c22e33ca2d97a0cc5a929aace1c749153b0fe1afa5` |
| `IBMPlexSans-Medium.ttf` | `331c8639d7598b2cde62a911a71db195e30cb655cd6bdf2e324a7e984955f907` |
| `IBMPlexSans-SemiBold.ttf` | `a20caf8286023a6a7a85e40b1d2a4ae9fc3e3b1f9eda8f4c542dd4986af67bb1` |

License: SIL Open Font License 1.1 (full text below and in `src/app/resources/fonts/OFL-IBM-Plex.txt`).

### IBM Plex Sans Thai

- Copyright: Copyright © 2017 IBM Corp. with Reserved Font Name "Plex"
- Designers: Mike Abbink, Paul van der Laan, Pieter van Rosmalen, Ben Mitchell, Mark Frömberg (Bold Monday, for IBM)
- Version: font version 1.2 (name table), release `@ibm/plex-sans-thai@1.1.0`
- Source: https://github.com/IBM/plex, tag `@ibm/plex-sans-thai@1.1.0`, commit `1da12f02587b630c07e92692d21492d722f53614`
  - Release: https://github.com/IBM/plex/releases/tag/%40ibm/plex-sans-thai%401.1.0
  - Files: `https://raw.githubusercontent.com/IBM/plex/1da12f02587b630c07e92692d21492d722f53614/packages/plex-sans-thai/fonts/complete/ttf/<file>`
  - The files are identical to the ones in the release's `ibm-plex-sans-thai.zip`.

| File | SHA-256 |
| --- | --- |
| `IBMPlexSansThai-Regular.ttf` | `83e1db8e8bad06bb760981f1dd528f5f209d20dfadebac12b21bf2f12453c8c6` |
| `IBMPlexSansThai-Medium.ttf` | `a88c2ecd469b3a29bf9e77e40930aed43644062f0e4d604de3db4e31755a9f59` |
| `IBMPlexSansThai-SemiBold.ttf` | `c0d88539aa6117a845a19ea4e83c2c0efc56dc43ebe41935b54d316ad2d6c7d5` |
| `IBMPlexSansThai-Bold.ttf` | `64451229916cfbfa8bb1bbdf3674b8ba96c00e04d3bc81d148b7bfb8330d8fd8` |

License: SIL Open Font License 1.1 (full text below and in `src/app/resources/fonts/OFL-IBM-Plex.txt`).

### IBM Plex Mono

- Copyright: Copyright © 2017 IBM Corp. with Reserved Font Name "Plex"
- Designers: Mike Abbink, Paul van der Laan, Pieter van Rosmalen (Bold Monday, for IBM)
- Version: font version 2.005 (name table), release `@ibm/plex-mono@2.5.0`
- Source: https://github.com/IBM/plex, tag `@ibm/plex-mono@2.5.0`, commit `2f9ba1b25957d958db71a849e85d72e3ecfb845a`
  - Release: https://github.com/IBM/plex/releases/tag/%40ibm/plex-mono%402.5.0
  - Files: `https://raw.githubusercontent.com/IBM/plex/2f9ba1b25957d958db71a849e85d72e3ecfb845a/packages/plex-mono/fonts/complete/ttf/<file>`
  - The files are identical to the ones in the release's `ibm-plex-mono.zip`
    (SHA-256 `6d23f01257663d8cc49a0d64c22ced630b79e0e2a0ac08a0da86e9a38bbc481c`, as published by GitHub).

| File | SHA-256 |
| --- | --- |
| `IBMPlexMono-Regular.ttf` | `7c6fbddca4b700be918f5f6183d9bd4464fa427fe435f0b480d77fe2bb8c5a43` |
| `IBMPlexMono-Medium.ttf` | `98fbd727aae340b236955879dabed4d991aac9e8e90b3b2a67ce4a59221cc97c` |
| `IBMPlexMono-SemiBold.ttf` | `f04d7c488ddf7d1fa99f2574efc3406ea4cbe17bb1af3a1ab960f84d0c96a172` |

License: SIL Open Font License 1.1 (full text below and in `src/app/resources/fonts/OFL-IBM-Plex.txt`).

### Bai Jamjuree

- Copyright: Copyright 2018 Bai Jamjuree (https://github.com/cadsondemak/Bai-Jamjuree)
- Designer: Cadson Demak (manufacturer in the name table: Cadson Demak Co.,Ltd.; designer: Katatrad Aksorn Co.,Ltd.)
- Version: 1.000 (name table)
- Source: https://github.com/google/fonts, path `ofl/baijamjuree/`, commit `c936464ebf3a837c3f9e4164f81a707bf9231dcb`
  - Files: `https://raw.githubusercontent.com/google/fonts/c936464ebf3a837c3f9e4164f81a707bf9231dcb/ofl/baijamjuree/<file>`
  - Upstream project: https://github.com/cadsondemak/Bai-Jamjuree

| File | SHA-256 |
| --- | --- |
| `BaiJamjuree-Medium.ttf` | `95782f3c3e07866749fb28e1d1a356e383e8f2bdcc7b88d4e3a3fc546613137f` |
| `BaiJamjuree-SemiBold.ttf` | `daaa68bc721e893861f948f40542b9f1ea8496a3fe4250a0ffc86dab0aee8f3c` |

License: SIL Open Font License 1.1 (full text below and in `src/app/resources/fonts/OFL-Bai-Jamjuree.txt`).

### License files

- `src/app/resources/fonts/OFL-Manrope.txt` is `OFL.txt` from googlefonts/manrope commit `6f81ebecdf65e4463b798cc07b16a4f8d5216917`, unchanged.
- `src/app/resources/fonts/OFL-JetBrains-Mono.txt` is `OFL.txt` from JetBrains/JetBrainsMono commit `cd5227bd1f61dff3bbd6c814ceaf7ffd95e947d9`,
  unchanged.
- `src/app/resources/fonts/OFL-IBM-Plex.txt` is `packages/plex-sans/LICENSE.txt` from IBM/plex commit
  `1da12f02587b630c07e92692d21492d722f53614`. The `plex-sans-thai` and `plex-mono` packages, and the
  `@ibm/plex-mono@2.5.0` commit, carry the same file. Line endings were changed from CRLF to LF; the text is unchanged.
- `src/app/resources/fonts/OFL-Bai-Jamjuree.txt` is `ofl/baijamjuree/OFL.txt` from google/fonts commit
  `c936464ebf3a837c3f9e4164f81a707bf9231dcb`, unchanged.

## Icons

### Boxicons

- Files: the SVG icons in `src/app/resources/icons/` (Boxicons 3, npm package `@boxicons/core` 1.0.6, from
  https://boxicons.com), stored under Wabcut's own file names; `tools/icons/boxicons-map.json` lists which glyph each
  file is. The paths are unchanged; each file gets a 24x24 `viewBox` and a black fill (`tools/icons/import_boxicons.py`).
- Copyright: Copyright (c) 2026 Boxicons.
- License: MIT License (full text in `src/app/resources/icons/LICENSE`).
- Not Boxicons: the glyphs listed under `own` in `tools/icons/boxicons-map.json` (the monitor buttons `mark-in`,
  `mark-out`, `go-to-in`, `go-to-out`, `play-in-out`, `insert-edit`, `overwrite-edit`, `lift-edit`, `extract-edit`,
  `safe-margins`, and `grip-vertical`, `type`), the logo and app icons in `src/app/resources/brand/` and the
  illustrations in `src/app/resources/illustrations/` are Wabcut's own drawings, under Wabcut's license.

## License texts

### SIL Open Font License 1.1: Manrope

```text
Copyright 2018 The Manrope Project Authors (https://github.com/sharanda/manrope)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

### SIL Open Font License 1.1: JetBrains Mono

```text
Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

### SIL Open Font License 1.1: IBM Plex (Sans, Sans Thai, Mono)

```text
Copyright © 2017 IBM Corp. with Reserved Font Name "Plex"

This Font Software is licensed under the SIL Open Font License, Version 1.1.

This license is copied below, and is also available with a FAQ at: http://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

### SIL Open Font License 1.1: Bai Jamjuree

```text
Copyright 2018 Bai Jamjuree (https://github.com/cadsondemak/Bai-Jamjuree)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Optional managed local assistant downloads (AI-10; verified 2026-10-11)

These files are downloaded on demand, not built from source or bundled into the editor executable.
Pinned source revisions, exact byte sizes, SHA-256 digests and runtime policy are recorded in
[docs/ai.md](docs/ai.md#pinned-downloads-and-model-cards) and `src/ai/src/ModelStore.cpp`.

- **llama.cpp llama-server**, ggml authors, official release `b11429`: MIT.
  [Pinned license](https://github.com/ggml-org/llama.cpp/blob/b11429/LICENSE).
- **Qwen3-4B-Instruct-2507**, Qwen/Alibaba Cloud: Apache-2.0.
  [Original model license/card](https://huggingface.co/Qwen/Qwen3-4B-Instruct-2507).
  Q4_K_M GGUF conversion by Unsloth, revision `a06e946bb6b655725eafa393f4a9745d460374c9`, Apache-2.0.
- **Qwen3-8B-GGUF**, Qwen/Alibaba Cloud: Apache-2.0.
  [Official GGUF repository](https://huggingface.co/Qwen/Qwen3-8B-GGUF/tree/7c41481f57cb95916b40956ab2f0b139b296d974).
- **Phi-4-mini-instruct**, Microsoft: MIT.
  [Original model license/card](https://huggingface.co/microsoft/Phi-4-mini-instruct).
  Q4_K_M GGUF conversion by Unsloth, revision `78eb92a46fc37e6b524df991ed9aca9bc6aa7b80`, MIT.
- **NVIDIA CUDA 12.4 runtime/cuBLAS**, optional separate llama.cpp runtime archive:
  [NVIDIA CUDA Toolkit EULA, redistributables Attachment A](https://docs.nvidia.com/cuda/eula/index.html).
  This is not an allow-listed model license. The planner requires explicit CUDA acceptance;
  the current UI uses Vulkan/CPU and does not download CUDA dependencies.

**c2pa-c** (Content Authenticity Initiative, Apache-2.0 OR MIT) was evaluated but is not linked or
redistributed in this implementation. The unsigned JSON/XMP fallback does not include C2PA signing
libraries or a signing certificate. Gemini and Runway are optional remote services under their own
terms, not redistributed model weights; uploads and spend require explicit consent.

## Caption translation models (AI-12; verified 2026-10-12)

Optional downloads for translating captions on this computer, never bundled: each language pair's model is downloaded on
demand (`wabcut::ai::ModelStore`, licence card first, every file SHA-256 verified) and run by `wabcut-embed` on ONNX Runtime.
Pinned revisions, sizes and digests are in `src/ai/src/TranslationManifest.cpp`; the pairs in
[docs/ai.md](docs/ai.md#going-global-ai-12). Text-to-speech has no local model (no permissive Thai voice; Piper needs
eSpeak NG, GPL-3.0): voice-over uses the user's own cloud provider and nothing is redistributed for it.

- **OPUS-MT** (Jörg Tiedemann, Santhosh Thottingal and the Language Technology Research Group at the University of Helsinki;
  "OPUS-MT — Building open translation services for the World", EAMT 2020), [github.com/Helsinki-NLP/Opus-MT](https://github.com/Helsinki-NLP/Opus-MT).
  The models, each from its Hugging Face card, in the 8-bit ONNX exports by Xenova (unchanged, the same licence):
  - [Helsinki-NLP/opus-mt-en-mul](https://huggingface.co/Helsinki-NLP/opus-mt-en-mul) (Apache-2.0), export [Xenova/opus-mt-en-mul](https://huggingface.co/Xenova/opus-mt-en-mul) at `e57f9759f8e1`
  - [Helsinki-NLP/opus-mt-th-en](https://huggingface.co/Helsinki-NLP/opus-mt-th-en) (Apache-2.0), export [Xenova/opus-mt-th-en](https://huggingface.co/Xenova/opus-mt-th-en) at `7f0406b972f7`
  - [Helsinki-NLP/opus-mt-en-es](https://huggingface.co/Helsinki-NLP/opus-mt-en-es) (Apache-2.0), export [Xenova/opus-mt-en-es](https://huggingface.co/Xenova/opus-mt-en-es) at `4b002a4c7edd`
  - [Helsinki-NLP/opus-mt-en-fr](https://huggingface.co/Helsinki-NLP/opus-mt-en-fr) (Apache-2.0), export [Xenova/opus-mt-en-fr](https://huggingface.co/Xenova/opus-mt-en-fr) at `28726206f808`
  - [Helsinki-NLP/opus-mt-en-de](https://huggingface.co/Helsinki-NLP/opus-mt-en-de) (CC-BY-4.0), export [Xenova/opus-mt-en-de](https://huggingface.co/Xenova/opus-mt-en-de) at `1ca130c44c4c`
  - [Helsinki-NLP/opus-mt-en-zh](https://huggingface.co/Helsinki-NLP/opus-mt-en-zh) (Apache-2.0), export [Xenova/opus-mt-en-zh](https://huggingface.co/Xenova/opus-mt-en-zh) at `046f55aec303`
  - [Helsinki-NLP/opus-mt-ja-en](https://huggingface.co/Helsinki-NLP/opus-mt-ja-en) (Apache-2.0), export [Xenova/opus-mt-ja-en](https://huggingface.co/Xenova/opus-mt-ja-en) at `1a906cfaaf7c`
  - [Helsinki-NLP/opus-mt-ko-en](https://huggingface.co/Helsinki-NLP/opus-mt-ko-en) (Apache-2.0), export [Xenova/opus-mt-ko-en](https://huggingface.co/Xenova/opus-mt-ko-en) at `d9fa1ac60082`
  - [Helsinki-NLP/opus-mt-zh-en](https://huggingface.co/Helsinki-NLP/opus-mt-zh-en) (CC-BY-4.0), export [Xenova/opus-mt-zh-en](https://huggingface.co/Xenova/opus-mt-zh-en) at `39d480d52a9e`
  - [Helsinki-NLP/opus-mt-es-en](https://huggingface.co/Helsinki-NLP/opus-mt-es-en) (Apache-2.0), export [Xenova/opus-mt-es-en](https://huggingface.co/Xenova/opus-mt-es-en) at `eadfd7c658a9`
  - [Helsinki-NLP/opus-mt-fr-en](https://huggingface.co/Helsinki-NLP/opus-mt-fr-en) (Apache-2.0), export [Xenova/opus-mt-fr-en](https://huggingface.co/Xenova/opus-mt-fr-en) at `6b166a182780`
  - [Helsinki-NLP/opus-mt-de-en](https://huggingface.co/Helsinki-NLP/opus-mt-de-en) (Apache-2.0), export [Xenova/opus-mt-de-en](https://huggingface.co/Xenova/opus-mt-de-en) at `399dfd687067`
- **Attribution for the CC-BY-4.0 models** (opus-mt-en-de, opus-mt-zh-en): © Language Technology Research Group, University of
  Helsinki; licensed under [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/);
  converted to ONNX and 8-bit quantized by Xenova; not otherwise modified. The same attribution is on the model card shown before
  the download.
- The Apache-2.0 models are used under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0); no NOTICE files are
  published with them.

## Local audio models and the DeepFilterNet signal processing (A.4; verified 2026-10-12)

The models are downloaded on demand (`wabcut::ai::ModelStore`, licence card first, SHA-256 verified), never bundled. Pinned
sources, sizes and digests are in [docs/ai.md](docs/ai.md#enhance-speech-and-stem-separation-a4) and
`src/ai/src/ModelStore.cpp`.

- **DeepFilterNet3** (Enhance Speech), Hendrik Schröter, [github.com/Rikorose/DeepFilterNet](https://github.com/Rikorose/DeepFilterNet):
  the code and the model weights are dual-licensed MIT OR Apache-2.0 at your option; Wabcut uses them under the MIT License
  below. The ONNX archive comes from commit `84d57ec2c08fe08e68a13fb32a58cd7092060a0f`.
- **The DeepFilterNet signal processing in `src/ai/audio/src/DeepFilter.cpp`** is a C++ port of libDF (`libDF/src/lib.rs`,
  `libDF/src/tract.rs` at the same commit): the ERB filter bank, the Vorbis-window STFT, the feature normalisations, the mask and
  the deep filter. It is distributed under the MIT License below, with that copyright.
- **HTDemucs** (stem separation), Simon Rouard, Francisco Massa, Alexandre Défossez (Meta),
  [github.com/facebookresearch/demucs](https://github.com/facebookresearch/demucs): MIT License; the repository releases the
  pretrained models under it.
- **The HTDemucs ONNX export** by StemSplit ([huggingface.co/StemSplitio/htdemucs-onnx](https://huggingface.co/StemSplitio/htdemucs-onnx),
  revision `d54ed9eb60e258ea82131c6ee14578628816456a`; pipeline [github.com/StemSplit/demucs-onnx](https://github.com/StemSplit/demucs-onnx)):
  MIT License.

Copyright lines, each under the MIT License text that follows:

- Copyright (c) 2021 Hendrik Schröter
- Copyright (c) Meta Platforms, Inc. and affiliates.
- Copyright (c) 2026 StemSplit

```
MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## AI Object Mask model (AI-14; verified 2026-10-12)

An optional download for the AI Object Mask, never bundled: downloaded on request (`wabcut::ai::ModelStore`, licence card
first, every file SHA-256 verified) and run by `wabcut-embed` on ONNX Runtime. Sizes and digests are in
[docs/ai.md](docs/ai.md#ai-object-mask-ai-14) and `src/ai/src/ModelStore.cpp`.

- **SAM 2.1 Hiera-Tiny** (Segment Anything Model 2.1; Nikhila Ravi et al., Meta FAIR),
  [github.com/facebookresearch/sam2](https://github.com/facebookresearch/sam2): the code and the model weights are under the
  Apache License 2.0 (Copyright (c) Meta Platforms, Inc. and affiliates); weights
  [facebook/sam2.1-hiera-tiny](https://huggingface.co/facebook/sam2.1-hiera-tiny) at `de431c4043854a71d8101e17995dfe596bf101a5`.
  The release's connected-components post-processing (cc_torch, BSD-3-Clause) is not part of the export and not used.
- **The ONNX export of its video tracker** by square-zero-labs (from Hugging Face transformers' `Sam2VideoModel`),
  [huggingface.co/square-zero-labs/sam2.1-tiny-video-onnx](https://huggingface.co/square-zero-labs/sam2.1-tiny-video-onnx),
  revision `3b2984dd865f6e9d2cc6aed0be6a5a5c2eb352ce`: Apache License 2.0. Files (SHA-256): `onnx/vision_encoder.onnx`
  `aa7a8542942f042e235a993a1ab0ccf5f049918500577802a7f10ec1b39bb873`, `onnx/mask_decoder.onnx`
  `0461896de3db00936fe1643506f129d71cb6d5d2ae15754811756b7ea1b070c6`, `onnx/memory_attention.onnx`
  `791e648ce8f5ef91ad00ba06e83066ff261ae5a645f0b042b4f46a89fd054baf`, `onnx/memory_encoder.onnx`
  `580d246c109de88838f600ba7c1c0d03d1fe267f7641b06f8c88c5f0dc5834cd`, `onnx/pointer_tpos.onnx`
  `7e71df1d75dba09bc18dd4ae745c2a2cceebdd14d84eadea2fe65d0205f101fc`, `constants.json`
  `172edc70e892aab0aef6caedf97b8e4091fbf6c5d26dac5fd9ea69d093d927d3`.
- Both are used under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0); no NOTICE files are published
  with them. The AI Object Mask also asks YOLOX-Tiny (above, AI-9) for every COCO class when a subject is named.

## Audio plugin SDKs (A.6; verified 2026-10-12)

The audio plugin host (`src/plughost`, [docs/plugins.md](docs/plugins.md)) is built against two SDKs. Both come from vcpkg at
versions pinned by the manifest's baseline (feature `audio-plugins`); neither is kept in this repository. They are listed in
the installed `THIRD-PARTY-NOTICES.txt` with their licence files.

- **CLAP 1.2.10** (`clap-cleveraudio`, [github.com/free-audio/clap](https://github.com/free-audio/clap)): the plugin ABI
  headers. MIT License, Copyright (c) 2021 Alexandre BIQUE.
- **VST 3 SDK 3.8.1 build 84** (`vst3sdk`, [github.com/steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk)): the
  interfaces and the hosting classes. **MIT License**, Copyright (c) 2026 Steinberg Media Technologies GmbH.
  - Verified from the SDK's own `LICENSE.txt`, installed by vcpkg as `share/vst3sdk/copyright`.
  - Before 3.8 the SDK was dual-licensed (GPLv3, or Steinberg's proprietary licence). Wabcut (GPL-3.0-or-later) could have used
    it under the GPLv3 then. Under MIT there is no copyleft condition to meet, and the MIT notice is reproduced.
  - **The overlay port.** `cmake/ports/vst3sdk` is the stock vcpkg port of the same version. It also installs the SDK's
    `public.sdk/source/common` and `public.sdk/source/main` headers, which the plugin-host tests' VST3 test plugin needs.
  - **Trademark.** "VST" is a registered trademark of Steinberg Media Technologies GmbH. The MIT licence covers the code, not
    the trademark or the VST logo; Wabcut names the format, and shows no logo.
- **AU and LV2** are not hosted yet (a macOS / Linux follow-up), so their SDKs are not used.

The plugins a user hosts are their own software under their own licences; Wabcut neither bundles nor redistributes any.
