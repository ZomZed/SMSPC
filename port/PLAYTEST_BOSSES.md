# Boss fights: playtest notes (w5)

The scripted bots (w5) played six of the eight fights to the end on real game logic: defeat, Shine, save prompt, back in Delfino Plaza.
Two are not confirmed yet and need a human run: **Wiggler** and **final Bowser**.
Details of every run are in `port/BOOTLOG.md`, section "Boss fights (w5)".

## How to start a boss directly and keep a log

Open a Command Prompt in the `SMS_PC` folder and set the knobs before starting the game.
The debug warp starts the stage on a fresh save, right after the Nintendo logo (your own memory card is not touched by the warp itself; answer "No" to saving if you do not want the warp's save written).

```
cd /d <project>/SMS_PC
set SMS_PORT_WARP=4,2
set SMS_PORT_BOSSTRACE=1
set SMS_PORT_TRACE=1
sms_port.exe > boss_wiggler.log 2>&1
```

- `SMS_PORT_WARP=<area>,<scenario>`: the stage (table below).
- `SMS_PORT_BOSSTRACE=1` (needs an SMS_PC build made after w5's boss tools are merged): one `[boss]` line per state / hit-point change of the boss, Shine state changes, and per-boss details. Please leave it on.
- `SMS_PORT_TRACE=1`: director state, lives, Shine count and Mario's position every few seconds (what tells me where a run stopped).
- The `> boss_xxx.log 2>&1` part writes all of that to a file next to the exe; send me that file.
- If something goes wrong, press **F9** right away: it saves `sms_replay_*.bin` next to the exe, which replays your exact input.
  A crash writes `sms_port_crash.log` (and a replay) on its own.
- Optional, only if a fight is too punishing to reach the part you want to test: `set SMS_PORT_GODMODE=1` (health and air never drop; falling off still loses a life),
  `set SMS_PORT_INFWATER=1` (FLUDD stays full), `set SMS_PORT_WARP_FLAGS=20001=50` (50 lives).
  Please say in your report if you used any of these.
- To clear a knob for the next run: `set SMS_PORT_WARP=` (nothing after the `=`), or open a new Command Prompt.

## Not confirmed yet (please play these)

### Wiggler: Gelato Beach episode 3 "Wiggler Ahoy! Full Steam Ahead!" (`SMS_PORT_WARP=4,2`)
- The bot got him to 1 hit point (two hits) and stopped there; his later falls happened in the sea.
- How it works in the code: spray a sand sprout or sand bomb in his path until the sand mound has grown, he walks over it and tips over, then ground pound his weak (belly) segment while he lies on his back.
- Watch for: he tips over when he walks over a grown mound; a ground pound on the right segment while he is down takes a hit (3 in total); the Shine, the save prompt, the return to the plaza.
  Anything odd: he never tips over on a fully grown mound, ground pounds on his belly never count, a softlock in the defeat demo.
- Log lines to expect: `TNerveBossHanachanTumble`, `...Down`, `...Damage` with `hp 2`, `hp 1`, `hp 0`, `wiggler segment N got a hip drop`, `shine ... -> 11`.

### Final Bowser: Corona Mountain (`SMS_PORT_WARP=60,0`)
- The bot landed super ground pounds on a bathtub grip (the grip counted it, 5 -> 4 grips standing), but each time the grip crumbled under Mario and he slid off the rocking tub, so it never got past the first grip.
- How it works: on one of the five points of the tub's rim, fire the rocket nozzle straight up (hold R) and ground pound at the top, so the long fall makes it a super ground pound on the grip; five grips start the ending.
- Watch for: each grip breaking on a super ground pound; how the tub rocks afterwards; the ending demo, movie, credits and the save prompt after the fifth; then the plaza.
  Anything odd: a grip that takes no damage from a proper super ground pound, falling through the tub's rim, a hang in the ending.
- Log lines to expect: `bathtub grip ... got a super hip drop`, `bathtub: 4 grips standing` ... `0`/ending, `TNerveKoopa...` state changes.

## Already finished by the bots (play them too if you like)

| Boss | Warp | Confirmed |
|---|---|---|
| Petey Piranha (Bianco Hills episode 1) | `55,0` | defeat, Shine, save prompt, plaza |
| Gooper Blooper (Ricco Harbor episode 1) | `59,0` | defeat, Shine, save prompt, plaza |
| Mecha-Bowser (Pinna Park, coaster) | `58,1` | defeat, stage change, Shine, save prompt, plaza |
| King Boo (Hotel Delfino casino) | `56,0` | defeat, Shine, save prompt, plaza |
| Eely-Mouth (Noki Bay) | `57,0` | defeat, Shine, save prompt, plaza |
| Phantamanta (Sirena Beach episode 1 "The Manta Storm") | `6,0` | defeat, Shine, save prompt, plaza (battle start forced, see below) |

Notes:
- Phantamanta: the battle starts when the stage script sets flag 0x50007; the bot could not find what triggers it, so it was set with `SMS_PORT_DEBUG_CALL=flag50007@1000`.
  In a normal play-through just walk around Sirena Beach; if the mantas never appear, that is worth a report (with the log).
- Wiggler is Gelato Beach episode 3 and Phantamanta is Sirena Beach, not the episodes / levels named in the original task list.
- The bot runs used `SMS_PORT_GODMODE=1` and, for the spraying fights, `SMS_PORT_INFWATER=1`, so a run with normal health and water is still worth doing for each.
