#!/usr/bin/env python
"""Push the store listing's text to the Pebble appstore dashboard.

`pebble publish` uploads a release, its notes and its screenshots, but the
title, description and links live on the app itself and the tool has no
call for them. The dashboard does: it trades the developer's Firebase token
for a session cookie and PATCHes the app as a form. This does the same with
the text in store-listing.md.

    ~/.local/share/uv/tools/pebble-tool/bin/python tools/store.py        # show what the store has
    ~/.local/share/uv/tools/pebble-tool/bin/python tools/store.py push   # send the listing
    ~/.local/share/uv/tools/pebble-tool/bin/python tools/store.py shots DIR   # replace the screenshots

The screenshots in DIR are named <platform>_<order>_<anything>.png. The new
ones go up first, a platform at a time, and only once they're there are the
old ones deleted, so the page is never left without.

Needs the pebble tool's Python, for its saved login.
"""
import http.cookiejar
import json
import os
import sys
import urllib.request
import uuid

from pebble_tool.account import get_account

APP_ID = "28a589e21b934ab6beac7b12"
BASE = "https://appstore-api.repebble.com"
LISTING = os.path.join(os.path.dirname(__file__), "..", "store-listing.md")


def section(text, name):
    """The body of a '## name' section, paragraphs joined onto one line each."""
    start = text.index("## " + name)
    body = text[start:].split("\n", 1)[1]
    end = body.find("\n## ")
    body = body if end < 0 else body[:end]
    return "\n\n".join(" ".join(p.split()) for p in body.strip().split("\n\n"))


def listing():
    text = open(LISTING).read()
    return {
        "title": section(text, "Title"),
        "description": section(text, "Description"),
        "source": "https://github.com/sybenx/headway",
        "website": "",
        "visibility": "listed",
    }


class Dashboard:
    def __init__(self):
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        token = get_account(auth_provider="firebase").get_access_token()
        self.call("POST", "/api/auth/firebase/session", json.dumps({"idToken": token}).encode(),
                  {"Content-Type": "application/json"})

    def call(self, method, path, data=None, headers=None):
        req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers or {})
        try:
            with self.opener.open(req) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def app(self):
        status, body = self.call("GET", "/api/dashboard/apps/" + APP_ID)
        if status != 200:
            sys.exit("dashboard refused: %s %s" % (status, body[:200]))
        return json.loads(body)["app"]

    def patch(self, fields, files=()):
        """fields as form values; files as (field name, path) PNG parts."""
        boundary = "----headway" + uuid.uuid4().hex
        body = b""
        for key, value in fields.items():
            body += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n" % (boundary, key, value)).encode()
        for key, path in files:
            body += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: image/png\r\n\r\n"
                     % (boundary, key, os.path.basename(path))).encode() + open(path, "rb").read() + b"\r\n"
        body += ("--%s--\r\n" % boundary).encode()
        return self.call("PATCH", "/api/dashboard/apps/" + APP_ID, body,
                         {"Content-Type": "multipart/form-data; boundary=" + boundary})


def shots(dash, folder):
    by = {}
    for name in sorted(os.listdir(folder)):
        if name.endswith(".png") and "_" in name:
            by.setdefault(name.split("_")[0], []).append(os.path.join(folder, name))
    for platform, paths in sorted(by.items()):
        old = next((a["screenshots"] for a in dash.app()["assets"] if a["platform"] == platform), [])
        status, body = dash.patch(listing(), [("screenshots_" + platform, p) for p in paths])
        now = next((a["screenshots"] for a in dash.app()["assets"] if a["platform"] == platform), [])
        added = [p for p in now if p not in old]
        if status != 200 or len(added) != len(paths):
            sys.exit("%s: upload failed (%s, %d of %d there): %s" % (platform, status, len(added), len(paths), body[:200]))
        if old:
            status, body = dash.patch(dict(listing(), deletedScreenshots=json.dumps({platform: old})))
            left = next((a["screenshots"] for a in dash.app()["assets"] if a["platform"] == platform), [])
            if status != 200 or left != added:
                sys.exit("%s: old ones not removed (%s): %s" % (platform, status, body[:200]))
        print(platform, len(added), "up,", len(old), "replaced")
    return 0


def main(argv):
    dash = Dashboard()
    if argv[1:2] == ["shots"] and len(argv) == 3:
        return shots(dash, argv[2])
    app = dash.app()
    if argv[1:] == ["push"]:
        want = listing()
        status, body = dash.patch(want)
        if status != 200:
            sys.exit("patch failed: %s %s" % (status, body[:300]))
        app = dash.app()
        same = app["title"] == want["title"] and app["description"] == want["description"]
        print("pushed;", "the store reads as the listing" if same else "but the store reads differently")
        return 0 if same else 1
    print("title:", app["title"])
    print("release:", app["latest_release"]["version"], app["latest_release"]["published_date"])
    print("screenshots:", {a["platform"]: len(a["screenshots"]) for a in app["assets"]})
    print("description:\n" + app["description"])
    print("\nnotes:\n" + (app["latest_release"].get("release_notes") or ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
