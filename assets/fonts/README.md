# Display font

Barlow Semi Condensed SemiBold is Copyright 2017 The Barlow Project Authors,
redistributed under the [SIL Open Font License 1.1](OFL.txt). This license also
applies to its generated bitmap font data in `StatusFonts.h`.

Source: [Google Fonts, pinned revision 89f5431](https://github.com/google/fonts/tree/89f5431ff0db41bd2fe3f7ba21a723a01622428b/ofl/barlowsemicondensed).

TTF SHA-256: `bd299f4bc5b44d30be8d42e9bad3a5df7d66af1cd55d0ed72a8b8916360a1424`.

The checked-in header contains native 8-bit grayscale VLW glyphs. M5GFX blends
their edges against black, with no runtime filesystem or sprite allocation.
Title and detail fonts cover printable ASCII; the smaller progress title only
includes the characters in “Sending.” The display uses native pixel sizes,
without pixel replication or further scaling.

To regenerate after changing typography:

```sh
.venv/bin/python scripts/generate_fonts.py --preview .local/font-preview.png
.venv/bin/python scripts/generate_fonts.py --check
```

The generator uses Pillow 12.3.0 (pinned in the project requirements). The optional
preview checks layout bounds and shows the actual glyph masks at 3× nearest-
neighbor magnification. It does not model the physical LCD's dim backlight.
