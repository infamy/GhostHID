"""Synchronous WebSocket client for a GhostHID device.

Usage:

    from ghosthid import GhostHID

    with GhostHID("192.168.4.1", token="ghosthid") as g:
        g.type("hello world")
        g.key("ENTER")
        g.chord("CTRL", "ALT", "DELETE")
        g.mouse_move(100, 50)
        g.click("left")

The context manager matters: on exit it releases every held key and button.
Leaving a modifier stuck on the target machine is the failure mode this whole
project has to avoid, so the client defends against it too rather than relying
solely on the device-side watchdog.
"""

from __future__ import annotations

import json
import threading
import time
from typing import Iterable

try:
    from websockets.sync.client import connect as _ws_connect
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "GhostHID needs the 'websockets' package (pip install websockets)"
    ) from exc


class GhostHIDError(RuntimeError):
    """Raised when the device rejects a command or the link fails."""


# Keep well under the device's watchdog timeout (750 ms) so a healthy but idle
# link is never mistaken for a dead one.
_PING_INTERVAL = 0.25


class GhostHID:
    def __init__(
        self,
        host: str = "192.168.4.1",
        port: int = 80,
        token: str = "ghosthid",
        timeout: float = 5.0,
    ) -> None:
        self.url = f"ws://{host}:{port}/ws"
        self._token = token
        self._timeout = timeout
        self._ws = None
        self._lock = threading.Lock()
        self._pinger: threading.Thread | None = None
        self._stop = threading.Event()

    # --- connection --------------------------------------------------------

    def connect(self) -> "GhostHID":
        self._ws = _ws_connect(self.url, open_timeout=self._timeout)
        reply = self._request({"type": "auth", "token": self._token})
        if not reply.get("ok"):
            self.close()
            raise GhostHIDError("authentication rejected - wrong token?")

        self._stop.clear()
        self._pinger = threading.Thread(target=self._ping_loop, daemon=True)
        self._pinger.start()
        return self

    def close(self) -> None:
        self._stop.set()
        if self._pinger is not None:
            self._pinger.join(timeout=1.0)
            self._pinger = None
        if self._ws is not None:
            # Best effort: tell the device to let go before we drop the link.
            try:
                self.release_all()
            except Exception:
                pass
            try:
                self._ws.close()
            finally:
                self._ws = None

    def __enter__(self) -> "GhostHID":
        return self.connect()

    def __exit__(self, *exc) -> None:
        self.close()

    # --- plumbing ----------------------------------------------------------

    def _ping_loop(self) -> None:
        while not self._stop.wait(_PING_INTERVAL):
            try:
                self._send({"type": "ping"})
            except Exception:
                return  # link is gone; the device watchdog takes over

    def _send(self, payload: dict) -> None:
        if self._ws is None:
            raise GhostHIDError("not connected")
        with self._lock:
            self._ws.send(json.dumps(payload))

    def _request(self, payload: dict) -> dict:
        if self._ws is None:
            raise GhostHIDError("not connected")
        with self._lock:
            self._ws.send(json.dumps(payload))
            raw = self._ws.recv(timeout=self._timeout)
        try:
            return json.loads(raw)
        except (TypeError, ValueError):
            raise GhostHIDError(f"unparseable reply: {raw!r}") from None

    # --- keyboard ----------------------------------------------------------

    def key_down(self, key: str) -> None:
        self._send({"type": "key", "key": key, "pressed": True})

    def key_up(self, key: str) -> None:
        self._send({"type": "key", "key": key, "pressed": False})

    def key(self, key: str, hold: float = 0.01) -> None:
        """Press and release a single key."""
        self.key_down(key)
        time.sleep(hold)
        self.key_up(key)

    def chord(self, *keys: str, hold: float = 0.03) -> None:
        """Press keys together, then release in reverse order.

        Reverse order matters: releasing a modifier before the key it modifies
        can deliver the bare keypress to the target.
        """
        pressed: list[str] = []
        try:
            for k in keys:
                self.key_down(k)
                pressed.append(k)
            time.sleep(hold)
        finally:
            for k in reversed(pressed):
                self.key_up(k)

    def type(self, text: str) -> None:
        """Type an ASCII string. Non-ASCII characters are skipped by the device."""
        self._send({"type": "text", "text": text})

    # --- mouse -------------------------------------------------------------

    def mouse_move(self, dx: int, dy: int) -> None:
        """Relative movement. Any magnitude is fine; the device splits it into
        multiple HID reports (one report carries only -127..127)."""
        self._send({"type": "mouse_move", "dx": int(dx), "dy": int(dy)})

    def mouse_move_absolute(self, x: float, y: float) -> None:
        """Put the pointer at a fraction of the target's desktop.

        (0, 0) is the top-left corner, (1, 1) the bottom-right. Fractions
        rather than pixels because the device cannot learn the target's
        resolution, and a fraction stays correct when it changes.

        Unlike ``mouse_move`` this is not rescaled by the target's pointer
        acceleration, so the pointer lands exactly here — which is what makes
        it possible to know where the pointer is without seeing the screen.
        """
        self._send({"type": "mouse_abs", "x": float(x), "y": float(y)})

    def mouse_button(self, button: str, pressed: bool) -> None:
        self._send({"type": "mouse_button", "button": button, "pressed": pressed})

    def click(self, button: str = "left", hold: float = 0.02) -> None:
        self.mouse_button(button, True)
        time.sleep(hold)
        self.mouse_button(button, False)

    def scroll(self, delta: int) -> None:
        """Positive scrolls up."""
        self._send({"type": "mouse_wheel", "delta": int(delta)})

    def pan(self, delta: int) -> None:
        """Horizontal wheel. Positive scrolls right."""
        self._send({"type": "mouse_wheel", "delta": 0, "pan": int(delta)})

    # --- safety ------------------------------------------------------------

    def release_all(self) -> None:
        self._send({"type": "release_all"})
