// GhostHID case - Waveshare ESP32-S3-LCD-1.47 (USB-A stick form factor)
// ============================================================================
// A two-part case for the board GhostHID runs on. That board is a USB-A "stick":
// the male USB-A plug is part of the PCB and pokes out one short end, the 1.47"
// LCD is on the front face, and the ESP32-S3 + side BOOT/RST buttons are on the
// back / edges.
//
//   - bottom : cradles the back (component) side. Four bosses land on the M2
//              nuts the board carries on its back; M2x4 screws through the floor
//              hold it. USB end open; slots in the side walls for the BOOT/RST
//              buttons; the GHOST logo inlaid in the floor.
//   - top    : a bezel that clips on and frames the LCD active area.
//
// EVERY dimension here was measured off Waveshare's STEP model
// (esp32-s3-lcd-1_47_asm.stp) and cross-checked against the structural drawing
// and the LBS147TC-IF15 LCD datasheet. Board frame used throughout: x along the
// length from the NON-USB end, y across, z from the PCB *front* face (LCD = +).
//
//   PCB            36.39 x 20.34 x 0.8, R2.8 corners
//   LCD stack      4.25 proud of the PCB front (2.8 PMMA spacer + 1.45 glass);
//                  module 36.28 x 19.39 at y 0.47..19.87, runs to x 37.01 - i.e.
//                  0.62 PAST the USB end of the PCB (folded FPC)
//   active area    32.35 x 17.39 at x 1.00..33.35, y 1.47..18.86 (NOT centred:
//                  1.0 from the non-USB end, 2.9 from the USB end)
//   mount holes    O2.25 with SMT M2 nuts (SMTSO-M2-2.5ET, O3.5) standing 2.51
//                  off the back: (1.98,3.53) (1.98,16.81) (33.98,2.01) (33.98,18.33)
//                  - 32.0 pitch along, 13.28 across at the non-USB end but 16.32
//                  at the USB end
//   back parts     TF slot 2.90 deep (tallest), USB shell 2.19, nuts 2.51
//   BOOT/RST       bodies x 27.07..31.62, z -2.6..-0.4, protrude 1.19 past the
//                  PCB edge on both long sides
//   USB-A plug     12.0 wide x 4.5 tall (z -2.99..+1.51), starts at x 33.6
//   pin headers    NOT fitted (bare pads). If yours are, they reach 9.3 below
//                  the back and this case won't close.
//
// The only judgement calls are clearances, flagged [tune].
//
//   openscad -D 'part="bottom"' -o ghosthid-bottom.stl ghosthid-case.scad
//   openscad -D 'part="top"'    -o ghosthid-top.stl    ghosthid-case.scad
// ============================================================================

/* [What to build] */
part = "both";                 // ["bottom":Bottom shell, "top":Top bezel, "both":Assembled preview]

/* [Board - measured off the STEP] */
board_l      = 36.39;          // PCB length, excludes the USB-A plug
board_w      = 20.34;          // PCB width
pcb_t        = 0.8;            // PCB thickness (it is a thin board, not 1.6)
lcd_h        = 4.25;           // LCD stack proud of the PCB front (spacer + glass)
lcd_end_x    = 37.01;          // LCD module reaches this far along x (past the PCB end)
back_h       = 3.3;            // [tune] cavity depth behind the PCB (TF slot is 2.90)

/* [LCD active area - from the LCD datasheet, board coords] */
aa_x0        = 1.00;           // active-area start from the non-USB end
aa_y0        = 1.47;           // active-area start from the near long edge
aa_l         = 32.35;
aa_w         = 17.39;
win_margin   = 0.4;            // window opened this much beyond the active area
win_r        = 2.8;            // window corner radius - the active area's corners are ~R2.4

/* [Mounting - 4x SMT M2 nuts on the back] */
holes        = [[1.98, 3.53], [1.98, 16.81], [33.98, 2.01], [33.98, 18.33]];
nut_h        = 2.51;           // nut face stands this far off the PCB back
boss_d       = 4.6;            // boss landing under each nut
// The two non-USB-end bosses carry a socket the O3.5 nut drops into, with a
// funnel to guide it. Two sockets fix position and rotation, so the board is
// located even with no screws fitted (a loose board makes the BOOT paddle
// erratic). The USB-end nuts sit too close to the USB shell for a socket.
locate_nuts  = true;
sock_d       = 3.8;            // socket for the O3.5 nut body
sock_h       = 1.0;            // socket depth (nut is 2.5 tall)
sock_wall    = 0.9;
screw_d      = 2.3;            // M2 clearance hole through floor + boss
screw_cb_d   = 4.2;            // counterbore for the M2 pan head (outside the floor)
screw_cb_h   = 1.2;            // pan heads sit ~0.4 proud; deeper would thin the floor under them

/* [BOOT / RST - side-actuated buttons on both long edges] */
// Both switches sit at the same x; BOOT is on the y=0 edge, RST on the y=20.34
// edge (Waveshare's back-side photo, confirmed against asymmetric parts in the
// STEP). Reference on the real board: screen toward you, USB plug pointing up ->
// BOOT is on the RIGHT edge. If a board revision swaps them, flip boot_side.
buttons      = true;
boot_side    = 0;              // [0:BOOT on the y=0 wall, 1:BOOT on the far wall]
btn_x        = 29.35;          // switch centre from the non-USB end
btn_z        = -1.5;           // switch body centre below the PCB front face
act_z        = -1.7;           // actuator tip centre (tip is 1.8 x 0.8 at y -1.19)
act_y        = -1.19;          // actuator tip protrudes this far past the PCB edge
// RST: a plain slot - it only needs a fingernail, and harder-to-hit is fine.
btn_slot_l   = 5.6;            // slot through the side wall (body is 4.55 x 2.2)
btn_slot_h   = 3.2;
// The actuators poke 0.79 into the wall zone, so without a way down the board
// has to be forced past solid wall (and forced back out - that breaks
// switches). Each switch gets an entry channel: a groove in the inner face of
// the wall from the switch up through the lip. Wide enough for the actuator
// plus the board's play before the nuts find their sockets.
chan_w       = 3.8;            // actuator 1.8 + up to 1.4 of play + margin
chan_dx      = 0.3;            // play is asymmetric (1.0 toward the USB end, 0.4 away)
chan_depth   = 1.3;            // into the wall from its inner face (actuator 0.79 + 0.4 play)
// BOOT: a flush paddle in the wall - the firmware uses this button, so it has
// to be easy to hit. A 1.0 mm plate cut free on three sides, hinged along its
// non-USB end so the actuator sits near the free end (good leverage). The
// hinge is a vertical strip, so it flexes along the print layers, not across
// them. Press anywhere; the dimple marks the actuator.
boot_paddle  = true;
pad_x0       = 22.5;           // paddle from here (the hinge end) ...
pad_x1       = 31.5;           // ... to here, board x. Actuator at 29.35.
pad_t        = 1.0;            // plate thickness (outer part of the wall)
pad_gap      = 0.5;            // cut around the free edges
pad_zb       = 1.2;            // paddle bottom, case z (floor is 0..plate)
hinge_t      = 0.7;            // root thinned to this over hinge_l
hinge_l      = 1.5;
dimple_r     = 2.0;            // finger-locating dimple over the actuator
dimple_d     = 0.3;

/* [USB-A end] */
usb_w        = 12.0;           // plug shell width
usb_top      = 1.51;           // plug shell top above the PCB front
usb_clr      = 0.6;            // opening clearance around the shell

/* [Case build] */
plate        = 1.6;            // floor and lid thickness
wall         = 2.0;            // side wall (the lip lives inside this thickness)
clr          = 0.4;            // clearance around the PCB
end_clr      = 1.0;            // extra at the USB end for the LCD overhang (0.62)
top_clr      = 0.3;            // air gap over the LCD glass
corner_r     = 3.0;            // outer corner radius
split_up     = 0.5;            // parting plane this far above the PCB front
lip_h        = 3.0;            // lip on the bottom that nests into the top
lip_t        = 0.9;            // lip thickness (inner part of the wall)
lip_clr      = 0.10;           // rebate clearance around the lip
snap         = true;           // detent snaps so the lid actually clicks shut
snap_r       = 0.5;            // detent bump radius (pocket must stay inside the 0.95 lid skin)
snap_pos     = [[12.1, 6], [34.6, 4]];  // [board x centre, length]: between the glow slots, clear of the paddle
vent         = false;          // heat-shed slots over the SoC (off: keeps the logo face clean)

/* [RGB glow] */
// The board's RGB LED is a side-firing bead at the middle of the non-USB end,
// on the back, pointing out through the PCB edge. Waveshare's transparent
// acrylic LCD spacer (0..2.8 above the PCB front) carries the glow around the
// display. Slots let it out: two on the end wall - one in front of the LED
// itself, one at acrylic height - and two per long side at acrylic height.
// They cut the lid's skin and the bottom's lip, which is why the snaps moved.
glow         = true;
glow_za      = 0.3;            // slot bottom, above the parting plane (acrylic spans -0.5..2.3)
glow_zb      = 2.0;            // slot top
glow_side_x  = [[1.6, 7.6], [16.1, 21.6]];   // board-x spans of the side slots
glow_end_w   = 7.0;            // end-wall slots, centred on the LED
led_z        = -1.5;           // LED lens centre below the PCB front (bead sits on the back)
led_slot_h   = 1.6;

/* [Ghost logo] */
// The GhostHID mark (assets/mark.svg): a ghost whose hem is a square wave - a
// scalloped tail and a digital pulse train are the same silhouette. Printed as a
// two-colour INLAY: the shell carries a ghost-shaped pocket and the logo body
// (part="logo") fills it flush, in the second filament. Centred on the back.
logo         = true;
logo_len     = 22.0;           // ghost height, along the length of the case
logo_depth   = 0.8;            // inlay depth = colour-2 thickness (~4 layers of PLA at 0.2)

$fn = 56;

// ---- derived ---------------------------------------------------------------
cav_l   = board_l + clr + end_clr;         // asymmetric: LCD overhangs the USB end
cav_w   = board_w + 2*clr;
outer_l = cav_l + 2*wall;
outer_w = cav_w + 2*wall;

seat_z  = plate + back_h;                  // PCB back face
pcb_top = seat_z + pcb_t;                  // PCB front face (board z = 0)
split_z = pcb_top + split_up;              // parting plane
snap_z  = split_z + lip_h*0.5;             // height of the snap detents on the lip
top_h   = lcd_h + top_clr - split_up + plate;  // bezel height above the split
total_h = split_z + top_h;
lip_out = wall - lip_t;                    // lip outer face, inset from the outer skin

// board-coord -> case-coord helpers (board origin at its non-USB, near corner)
function bx(x) = wall + clr + x;
function by(y) = wall + clr + y;
function bz(z) = pcb_top + z;

win_l = aa_l + 2*win_margin;
win_w = aa_w + 2*win_margin;
win_x = bx(aa_x0 - win_margin);
win_y = by(aa_y0 - win_margin);

// ---- helpers ---------------------------------------------------------------
module rrect(l, w, r) {                     // 2D rounded rectangle, origin at 0,0
    r2 = min(r, min(l,w)/2);
    hull() for (x=[r2, l-r2], y=[r2, w-r2]) translate([x,y]) circle(r=r2);
}

// The GhostHID mark, traced from assets/mark.svg. Built in the SVG's 64x64 box
// (converted to y-up), then centred on the origin and scaled so it stands `h`
// tall. The hem is the exact square-wave of the logo; the eyes are the logo's
// tall pill holes (they become the case colour in the inlay). Dome points +Y.
module ghost_mark(h) {
    // SVG y-down converted to y-up: v = 64 - y. Body spans u[15,49], v[9,55].
    na  = 40;                                   // dome arc segments
    arc = [ for (i=[0:na]) let(t = 180 - 180*i/na) [32 + 17*cos(t), 38 + 17*sin(t)] ];
    hem = [[49,9],[49,16],[46,16],[46,9],[41,9],[41,16],[39,16],[39,9],
           [33,9],[33,16],[29,16],[29,9],[26,9],[26,16],[23,16],[23,9]];
    pts = concat([[15,9]], arc, hem);           // left wall, dome, right wall, square-wave hem
    s = h/46;                                    // 46 = body height (v 9..55)
    scale([s,s]) translate([-32,-32])
        difference() {
            polygon(pts);
            translate([24,30]) rrect(5,10,1.5);  // left eye  (SVG x24 y24 w5 h10)
            translate([35,30]) rrect(5,10,1.5);  // right eye (SVG x35 y24 w5 h10)
        }
}

// The ghost centred on the floor, standing along the length of the case (dome
// toward the USB end). Shared by the pocket cut and the inlay body so they match.
module logo_2d() {
    translate([outer_l/2, outer_w/2]) rotate([0,0,-90]) ghost_mark(logo_len);
}

// The inlay body: fills the floor pocket flush, printed in the second colour.
module logo_body() {
    linear_extrude(logo_depth) logo_2d();
}

// Brand colours for the preview renders (does not affect printed geometry).
c_case  = "#4b5763";   // gunmetal (reads on a dark ground)
c_ghost = "#00e5ff";   // GhostHID cyan

// Snap detents: rounded ridges on the outer face of the bottom's lip (both long
// sides) that click into matching pockets in the top. `g` grows the shape, which
// turns the bump (g=0) into the slightly larger pocket the top subtracts.
module snaps(g=0) {
    for (p = snap_pos)
        for (sy = [lip_out, outer_w - lip_out])
            translate([bx(p[0]), sy, snap_z])
                rotate([0,90,0])
                    cylinder(h = p[1] + 2*g, r = snap_r + g, center = true, $fn = 32);
}

// Rounded slots through a wall. yslot runs along x through a long wall (axis
// along y, starting at y0 for len); xslot runs along y through an end wall.
module yslot(x0, x1, y0, len, z0, h) {
    translate([0, y0, z0 + h/2]) rotate([-90,0,0])
        hull() for (x = [x0 + h/2, x1 - h/2])
            translate([x, 0, 0]) cylinder(h = len, d = h, $fn = 24);
}
module xslot(y0, y1, x0, len, z0, h) {
    translate([x0, 0, z0 + h/2]) rotate([0,90,0])
        hull() for (y = [y0 + h/2, y1 - h/2])
            translate([0, y, 0]) cylinder(h = len, d = h, $fn = 24);
}

// Glow slots, subtracted from both halves (each only keeps what lands in it).
module glow_cut() {
    z0 = split_z + glow_za;  h = glow_zb - glow_za;
    for (s = glow_side_x) for (y0 = [-1, outer_w - wall - 1])           // long sides
        yslot(bx(s[0]), bx(s[1]), y0, wall + 2, z0, h);
    ey0 = (outer_w - glow_end_w)/2;
    xslot(ey0, ey0 + glow_end_w, -1, wall + 2, z0, h);                  // end, acrylic height
    xslot(ey0, ey0 + glow_end_w, -1, wall + 2, bz(led_z) - led_slot_h/2, led_slot_h);  // end, the LED itself
}

// The USB end opening, subtracted from both halves. In the bottom it runs from
// the floor right through the lip, so the plug shell can pass straight down (and
// back up) - a lip strip over it would trap the board. In the top it stops just
// over the plug shell, so the lid's end wall stays closed above it.
module usb_cut(z_top) {
    w = usb_w + 2*usb_clr;
    translate([outer_l - wall - 0.1, (outer_w - w)/2, plate])
        cube([wall + 1.1, w, z_top - plate]);
}
usb_top_z = bz(usb_top + usb_clr);             // plug shell clearance height

// One mounting boss: rises from the floor to meet the face of the board's SMT
// nut; the M2 screw comes up through floor + boss into the nut. The nut spans
// the PCB, so an M2x4 stops inside it - longer would poke the LCD spacer.
module boss(x, y, socket = false) {
    h = back_h - nut_h;                        // boss top = nut face
    translate([bx(x), by(y), plate - 0.01]) {
        cylinder(h = h + 0.01, d = boss_d);
        if (socket) difference() {
            cylinder(h = h + sock_h + 0.01, d = sock_d + 2*sock_wall);
            translate([0,0,h]) cylinder(h = sock_h + 0.1, d = sock_d);
            translate([0,0,h + sock_h - 0.5])                       // funnel
                cylinder(h = 0.51, d1 = sock_d, d2 = sock_d + 1.0);
        }
    }
}
module screw_hole(x, y) {
    translate([bx(x), by(y), -1]) cylinder(h = seat_z + 1, d = screw_d);
    translate([bx(x), by(y), -0.01]) cylinder(h = screw_cb_h + 0.01, d = screw_cb_d);
}

// Slot through a side wall for the RST button (body pokes 1.19 past the PCB
// edge, so the slot also gives it room). Rounded ends, runs along x.
module btn_slot(near) {
    y0 = near ? -1 : outer_w - wall - 1;
    translate([bx(btn_x), y0, bz(btn_z)])
        rotate([-90,0,0])
            hull() for (dx = [-1, 1])
                translate([dx*(btn_slot_l - btn_slot_h)/2, 0, 0])
                    cylinder(h = wall + 2, d = btn_slot_h, $fn = 32);
}

// Entry channel for one switch: a groove in the inner face of the side wall,
// from just under the switch up through the lip, so the actuator rides down
// into place (and back out) instead of being forced past solid wall.
module entry_channel(near) {
    z0 = bz(btn_z) - btn_slot_h/2 - 0.3;
    y0 = near ? wall - chan_depth : outer_w - wall - 0.01;
    translate([bx(btn_x + chan_dx) - chan_w/2, y0, z0])
        cube([chan_w, chan_depth + 0.01, split_z + lip_h + 1 - z0]);
}

// Everything subtracted from the y=0 wall to turn a patch of it into the BOOT
// paddle. Mirror it across the case for boot_side=1. Pieces:
//   pocket : inner (wall - pad_t) of the wall removed behind the plate, so the
//            actuator (at y -1.19, i.e. inside the wall zone) has room and the
//            plate is free to move. It runs all the way up through the lip -
//            that is the switch's entry channel, and it leaves nothing that
//            would have to print as a bridge. The lid's skin covers it.
//   root   : plate thinned to hinge_t over hinge_l at the hinge end.
//   gaps   : bottom / free-end cuts through the plate; the top edge stops
//            pad_gap short of the parting plane, so the lid's edge is the gap.
//   dimple : shallow spherical finger-locator over the actuator.
module boot_cut() {
    x0 = bx(pad_x0);  x1 = bx(pad_x1);
    zt = split_z - pad_gap;                    // paddle top
    // pocket + root
    translate([x0, pad_t, pad_zb])      cube([x1 + pad_gap - x0, wall - pad_t + 0.01, split_z + lip_h + 1 - pad_zb]);
    translate([x0, hinge_t, pad_zb])    cube([hinge_l, pad_t - hinge_t + 0.01, zt - pad_zb]);
    // gaps through the plate
    translate([x0, -1, zt])             cube([x1 + pad_gap - x0, pad_t + 1, pad_gap + 0.01]);
    translate([x0, -1, pad_zb - pad_gap]) cube([x1 + pad_gap - x0, pad_t + 1, pad_gap]);
    translate([x1, -1, pad_zb - pad_gap]) cube([pad_gap, pad_t + 1, zt - pad_zb + 2*pad_gap]);
    // dimple
    translate([bx(btn_x), dimple_d - dimple_r, bz(act_z)]) sphere(r = dimple_r, $fn = 48);
}
module boot_paddle_cut() {
    if (boot_side == 0) boot_cut();
    else translate([0, outer_w, 0]) mirror([0,1,0]) boot_cut();
}

// ---- bottom shell ----------------------------------------------------------
module bottom() {
    difference() {
        union() {
            linear_extrude(split_z) rrect(outer_l, outer_w, corner_r);   // solid block
            translate([0,0,split_z])                                     // alignment lip
                linear_extrude(lip_h)
                    difference() {
                        offset(-lip_out) rrect(outer_l, outer_w, corner_r);
                        offset(-wall)    rrect(outer_l, outer_w, corner_r);
                    }
            if (snap) snaps(0);                                          // detent bumps on the lip
        }
        // cavity: everything above the floor, full footprint, hollowed out
        translate([wall, wall, plate])
            cube([cav_l, cav_w, back_h + pcb_t + split_up + lip_h + 1]);
        usb_cut(split_z + lip_h + 1);         // through the lip - the plug must pass
        if (buttons) {
            btn_slot(boot_side != 0);                     // RST: slot on the other wall
            entry_channel(boot_side != 0);                //      + its entry channel
            if (boot_paddle) boot_paddle_cut();           // BOOT: paddle (its pocket is the channel)
            else { btn_slot(boot_side == 0); entry_channel(boot_side == 0); }
        }
        for (h = holes) screw_hole(h[0], h[1]);
        if (glow) glow_cut();
        // heat-shed slots over the ESP32-S3 (off by default)
        if (vent)
            for (i=[-1,1])
                translate([bx(22), by(board_w/2) + i*3.5 - 0.6, -1])
                    cube([8, 1.2, plate+2]);
        // Logo pocket in the OUTSIDE floor (the inlay body fills it flush).
        if (logo)
            translate([0,0,-0.02]) linear_extrude(logo_depth + 0.02) logo_2d();
    }
    // mounting bosses (added after the cavity so they stand inside it); the
    // non-USB-end pair get the locating sockets
    difference() {
        for (h = holes) boss(h[0], h[1], locate_nuts && h[0] < board_l/2);
        for (h = holes) screw_hole(h[0], h[1]);
        usb_cut(split_z + lip_h + 1);
    }
}

// ---- top bezel -------------------------------------------------------------
module top_solid() {
    difference() {
        translate([0,0,split_z]) linear_extrude(top_h) rrect(outer_l, outer_w, corner_r);
        // hollow the underside for the LCD stack
        translate([wall, wall, split_z - 0.01])
            cube([cav_l, cav_w, top_h - plate + 0.01]);
        // rebate so the bottom's lip nests inside
        translate([0,0,split_z-0.01])
            linear_extrude(lip_h + lip_clr + 0.01)
                difference() {
                    offset(-lip_out + lip_clr) rrect(outer_l, outer_w, corner_r);
                    offset(-wall - 0.01)       rrect(outer_l, outer_w, corner_r);
                }
        // LCD window through the top face (rounded like the panel's corners)
        translate([win_x, win_y, split_z + top_h - plate - 0.01])
            linear_extrude(plate + 0.1) rrect(win_l, win_w, win_r);
        // snap pockets the bottom's detent bumps click into
        if (snap) snaps(0.25);
        if (glow) glow_cut();
        usb_cut(usb_top_z);
    }
}

// ---- layout ----------------------------------------------------------------
if (part == "bottom") {
    bottom();
} else if (part == "logo") {
    logo_body();                                                     // second-colour inlay STL
} else if (part == "3mf") {
    // The whole case as one file: three colour-tagged objects (bottom, logo,
    // top) laid flat and ready to slice. Export with --enable=lazy-union so they
    // stay distinct parts; pick a filament for each (case colour for bottom+top,
    // an accent for the logo). Bottom+logo interlock in place; the top sits
    // beside them, window-face down.
    color("Gainsboro") bottom();
    color("SkyBlue")   logo_body();
    color("Gainsboro") translate([0, outer_w*2 + 8, total_h]) rotate([180,0,0]) top_solid();
} else if (part == "top") {
    translate([0, outer_w, total_h]) rotate([180,0,0]) top_solid();   // window-face down
} else if (part == "back") {
    // preview only: bottom + inlay, flipped floor-up so the logo is visible
    translate([outer_l/2, outer_w/2, 0]) rotate([0,180,0])
        translate([-outer_l/2, -outer_w/2, 0]) {
            color(c_case)  bottom();
            color(c_ghost) logo_body();
        }
} else if (part == "closed") {
    // preview only: the finished, closed case
    color(c_case)  bottom();
    color(c_ghost) logo_body();
    color(c_case)  top_solid();
} else {
    // "both": exploded preview
    color(c_case)  bottom();
    color(c_ghost) logo_body();
    color(c_case)  translate([0,0,9]) top_solid();
}
