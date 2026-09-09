#!/usr/bin/env python3
"""Create (or reuse) a Gitea release and attach files to it.

Plain stdlib against the Gitea API - no third-party action, so nothing here
depends on a JS runtime being available inside the runner image.

    gitea_release.py <api-base> <tag> <body-file> <asset>...
"""

import json
import os
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path


def api(url, token, data=None, headers=None, method=None):
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Authorization": f"token {token}",
                                          **(headers or {})})
    return urllib.request.urlopen(req, timeout=120)


def ensure_release(base, tag, body, token):
    payload = json.dumps({
        "tag_name": tag, "name": f"GhostHID {tag}",
        "body": body, "draft": False, "prerelease": False,
    }).encode()
    try:
        r = api(f"{base}/releases", token, payload,
                {"Content-Type": "application/json"})
        rel = json.load(r)
        print(f"created release {tag}")
        return rel["id"]
    except urllib.error.HTTPError as e:
        if e.code not in (409, 422):
            sys.stderr.write(e.read().decode(errors="replace") + "\n")
            raise
    # Already there: reuse it, so re-running a release job attaches rather
    # than failing outright.
    rel = json.load(api(f"{base}/releases/tags/{tag}", token))
    print(f"reusing existing release {tag}")
    return rel["id"]


def delete_existing(base, rel_id, name, token):
    """Remove any asset already attached under this name, so re-running a
    release job replaces assets instead of piling up duplicates."""
    assets = json.load(api(f"{base}/releases/{rel_id}/assets", token))
    for a in assets:
        if a.get("name") == name:
            api(f"{base}/releases/{rel_id}/assets/{a['id']}", token,
                method="DELETE")
            print(f"  removed stale {name} (id {a['id']})")


def attach(base, rel_id, path, token):
    boundary = uuid.uuid4().hex
    name = Path(path).name
    delete_existing(base, rel_id, name, token)
    body = b"".join([
        f"--{boundary}\r\n".encode(),
        f'Content-Disposition: form-data; name="attachment"; filename="{name}"\r\n'.encode(),
        b"Content-Type: application/octet-stream\r\n\r\n",
        Path(path).read_bytes(),
        f"\r\n--{boundary}--\r\n".encode(),
    ])
    api(f"{base}/releases/{rel_id}/assets?name={name}", token, body,
        {"Content-Type": f"multipart/form-data; boundary={boundary}"})
    print(f"  attached {name} ({len(body) // 1024} KB)")


def main(argv):
    if len(argv) < 4:
        sys.exit(__doc__)
    base, tag, body_file, assets = argv[1].rstrip("/"), argv[2], argv[3], argv[4:]
    token = os.environ.get("GITEA_TOKEN") or os.environ.get("TOKEN")
    if not token:
        sys.exit("GITEA_TOKEN is not set")

    body = Path(body_file).read_text() if Path(body_file).is_file() else "See CHANGELOG.md."
    rel_id = ensure_release(base, tag, body.strip() or "See CHANGELOG.md.", token)
    for a in assets:
        attach(base, rel_id, a, token)
    print(f"release {tag} ready with {len(assets)} asset(s)")


if __name__ == "__main__":
    main(sys.argv)
