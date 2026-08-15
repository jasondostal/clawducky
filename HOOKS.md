# Claude Code hooks

clawducky can act as a physical Claude Code status lamp and a live usage
readout. Both are driven by hooks. Requires station mode (see
`wifi_credentials.h.example`) so the device is reachable on your LAN.

If `clawducky.local` doesn't resolve — mDNS doesn't cross VLANs — use the IP and
set `CLAWD_HOST` for the feeder.

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
curl 'http://clawducky.local/claude?e=working'
curl 'http://clawducky.local/claude?e=done'
curl 'http://clawducky.local/state'   # includes claude/sta/ip fields
```

## Hooks config

Merge into `~/.claude/settings.json`. Each command backgrounds a curl with a
2s cap, so a powered-off duck never slows Claude down.

```json
{
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawducky.local/claude?e=working' >/dev/null 2>&1 &)" }] }
    ],
    "Stop": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawducky.local/claude?e=done' >/dev/null 2>&1 &)" }] }
    ],
    "Notification": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawducky.local/claude?e=waiting' >/dev/null 2>&1 &)" }] }
    ],
    "SessionEnd": [
      { "hooks": [{ "type": "command", "command": "(curl -sm2 'http://clawducky.local/claude?e=idle' >/dev/null 2>&1 &)" }] }
    ]
  }
}
```

Event mapping rationale:

- `UserPromptSubmit` → working: fires once per prompt. `PreToolUse` would
  also work but fires on every tool call — chatty and adds latency.
- `Stop` → done: Claude finished its turn. The duck celebrates, then
  auto-idles after 6 seconds.
- `Notification` → waiting: Claude is blocked on your input (permission
  prompt or idle nudge). The duck stares at you until you deal with it.
- `SessionEnd` → idle: session closed, back to normal eyes.

Manual control from the web UI (any `/cmd` press) also clears hook-driven
state, so the duck never gets stuck in a mode.

## Usage meter

`tools/clawducky-usage.py` feeds the `/meter` display: context-window occupancy
from the session transcript, plus the 5-hour and 7-day quota percentages from
Anthropic's OAuth usage endpoint.

The device never sees a credential. It receives three percentages and renders
them — it has no idea Anthropic exists. That matters because the duck serves an
unauthenticated HTTP UI to the whole LAN and its flash can be dumped over USB
by anyone holding it.

Add to the same `Stop` block as the status hook:

```json
"Stop": [
  { "hooks": [
      { "type": "command", "command": "(curl -sm2 'http://clawducky.local/claude?e=done' >/dev/null 2>&1 &)" },
      { "type": "command", "command": "(~/path/to/clawducky/tools/clawducky-usage.py >/dev/null 2>&1 &)" }
  ] }
]
```

`Stop` is the right and only necessary trigger: it is the moment usage actually
changes. Set `CLAWD_HOST` if the duck isn't at the default address.

Behaviour worth knowing:

- **Exits 0 no matter what.** A non-zero exit from a hook surfaces as an error
  in your session, so every failure path is swallowed. A desk toy must never
  break the editor.
- **Bounded at ~1.5s worst case** when the duck is unreachable, and it
  backgrounds anyway, so being off the network costs you nothing.
- **Quota is cached 60s.** Those numbers crawl; there's no reason to hit the
  API every turn. Context is always read fresh from the transcript.
- **The token** is read from the macOS Keychain (`Claude Code-credentials`),
  the same one Claude Code already maintains, so refresh is handled for you.

Check what it would send without sending it:

```sh
tools/clawducky-usage.py --print
```

## Reacting to a finished turn

The feeder appends `stop=1` to the push it already sends, so a finished turn
costs one request rather than two. What the duck *does* about that is
configured on the duck, not in the hook — the hook reports an event, the device
owns the reaction:

```sh
curl 'http://clawducky.local/stopface?mode=random'              # a different ticked face each turn
curl 'http://clawducky.local/stopface?mode=fixed&f=disapproval' # always the same one
curl 'http://clawducky.local/stopface?mode=none'                # just update the numbers
```

Also settable from the `// on stop` picker in the web UI. `random` is the fun
one: a fixed face stops registering after a day, but a rotating one reads as
the duck having moods.

`random` draws from the same corner ticks as the cycle, so unticking a face
takes it out of both — with no face ticked, a finished turn just updates the
numbers. `fixed` is an explicit choice and shows whether or not it's ticked.

Whatever face a turn produces also becomes the auto-cycle's home face, so a
cycling duck alternates between the usage screen and the most recent reaction
rather than a stale default.

`GET /meter/cycle?on=1&sec=10&random=1` cycles between face and usage, picking
a fresh random face each time round.
