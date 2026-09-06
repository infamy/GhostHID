// GhostHID case - Waveshare ESP32-S3-LCD-1.47 (USB-A stick form factor)
// ============================================================================
// A two-part case for the board GhostHID runs on. That board is a USB-A "stick":
// the male USB-A plug is part of the PCB and pokes out one short end, the 1.47"
// LCD is on the front face, and the ESP32-S3 module + side BOOT/RST buttons are
// on the back / edges.
//
//   - bottom : cradles the back (component) side. Four locating bosses drop the
//              board onto its Ø2.0 mounting holes. USB end open; side holes for
//              the BOOT/RST buttons; the GHOST logo debossed on the floor.
//   - top    : a bezel that friction-clips on and frames the LCD active area.
//
// EVERY dimension here is taken from Waveshare's own structural drawing
// (ESP32-S3-LCD-1.47.pdf / .stp): PCB 36.37 x 20.33 x 1.6, LCD module glass
// 1.46 proud, active area 32.35 x 17.39, four Ø2.0 mounting holes on a
// 13.28 x 29.3 grid (~3.53 in from every edge), USB-A plug base 16.32 wide.
// The only judgement calls are clearances and the exact BOOT/RST button height,
// all flagged [tune] - print the bottom, drop the bare board in, adjust, reprint.
//
//   openscad -D 'part="bottom"' -o ghosthid-bottom.stl ghosthid-case.scad
//   openscad -D 'part="top"'    -o ghosthid-top.stl    ghosthid-case.scad
// ============================================================================

/* [What to build] */
part = "both";                 // ["bottom":Bottom shell, "top":Top bezel, "both":Assembled preview]

/* [Board - from the structural drawing] */
board_l      = 36.37;          // PCB length, excludes the USB-A plug
board_w      = 20.33;          // PCB width
pcb_t        = 1.6;            // PCB thickness
lcd_glass_h  = 1.46;           // LCD module glass proud of the PCB front
back_comp_h  = 2.7;            // [tune] tallest part on the back (module ~2.4-3.2). Body total ~5.1.

/* [LCD active area - from the drawing, centred on the board] */
aa_l         = 32.35;          // active-area length
aa_w         = 17.39;          // active-area width
win_margin   = 0.4;            // window opened this much beyond the active area

/* [Mounting holes - 4x Ø2.0 on a 13.28 x 29.3 grid] */
hole_d       = 2.0;
hole_inset_l = 3.55;           // hole centre from each short end  (36.37-29.3)/2
hole_inset_w = 3.53;           // hole centre from each long edge  (20.33-13.28)/2
peg_d        = 1.8;            // locating peg through the hole (under Ø2.0)
boss_d       = 4.4;            // boss shoulder the PCB rests on

/* [BOOT / RST - side-actuated buttons on the long edges near the USB end] */
buttons      = true;
btn_x        = 30.0;           // [tune] button centre from the non-USB end
btn_hole_d   = 3.4;            // access hole in the side wall

/* [Case build] */
wall         = 1.6;            // outer wall / floor / lid thickness
clr          = 0.4;            // clearance around the PCB
top_clr      = 0.3;            // air gap over the LCD glass
corner_r     = 2.5;            // outer corner radius
lip_h        = 3.2;            // overlap between the two halves
lip_t        = 0.9;            // lip wall thickness
snap         = true;           // detent snaps so the lid actually clicks shut
snap_r       = 0.7;            // detent bump radius (how far it protrudes / clicks)
snap_len     = 7;              // detent length along the case
usb_open_w   = 17.4;           // width of the open slot at the USB end (clears the 16.32 plug base)
vent         = false;          // heat-shed slots over the module (off: keeps the logo face clean)

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
cav_l   = board_l + 2*clr;
cav_w   = board_w + 2*clr;
outer_l = cav_l + 2*wall;
outer_w = cav_w + 2*wall;

seat_z  = wall + back_comp_h;              // PCB rests here (top of the back cavity)
split_z = seat_z + pcb_t;                  // parting plane = top face of the PCB
snap_z  = split_z + lip_h*0.55;            // height of the snap detents on the lip
top_h   = lcd_glass_h + top_clr + wall;    // bezel height above the split
total_h = split_z + top_h;

// board-coord -> case-coord helpers (board origin at its non-USB, near corner)
function bx(x) = wall + clr + x;
function by(y) = wall + clr + y;

win_l = aa_l + 2*win_margin;
win_w = aa_w + 2*win_margin;
win_x = bx((board_l - aa_l)/2 - win_margin);
win_y = by((board_w - aa_w)/2 - win_margin);

hole_x = [hole_inset_l, board_l - hole_inset_l];
hole_y = [hole_inset_w, board_w - hole_inset_w];

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
    for (sx = [board_l*0.30, board_l*0.70])
        for (sy = [wall, outer_w - wall])
            translate([bx(sx), sy, snap_z])
                rotate([0,90,0])
                    cylinder(h = snap_len + 2*g, r = snap_r + g, center = true, $fn = 32);
}

// The USB end opening, subtracted from both halves so the plug (and its wide
// base) exits freely while sturdy corner posts remain.
module usb_cut() {
    translate([outer_l - wall - 0.1, (outer_w - usb_open_w)/2, -1])
        cube([wall + 1.1, usb_open_w, total_h + 2]);
}

// One PCB-locating boss: a shoulder the board rests on, with a peg through the
// mounting hole.
// The peg stops flush with the PCB top face: the mounting holes sit under the
// LCD module on the front, so a peg poking proud would foul the glass.
module boss(x, y) {
    translate([bx(x), by(y), wall]) {
        cylinder(h = back_comp_h, d = boss_d);                 // shoulder
        cylinder(h = back_comp_h + pcb_t - 0.1, d = peg_d);    // locating peg, flush
    }
}

// ---- bottom shell ----------------------------------------------------------
module bottom() {
    difference() {
        union() {
            linear_extrude(split_z) rrect(outer_l, outer_w, corner_r);   // solid block
            translate([0,0,split_z])                                     // alignment lip
                linear_extrude(lip_h)
                    difference() {
                        offset(-wall) rrect(outer_l, outer_w, corner_r);
                        offset(-wall-lip_t) rrect(outer_l, outer_w, corner_r);
                    }
            if (snap) snaps(0);                                          // detent bumps on the lip
        }
        // cavity: everything above the floor, full footprint, hollowed out
        translate([wall, wall, wall])
            cube([cav_l, cav_w, back_comp_h + pcb_t + lip_h + 1]);
        usb_cut();
        // side access holes for the BOOT / RST buttons (one per long edge)
        if (buttons)
            for (s = [0, 1])
                translate([bx(btn_x), s==0 ? -1 : outer_w - wall - 1,
                           seat_z + pcb_t/2])
                    rotate([-90,0,0]) cylinder(h = wall + 2, d = btn_hole_d);
        // heat-shed slots over the ESP32-S3 module (off by default)
        if (vent)
            for (i=[-1,1])
                translate([bx(24), by(board_w/2) + i*3.5 - 0.6, -1])
                    cube([8, 1.2, wall+2]);
        // Logo pocket in the OUTSIDE floor (the inlay body fills it flush).
        if (logo)
            translate([0,0,-0.02]) linear_extrude(logo_depth + 0.02) logo_2d();
    }
    // locating bosses (added after the cavity so they stand inside it)
    difference() {
        for (x = hole_x, y = hole_y) boss(x, y);
        usb_cut();
    }
}

// ---- top bezel -------------------------------------------------------------
module top_solid() {
    difference() {
        translate([0,0,split_z]) linear_extrude(top_h) rrect(outer_l, outer_w, corner_r);
        // hollow the underside for the LCD glass
        translate([wall, wall, split_z - 0.01])
            cube([cav_l, cav_w, lcd_glass_h + top_clr + 0.02]);
        // rebate so the bottom's lip nests inside
        translate([0,0,split_z-0.01])
            linear_extrude(lip_h+0.02)
                difference() {
                    offset(-wall+0.15) rrect(outer_l, outer_w, corner_r);
                    offset(-wall-lip_t-0.15) rrect(outer_l, outer_w, corner_r);
                }
        // LCD window through the top face
        translate([win_x, win_y, split_z + lcd_glass_h + top_clr - 0.01])
            cube([win_l, win_w, wall + 0.1]);
        // snap pockets the bottom's detent bumps click into
        if (snap) snaps(0.35);
        usb_cut();
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
