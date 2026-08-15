#!/usr/bin/env python3
"""Push Claude Code usage to a clawd-mochi display.

Designed to be run from a Claude Code Stop hook: it reads the session
transcript for context-window occupancy, asks the Anthropic OAuth usage
endpoint for the 5h and 7d quota percentages, and POSTs all three to the
device's /meter endpoint.

The device stores no credentials and knows nothing about Anthropic -- it just
renders whatever percentages it is handed.

Usage:
    clawd-usage.py                 # hook mode: reads hook JSON on stdin
    clawd-usage.py --print         # print what would be sent, don't send
    CLAWD_HOST=192.168.1.50 clawd-usage.py   # if mDNS does not resolve

Exits 0 unconditionally. A desk toy must never break the editor: every failure
path is swallowed, because a non-zero exit from a hook surfaces as an error in
the session.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

HOST = os.environ.get("CLAWD_HOST", "clawd.local")
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CACHE = "/tmp/.clawd-usage-cache.json"
CACHE_TTL = 60          # seconds; quota crawls, no need to poll every turn
API_TIMEOUT = 3        # the usage endpoint, cached so it is rarely hit
PUSH_TIMEOUT = 1.5     # the crab is on the LAN; do not stall a turn for it

# Context windows by model family. The transcript's `model` field does not
# carry the [1m] suffix, so the 1M variant is detected from settings and from
# the observed total (see context_pct).
WINDOWS = {"haiku": 200_000, "sonnet": 200_000, "opus": 200_000, "fable": 200_000}
DEFAULT_WINDOW = 200_000


def oauth_token():
    """Read the Claude Code OAuth token from the macOS Keychain."""
    try:
        blob = subprocess.run(
            ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
            capture_output=True, text=True, timeout=5,
        ).stdout.strip()
        d = json.loads(blob)
        return (d.get("claudeAiOauth") or d).get("accessToken")
    except Exception:
        return None


def quota():
    """(five_hour_pct, seven_day_pct), cached briefly. (None, None) on failure."""
    try:
        st = os.stat(CACHE)
        if time.time() - st.st_mtime < CACHE_TTL:
            with open(CACHE) as f:
                c = json.load(f)
            return c.get("session"), c.get("week")
    except Exception:
        pass

    tok = oauth_token()
    if not tok:
        return None, None
    req = urllib.request.Request(
        USAGE_URL,
        headers={"Authorization": f"Bearer {tok}",
                 "anthropic-beta": "oauth-2025-04-20"},
    )
    try:
        with urllib.request.urlopen(req, timeout=API_TIMEOUT) as r:
            d = json.load(r)
    except Exception:
        return None, None

    ses = round((d.get("five_hour") or {}).get("utilization") or 0)
    wk = round((d.get("seven_day") or {}).get("utilization") or 0)
    try:
        with open(CACHE, "w") as f:
            json.dump({"session": ses, "week": wk}, f)
    except Exception:
        pass
    return ses, wk


def configured_window():
    """1M if the user's settings pin a [1m] model, else the default."""
    try:
        with open(os.path.expanduser("~/.claude/settings.json")) as f:
            if "[1m]" in (json.load(f).get("model") or ""):
                return 1_000_000
    except Exception:
        pass
    return None


def context_pct(transcript):
    """Occupancy of the context window, as a percentage."""
    if not transcript or not os.path.exists(transcript):
        return None
    total = model = None
    try:
        with open(transcript) as f:
            for line in f:
                try:
                    m = (json.loads(line).get("message") or {})
                    u = m.get("usage")
                    if u:
                        total = (u.get("input_tokens", 0)
                                 + u.get("cache_creation_input_tokens", 0)
                                 + u.get("cache_read_input_tokens", 0))
                        model = m.get("model") or model
                except Exception:
                    continue
    except Exception:
        return None
    if total is None:
        return None

    window = configured_window()
    if window is None:
        window = next((v for k, v in WINDOWS.items() if k in (model or "")),
                      DEFAULT_WINDOW)
    # Self-correcting: exceeding the assumed window proves it was the larger
    # variant, since the session would otherwise have compacted.
    if total > window:
        window = 1_000_000
    return max(0, min(100, round(total / window * 100)))


def newest_transcript():
    """Fallback when not invoked from a hook.

    Walks newest-first rather than taking the single newest file: the most
    recently touched transcript may belong to a session that has not produced
    an assistant message yet, and so carries no usage to read.
    """
    import glob
    paths = sorted(glob.glob(os.path.expanduser("~/.claude/projects/*/*.jsonl")),
                   key=os.path.getmtime, reverse=True)
    for p in paths[:8]:
        if context_pct(p) is not None:
            return p
    return None


def main():
    transcript = None
    if not sys.stdin.isatty():
        try:
            transcript = json.loads(sys.stdin.read() or "{}").get("transcript_path")
        except Exception:
            pass
    transcript = transcript or newest_transcript()

    ctx = context_pct(transcript)
    ses, wk = quota()

    parts = [f"{k}={v}" for k, v in
             (("ctx", ctx), ("session", ses), ("week", wk)) if v is not None]
    if not parts:
        return

    # stop=1 tells the device a turn just finished; what it does about that is
    # the device's business, configured on the device.
    url = f"http://{HOST}/meter?" + "&".join(parts + ["stop=1"])
    if "--print" in sys.argv:
        print(url)
        return
    try:
        urllib.request.urlopen(url, timeout=PUSH_TIMEOUT).read()
    except Exception:
        pass   # crab asleep, off the network, or elsewhere: not our problem


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
