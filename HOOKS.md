# Claude Code → Clawd status hooks

This fork adds a `/claude` endpoint so the crab acts as a physical Claude Code
status lamp. Requires station mode (see `wifi_credentials.h.example`) so the
crab is reachable at `http://clawd.local` on the LAN.

## Endpoint

`GET /claude?e=<event>`

| event     | display                                            |
|-----------|----------------------------------------------------|
| `working` | focused squish eyes, cycling dots, "working"       |
| `waiting` | eyes glancing around, "your turn!"                 |
| `done`    | happy wiggle, "done!" — auto-returns to idle in 6s |
| `error`   | red screen, flat unimpressed eyes                  |
| `idle`    | back to normal eyes                                |

Test from the Mac:

```sh
curl 'http://clawd.local/claude?e=working'
curl 'http://clawd.local/claude?e=done'
curl 'http://clawd.local/state'   # includes claude/sta/ip fields
```

## Hooks config

Merge into `~/.claude/settings.json`. Each command backgrounds a curl with a
2s cap, so a powered-off crab never slows Claude down.

```json
{
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawd.local/claude?e=working' >/dev/null 2>&1 &)" }] }
    ],
    "Stop": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawd.local/claude?e=done' >/dev/null 2>&1 &)" }] }
    ],
    "Notification": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawd.local/claude?e=waiting' >/dev/null 2>&1 &)" }] }
    ],
    "SessionEnd": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawd.local/claude?e=idle' >/dev/null 2>&1 &)" }] }
    ]
  }
}
```

Event mapping rationale:

- `UserPromptSubmit` → working: fires once per prompt. `PreToolUse` would
  also work but fires on every tool call — chatty and adds latency.
- `Stop` → done: Claude finished its turn. The crab celebrates, then
  auto-idles after 6 seconds.
- `Notification` → waiting: Claude is blocked on your input (permission
  prompt or idle nudge). The crab stares at you until you deal with it.
- `SessionEnd` → idle: session closed, back to normal eyes.

Manual control from the web UI (any `/cmd` press) also clears hook-driven
state, so the crab never gets stuck in a mode.

## Usage meter

`tools/clawd-usage.py` feeds the `/meter` display: context-window occupancy
from the session transcript, plus the 5-hour and 7-day quota percentages from
Anthropic's OAuth usage endpoint.

The device never sees a credential. It receives three percentages and renders
them — it has no idea Anthropic exists. That matters because the crab serves an
unauthenticated HTTP UI to the whole LAN and its flash can be dumped over USB
by anyone holding it.

Add to the same `Stop` block as the status hook:

```json
"Stop": [
  { "hooks": [
      { "type": "command", "command": "(curl -sm2 'http://clawd.local/claude?e=done' >/dev/null 2>&1 &)" },
      { "type": "command", "command": "(~/working/clawd-mochi/tools/clawd-usage.py >/dev/null 2>&1 &)" }
  ] }
]
```

`Stop` is the right and only necessary trigger: it is the moment usage actually
changes. Set `CLAWD_HOST` if the crab isn't at the default address.

Behaviour worth knowing:

- **Exits 0 no matter what.** A non-zero exit from a hook surfaces as an error
  in your session, so every failure path is swallowed. A desk toy must never
  break the editor.
- **Bounded at ~1.5s worst case** when the crab is unreachable, and it
  backgrounds anyway, so being off the network costs you nothing.
- **Quota is cached 60s.** Those numbers crawl; there's no reason to hit the
  API every turn. Context is always read fresh from the transcript.
- **The token** is read from the macOS Keychain (`Claude Code-credentials`),
  the same one Claude Code already maintains, so refresh is handled for you.

Check what it would send without sending it:

```sh
tools/clawd-usage.py --print
```

## Reacting to a finished turn

The feeder appends `stop=1` to the push it already sends, so a finished turn
costs one request rather than two. What the crab *does* about that is
configured on the crab, not in the hook — the hook reports an event, the device
owns the reaction:

```sh
curl 'http://clawd.local/stopface?mode=random'              # a different face each turn
curl 'http://clawd.local/stopface?mode=fixed&f=disapproval' # always the same one
curl 'http://clawd.local/stopface?mode=none'                # just update the numbers
```

Also settable from the `// on stop` picker in the web UI. `random` is the fun
one: a fixed face stops registering after a day, but a rotating one reads as
the crab having moods.

Whatever face a turn produces also becomes the auto-cycle's home face, so a
cycling crab alternates between the usage screen and the most recent reaction
rather than a stale default.

`GET /meter/cycle?on=1&sec=10&random=1` cycles between face and usage, picking
a fresh random face each time round.
