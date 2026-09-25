#!/usr/bin/env python
"""Push the store listing's text to the Pebble appstore dashboard.

`pebble publish` uploads a release, its notes and its screenshots, but the
title, description and links live on the app itself and the tool has no
call for them. The dashboard does: it trades the developer's Firebase token
for a session cookie and PATCHes the app as a form. This does the same with
the text in store-listing.md.

    ~/.local/share/uv/tools/pebble-tool/bin/python tools/store.py        # show what the store has
    ~/.local/share/uv/tools/pebble-tool/bin/python tools/store.py push   # send the listing

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

    def patch(self, fields):
        boundary = "----headway" + uuid.uuid4().hex
        body = b""
        for key, value in fields.items():
            body += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n" % (boundary, key, value)).encode()
        body += ("--%s--\r\n" % boundary).encode()
        return self.call("PATCH", "/api/dashboard/apps/" + APP_ID, body,
                         {"Content-Type": "multipart/form-data; boundary=" + boundary})


def main(argv):
    dash = Dashboard()
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
