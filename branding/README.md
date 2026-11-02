# Oscairn's mark

The wave cairn: three waves stacked like a cairn's stones, narrowing and thinning upward, the top
one in the accent. *Osc* is the oscillator in every voice; a *cairn* is stones stacked to mark a
path.

| File | Use |
| --- | --- |
| `mark-on-dark.svg` | On dark grounds: light waves, accent top |
| `mark-on-light.svg` | On light grounds: dark waves, accent top |
| `mark-mono.svg` | One color, from the context (`currentColor`): embossing, single-color print, icons that take a theme's color |
| `mark-app-icon.svg`, `app-icon-1024.png` | The app icon: the mark on a dark rounded square, on the macOS icon grid (an 824 px square in the 1024 px canvas, so it is the size of the system's icons) |
| `banner.svg`, `banner.png` | The repository's title in README.md (1280 x 320) |
| `social-preview.svg`, `social-preview.png` | GitHub's social preview (1280 x 640): set in the repository's Settings, General, Social preview |
| `mark-hills-*` | An alternate kept for later: each wave centered on its crest |

Colors: accent `#f29a3f`, light `#f3efe8`, dark `#15171c`. The mark's ink is centered on its
100 x 100 box, stroke included, so it can be placed by its box.

`logo.mjs` draws every file from the geometry (`node branding/logo.mjs`); change it there, not in
the SVGs. The PNG is rendered from `mark-app-icon.svg` with Inkscape, which keeps the corners transparent:
`inkscape mark-app-icon.svg --export-type=png --export-filename=app-icon-1024.png -w 1024 -h 1024`.

`banner.py` draws the banner and social preview from `mark-on-dark.svg` and the app's own font
(Inter, from the built-in theme), as outlines, so they look the same without the font installed
(`python3 branding/banner.py`, which needs fontTools). Render the PNGs with
`inkscape banner.svg --export-type=png --export-filename=banner.png` (and the same for
`social-preview`).
