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

## Avatar mode — the duck is Claude's face

Everything above maps lifecycle events to reactions the *device* was
configured to have. Avatar mode inverts that: Claude picks the face, in the
moment, based on how the work is actually going — the duck stops being a
status lamp and becomes Claude's face. While it's on, the cycle, the stop
face, and the `/claude?e=` takeovers all stand down; the display shows
whatever `/avatar?face=` last said (plus idle blinks, because a face that
never blinks is a photograph). The badges still run — they carry lifecycle,
the face carries mood.

```sh
curl 'http://clawducky.local/avatar?on=1'            # possess
curl 'http://clawducky.local/avatar?face=thinking'   # Claude wears a face
curl 'http://clawducky.local/avatar?on=0'            # release
```

Also a toggle in the web UI. Setting a face with the mode off is a polite
no-op — a stale hook can't repossess a duck you've released.

The one autonomous behaviour left is staleness: a face nobody has refreshed
in 15 minutes sags to `sleepy`. Hooks are fire-and-forget and sessions die;
a duck stuck on `angry` all night because a session got killed is worse
than no avatar at all.

`tools/duckface` is the write half:

```sh
tools/duckface disapproval
CLAWD_HOST=<duck-ip> tools/duckface eureka   # if mDNS won't cross your VLANs
```

It only fires when an `en*` interface holds a `192.168.x` address, so a
tethered, VPN'd, or coffee-shop laptop never sprays curls off the home LAN —
a VPN into the house rides a `utun` interface and deliberately fails the
guard. Always exits 0.

To let Claude actually use it, drop something like this in a `CLAUDE.md`
(global for everywhere, or per-project) and allow the command:

> When avatar mode is on (check `/state`), express how the work is going by
> running `~/working/clawducky/tools/duckface <face>` when your read on it
> changes: `thinking` while forming an opinion, `focused` mid-edit,
> `debugging` when tests fight back, `eureka`/`proud` when they stop,
> `overwhelmed`, `smug`, and `disapproval` as deserved. Change the face when
> something is worth expressing, not every turn.

The faces for the job, beyond the original eight: `thinking`, `focused`,
`debugging`, `smug`, `eureka`, `overwhelmed`, `staring`, `proud`. Some carry
a corner accent glyph (sweat drop, spark, thinking dots) in the top-left —
the badges own the top-right, so the two never collide.

## Corner badges

Two chips in the top-right, and the only thing on the device that never takes
the screen: the cycle keeps showing whatever it was showing, badges just add a
corner. That separation is the whole point. Anything that says what Claude is
doing by *picking a face* ends up fighting the rotation for the display, and
faces stop meaning anything once `dead` sometimes means "rate limited".

Left slot is activity, right slot is trouble — position carries the category,
so the glyph only has to carry the detail:

```sh
curl 'http://clawducky.local/badge?act=working'     # ∙∙● green dots, animated
curl 'http://clawducky.local/badge?act=idle'        # left slot off
curl 'http://clawducky.local/badge?warn=attention'  # ! amber — your turn
curl 'http://clawducky.local/badge?warn=limit'      # ✕ red — rate limited
curl 'http://clawducky.local/badge?warn=none'       # right slot off
```

Both slots lapse on their own after 10 minutes. Hooks are fire-and-forget and
some of them get dropped, and a chip still claiming "working" an hour later is
worse than no chip at all.

Wiring, in `~/.claude/settings.json` — `UserPromptSubmit` lights the activity
slot, `Stop` clears it, and `Notification` is the one that says look up:

```json
"UserPromptSubmit": [
  { "hooks": [{ "type": "command", "async": true,
                "command": "(curl -sm2 'http://clawducky.local/badge?act=working&warn=none' >/dev/null 2>&1 &)" }] }
],
"Stop": [
  { "hooks": [{ "type": "command", "async": true,
                "command": "(curl -sm2 'http://clawducky.local/badge?act=idle' >/dev/null 2>&1 &)" }] }
],
"Notification": [
  { "matcher": "permission_prompt|idle_prompt",
    "hooks": [{ "type": "command", "async": true,
                "command": "(curl -sm2 'http://clawducky.local/badge?warn=attention' >/dev/null 2>&1 &)" }] }
],
"StopFailure": [
  { "matcher": "rate_limit",
    "hooks": [{ "type": "command", "async": true,
                "command": "(curl -sm2 'http://clawducky.local/badge?warn=limit' >/dev/null 2>&1 &)" }] }
]
```

`UserPromptSubmit` clears the warning as a side effect, which is the right
place for it: the condition a `permission_prompt` describes is "Claude is
waiting on you", and you answering it *is* the clear.

The full-screen `/claude?e=` states are still there and still take over the
display — they're just not what the hooks drive any more. Hit one by hand when
you want the whole crab to react.
