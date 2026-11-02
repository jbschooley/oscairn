// Draws Oscairn's mark, the wave cairn, into SVG files in this folder: node branding/logo.mjs
// Each mark is a stack of waves on a 100 x 100 box, its ink centered on (50, 50).
import { writeFileSync } from "node:fs";

const accent = "#f29a3f", light = "#f3efe8", dark = "#15171c";
// Wave shapes, a = half the height: sine (one cycle, up then down) and hill (a crest at the center).
const shapes = {
  sine: (cx, cy, w, a) => { const x0 = cx - w / 2; return `M${x0} ${cy} Q${x0 + w / 4} ${cy - 2 * a} ${cx} ${cy} T${x0 + w} ${cy}`; },
  hill: (cx, cy, w, a) => { const x0 = cx - w / 2, t = w * .2; return `M${x0} ${cy + a} C${x0 + t} ${cy + a} ${cx - t} ${cy - a} ${cx} ${cy - a} S${x0 + w - t} ${cy + a} ${x0 + w} ${cy + a}`; },
};
// Waves bottom to top, [width, height, stroke width, top?]; gap between one wave's ink and the next.
const marks = {
  "tapered-gentle": { shape: "sine", gap: 4.5, waves: [[76, 18, 8], [54, 15, 6.5], [34, 12, 5, true]] },
  hills: { shape: "hill", gap: 4, waves: [[80, 16, 8.5], [58, 14, 6.5], [36, 12, 5, true]] },
};
const round = n => Math.round(n * 100) / 100;
function paths(mark, ink, top) {
  let edge = 0, low = Infinity, high = -Infinity;
  const placed = mark.waves.map(([w, h, sw, isTop], i) => {
    const half = h / 2 + sw / 2, cy = i ? edge - mark.gap - half : 0;
    edge = cy - half; low = Math.min(low, cy - half); high = Math.max(high, cy + half);
    return { w, h, sw, cy, color: isTop ? top : ink };
  });
  const shift = 50 - (low + high) / 2;
  return placed.map(p => `<path d="${shapes[mark.shape](50, round(p.cy + shift), p.w, p.h / 2).replace(/-?\d+\.\d+/g, v => round(+v))}" fill="none" stroke="${p.color}" stroke-width="${p.sw}" stroke-linecap="round" stroke-linejoin="round"/>`).join("");
}
const svg = (body, size = 100) => `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 100 100">${body}</svg>\n`;
for (const [name, mark] of Object.entries(marks)) {
  const base = name === "tapered-gentle" ? "mark" : `mark-${name}`;
  writeFileSync(`${base}-on-dark.svg`, svg(paths(mark, light, accent)));   // for dark grounds
  writeFileSync(`${base}-on-light.svg`, svg(paths(mark, dark, accent)));   // for light grounds
  writeFileSync(`${base}-mono.svg`, svg(paths(mark, "currentColor", "currentColor"))); // one color, from the context
  // The app icon: the mark on a dark rounded square, at 74% of the tile.
  // The macOS app icon grid: an 824 px square (corner radius about 185) centered in 1024, so the
  // icon sits at the size of the system's own; the mark keeps its place within the square.
  writeFileSync(`${base}-app-icon.svg`, svg(`<rect x="9.77" y="9.77" width="80.47" height="80.47" rx="18.1" fill="${dark}"/><g transform="translate(20.23 20.23) scale(.5955)">${paths(mark, light, accent)}</g>`, 1024));
}
