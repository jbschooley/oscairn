# Draws the repository banner and the social preview from the mark and the app's own font
# (Inter, from the built-in theme), as outlines, so they look the same wherever they are shown:
#   python3 branding/banner.py
# then render the PNGs with Inkscape (see README.md).
import re
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

here = Path(__file__).resolve().parent
fonts = here.parent / "hosts/standalone/themes/oscairn-classic/fonts"
dark, light, muted, accent = "#15171c", "#f3efe8", "#9a9ca4", "#f29a3f"

def text(string, font, size, x, baseline, fill, tracking=0.0):
    """The string as one SVG path, its left edge at x and its baseline at `baseline`; returns (svg, width)."""
    f = TTFont(fonts / font)
    glyphs, cmap, hmtx = f.getGlyphSet(), f.getBestCmap(), f["hmtx"]
    scale = size / f["head"].unitsPerEm
    pen = SVGPathPen(glyphs)
    advance = 0.0
    for ch in string:
        name = cmap[ord(ch)]
        # Font units are y-up; the page is y-down.
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, x + advance, baseline)))
        advance += hmtx[name][0] * scale + tracking * size
    return f'<path d="{pen.getCommands()}" fill="{fill}"/>', advance - tracking * size

def mark(x, y, size):
    """The mark (mark-on-dark.svg, a 100 x 100 box) placed at (x, y), size pixels square."""
    body = re.search(r"<svg[^>]*>(.*)</svg>", (here / "mark-on-dark.svg").read_text(), re.S).group(1)
    return f'<g transform="translate({x} {y}) scale({size / 100})">{body}</g>'

def ripples(width, y):
    """Faint waves across the bottom: the oscillator, quiet."""
    out = []
    for i, (amp, period, opacity) in enumerate([(10, 240, .10), (7, 170, .07)]):
        d, x = f"M -40 {y + i * 22}", -40
        while x < width + period:
            d += f" q {period / 4:.1f} {-amp * 2} {period / 2:.1f} 0 t {period / 2:.1f} 0"
            x += period
        out.append(f'<path d="{d}" fill="none" stroke="{accent}" stroke-width="3" opacity="{opacity}" stroke-linecap="round"/>')
    return "".join(out)

tagline = "An open instrument platform: synths, samplers, effects and sequencers"

def canvas(width, height, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
            f'<rect width="{width}" height="{height}" fill="{dark}"/>{ripples(width, height * .86)}{body}</svg>\n')

def banner(width, height, mark_size, name_size, tag_size, file):
    """The mark, then the name over the tagline, the whole block centered."""
    gap = mark_size * .26
    _, name_w = text("oscairn", "Inter-Bold.ttf", name_size, 0, 0, light, -0.02)
    _, tag_w = text(tagline, "Inter-Regular.ttf", tag_size, 0, 0, muted)
    left = (width - (mark_size + gap + max(name_w, tag_w))) / 2
    name_x, middle = left + mark_size + gap, height * .44
    name_svg, _ = text("oscairn", "Inter-Bold.ttf", name_size, name_x, middle + name_size * .18, light, -0.02)
    tag_svg, _ = text(tagline, "Inter-Regular.ttf", tag_size, name_x + name_size * .04, middle + name_size * .18 + tag_size * 1.9, muted)
    (here / file).write_text(canvas(width, height, mark(left, middle - mark_size / 2, mark_size) + name_svg + tag_svg))

def stacked(width, height, mark_size, name_size, tag_size, file):
    """The mark over the name over the tagline, each centered."""
    _, name_w = text("oscairn", "Inter-Bold.ttf", name_size, 0, 0, light, -0.02)
    _, tag_w = text(tagline, "Inter-Regular.ttf", tag_size, 0, 0, muted)
    top = height * .14
    name_base = top + mark_size + name_size * .95
    name_svg, _ = text("oscairn", "Inter-Bold.ttf", name_size, (width - name_w) / 2, name_base, light, -0.02)
    tag_svg, _ = text(tagline, "Inter-Regular.ttf", tag_size, (width - tag_w) / 2, name_base + tag_size * 2.1, muted)
    (here / file).write_text(canvas(width, height, mark((width - mark_size) / 2, top, mark_size) + name_svg + tag_svg))

banner(1280, 320, 168, 100, 24, "banner.svg")             # the README's title
stacked(1280, 640, 200, 120, 30, "social-preview.svg")    # GitHub's social preview, 2:1
