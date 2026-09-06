# GhostHID case (Waveshare ESP32-S3-LCD-1.47)

A parametric two-part case for the board GhostHID runs on. That board is a
**USB-A stick**: the male USB-A plug is part of the PCB and pokes out one short
end, the 1.47" LCD is on the front, and the ESP32-S3 module + side-actuated
BOOT/RST buttons are on the back / edges.

![assembled preview](preview-assembled.png)

- **bottom** — cradles the back (component) side. Four **locating bosses** drop
  the board onto its Ø2.0 mounting holes and set its height; the USB end is open
  for the plug (and card, if fitted); side holes line up with BOOT/RST; a
  logo-shaped pocket in the floor takes the inlay.
- **logo** — the **GhostHID ghost mark** (traced from `assets/mark.svg` — the
  square-wave "pulse" hem and pill eyes), centred on the back as an inlay that
  fills the pocket flush, printed in a second colour.
- **top** — a bezel that friction-clips on and frames the LCD active area.

![the two-colour back](preview-back-logo.png)

## Dimensions — from Waveshare's structural drawing

Everything is taken from `waveshare-structural-drawing.pdf` (in this folder) and
the STEP model, so the fit should be right the first time:

| feature            | value                                   |
|--------------------|-----------------------------------------|
| PCB                | 36.37 × 20.33 × 1.6 mm                   |
| LCD module glass   | 1.46 mm proud of the front              |
| LCD active area    | 32.35 × 17.39 mm, centred               |
| mounting holes     | 4 × Ø2.0, grid 13.28 × 29.3, ~3.53 in from every edge |
| USB-A plug base    | 16.32 mm wide (the USB end is left open) |
| overall body       | ~5.1 mm thick (excl. the USB plug)       |

Two values are the only judgement calls, both flagged `[tune]` in the `.scad`:

- `back_comp_h` (2.7) — height of the tallest part on the back. The drawing puts
  the body at ~5.1 mm total, which leaves ~2.0–2.7 mm here depending on the exact
  module. If the top bezel won't seat, raise it 0.5 mm and reprint the bottom.
- `btn_x` (30) — where the BOOT/RST buttons sit along the length. If the side
  holes miss, nudge this and reprint.

## Print it

Pre-exported STLs are in this folder, or regenerate after editing:

```sh
openscad -D 'part="bottom"' -o ghosthid-bottom.stl ghosthid-case.scad
openscad -D 'part="logo"'   -o ghosthid-logo.stl   ghosthid-case.scad   # second colour
openscad -D 'part="top"'    -o ghosthid-top.stl    ghosthid-case.scad
openscad -D 'part="both"'   -o preview.png         ghosthid-case.scad
```

**Two-colour bottom, the easy way:** open **`ghosthid-bottom-2color.3mf`** — one
file, two parts already aligned ("case bottom" and "logo"), each carrying its own
material. Just pick a filament for each part and print floor-down. Regenerate with:

```sh
openscad --enable=lazy-union --backend=Manifold \
         -D 'part="3mf"' -o ghosthid-bottom-2color.3mf ghosthid-case.scad
```

Prefer STLs? Load `ghosthid-bottom.stl` + `ghosthid-logo.stl` together (they share
coordinates, so they line up) and assign the logo the second filament. The inlay
is `logo_depth` (0.8 mm) deep. No multi-material printer? The pocket still reads
fine in one colour, or paint it in the slicer.

The top bezel (`ghosthid-top.stl`) is a separate single-colour print.

| setting        | value                                                    |
|----------------|----------------------------------------------------------|
| material       | PLA (both colours)                                       |
| layer height   | 0.16–0.20 mm                                              |
| walls          | 3 perimeters                                              |
| infill         | 20 %                                                      |
| supports       | none — both parts print flat                              |
| orientation    | bottom: floor down · top: **window-face down** (the `part="top"` output is already flipped for this) |

The halves are a friction fit (`lip_h` / `lip_t`); nudge `clr` / `lip_t` and
reprint the top only if it's tight or loose.

## Note for boards with headers soldered

This is drawn for the **bare-pad** board (no pin headers). If your board has the
male headers fitted, the pins protrude from the back and this case won't close —
raise `back_comp_h` to clear them, or leave them off.

There is no official Waveshare case and no community model carries the GhostHID
mark, which is why this is built from scratch off the structural drawing.
