#!/usr/bin/env python3
"""Create (or reuse) a GitHub release and attach files to it.

Plain stdlib against the GitHub REST API, so it runs on the Gitea runner without
a JS action or the gh CLI. Idempotent: an asset with the same name is replaced.

    GH_TOKEN=... GH_REPO=owner/name github_publish.py <tag> <body-file> <asset>...

Env:
    GH_TOKEN   a GitHub PAT with `repo` scope (required)
    GH_REPO    owner/name (required)
"""

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"


def req(url, token, data=None, method=None, ctype=None):
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "ghosthid-release",
    }
    if ctype:
        headers["Content-Type"] = ctype
    r = urllib.request.Request(url, data=data, method=method, headers=headers)
    return urllib.request.urlopen(r, timeout=180)


def ensure_release(repo, tag, body, token):
    payload = json.dumps({
        "tag_name": tag, "name": f"GhostHID {tag}",
        "body": body, "draft": False, "prerelease": False,
    }).encode()
    try:
        rel = json.load(req(f"{API}/repos/{repo}/releases", token, payload,
                            "POST", "application/json"))
        print(f"created GitHub release {tag}")
        return rel
    except urllib.error.HTTPError as e:
        if e.code != 422:  # 422 == already exists
            sys.stderr.write(e.read().decode(errors="replace") + "\n")
            raise
    rel = json.load(req(f"{API}/repos/{repo}/releases/tags/{tag}", token))
    # Refresh the notes/title on the existing release.
    req(f"{API}/repos/{repo}/releases/{rel['id']}", token,
        json.dumps({"name": f"GhostHID {tag}", "body": body}).encode(),
        "PATCH", "application/json")
    print(f"reusing existing GitHub release {tag}")
    return rel


def clear_existing(repo, rel_id, name, token):
    assets = json.load(req(f"{API}/repos/{repo}/releases/{rel_id}/assets", token))
    for a in assets:
        if a.get("name") == name:
            req(f"{API}/repos/{repo}/releases/assets/{a['id']}", token, method="DELETE")
            print(f"  removed stale {name}")


def upload(repo, rel_id, path, token):
    name = Path(path).name
    clear_existing(repo, rel_id, name, token)
    data = Path(path).read_bytes()
    req(f"{UPLOADS}/repos/{repo}/releases/{rel_id}/assets?name={name}",
        token, data, "POST", "application/octet-stream")
    print(f"  uploaded {name} ({len(data) // 1024} KB)")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    tag, body_file, assets = argv[1], argv[2], argv[3:]
    token = os.environ.get("GH_TOKEN")
    repo = os.environ.get("GH_REPO")
    if not token or not repo:
        sys.exit("GH_TOKEN and GH_REPO must be set")
    body = Path(body_file).read_text() if Path(body_file).is_file() else "See CHANGELOG.md."
    rel = ensure_release(repo, tag, body.strip() or "See CHANGELOG.md.", token)
    for a in assets:
        upload(repo, rel["id"], a, token)
    print(f"GitHub release {tag} ready with {len(assets)} asset(s)")


if __name__ == "__main__":
    main(sys.argv)
