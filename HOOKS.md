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
