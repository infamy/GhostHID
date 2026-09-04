"""Minimal end-to-end demo. Run with the target's text editor focused."""

import time
from ghosthid import GhostHID

with GhostHID("192.168.4.1", token="ghosthid") as g:
    g.type("Hello from GhostHID")
    g.key("ENTER")

    # A square, then back where we started.
    for dx, dy in ((120, 0), (0, 120), (-120, 0), (0, -120)):
        g.mouse_move(dx, dy)
        time.sleep(0.15)

    g.scroll(3)
    time.sleep(0.2)
    g.scroll(-3)
