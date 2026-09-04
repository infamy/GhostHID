"""Command-line entry point:  python -m ghosthid ..."""

from __future__ import annotations

import argparse
import sys

from .client import GhostHID, GhostHIDError


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ghosthid", description="Control a GhostHID device.")
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--token", default="ghosthid")

    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("type", help="type a string")
    s.add_argument("text")

    s = sub.add_parser("key", help="press a key or chord, e.g. CTRL+ALT+DELETE")
    s.add_argument("combo")

    s = sub.add_parser("move", help="move the mouse")
    s.add_argument("dx", type=int)
    s.add_argument("dy", type=int)

    s = sub.add_parser("moveto", help="put the pointer at a fraction of the screen (0..1)")
    s.add_argument("x", type=float)
    s.add_argument("y", type=float)

    s = sub.add_parser("click", help="click a mouse button")
    s.add_argument("button", nargs="?", default="left",
                   choices=["left", "right", "middle"])

    s = sub.add_parser("scroll", help="scroll the wheel (positive = up)")
    s.add_argument("delta", type=int)

    sub.add_parser("release", help="release all held keys and buttons")

    args = p.parse_args(argv)

    try:
        with GhostHID(args.host, args.port, args.token) as g:
            if args.cmd == "type":
                g.type(args.text)
            elif args.cmd == "key":
                keys = [k for k in args.combo.replace("-", "+").split("+") if k]
                g.chord(*keys) if len(keys) > 1 else g.key(keys[0])
            elif args.cmd == "move":
                g.mouse_move(args.dx, args.dy)
            elif args.cmd == "moveto":
                g.mouse_move_absolute(args.x, args.y)
            elif args.cmd == "click":
                g.click(args.button)
            elif args.cmd == "scroll":
                g.scroll(args.delta)
            elif args.cmd == "release":
                g.release_all()
    except (GhostHIDError, OSError) as exc:
        print(f"ghosthid: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
