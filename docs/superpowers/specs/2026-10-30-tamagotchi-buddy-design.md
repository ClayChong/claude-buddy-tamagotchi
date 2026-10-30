# Tamagotchi Buddy Design

## Goal

Turn the Claude Buddy into a desk pet that you look after. It gets three stats that change over
real time and with Claude activity, button interactions to care for it, and far more idle
variety, so the home screen stops looping one blink clip.

The approval flow, BLE protocol, security behaviour and settings stay as they are. Pet care
must never make it easier to approve a prompt by accident.

## Non-goals

- The pet never dies or gets sick. The worst state is a hungry, sad or tired buddy.
- No new hardware. Input is the existing UP, DOWN and OK buttons.
- No sound effects in this round.
- The 18 built-in procedural species get the stats and care buttons but no new animations.
  New animations exist only for uploaded packs.

## Stats

All stats are integers from 0 to 100 and start at 80 on first boot.

| Stat | Changes over time | Changes from actions |
| --- | --- | --- |
| Fullness | -1 every 864 s (empty in 24 h) | Feed +30 |
| Happiness | -1 every 720 s | Play +20, Pat +5, an approval or "always" decision that reaches Claude +3 |
| Energy | -1 every 300 s while Claude is busy; +1 every 120 s while sleeping; -1 every 1800 s otherwise | none |

"Sleeping" means the bridge is disconnected or stale, or the local time is 23:00–06:59. Night
counts only after a time sync.

While a permission prompt has been waiting more than 120 s, happiness also drops 1 every 120 s.

Mood comes from the stats, checked in this order:

1. Fullness < 25 → hungry
2. Energy < 20 → tired
3. Happiness < 25 → sad
4. All three ≥ 70 → happy
5. Otherwise → normal

## Care actions

- **Home page, OK click** (no prompt, no menu, no confirmation): pat.
- **Pet page:** DOWN cycles the selection Feed → Play → Pat. OK performs it. This replaces
  the old, unused `pet_page` toggle.
- **Feeding at fullness ≥ 90** is refused. Stats don't change, the buddy plays its refuse
  reaction, and the pet page shows `Not hungry` in place of the mood word for 3000 ms. Claude's
  status message on the home page is not touched.
- **Five pats within 3000 ms** make the buddy dizzy for 3000 ms instead of patting.
- Reactions last 3000 ms: eat, play, pat, refuse, dizzy.

### Approval guard

OK approves a prompt only when both are true:

- The prompt has been visible for at least 1000 ms. Visible means the screen is on and no
  confirmation or passkey screen covers it, so waking the screen restarts the count.
- No OK press came in the previous 1000 ms, so a run of pats never turns into an approval.

An OK press that fails either check is ignored. DOWN (deny) has no guard.

## Persistence and offline time

- Stats are stored in NVS as one `u64` under the key `pet`:
  `epoch_seconds << 32 | energy << 16 | happiness << 8 | fullness`.
  `epoch_seconds` is the synced UTC epoch at save time, or 0 when the clock was never synced
  during that session.
- Save after every care action and every 600 s. The existing 60 s settings flush throttle
  still applies.
- Offline catch-up runs once per boot, on the first time sync:
  `offline = boot_epoch - saved_epoch`, where `boot_epoch = synced_epoch - uptime_seconds`.
  It is applied as sleeping time, capped at 86 400 s. It is skipped when either epoch is 0 or
  the result is negative.
- Factory reset clears the pet back to 80/80/80.

## Idle animation

An idle scheduler picks which pack clip plays next. Each pick starts at frame 0.

- Clip 0 of `idle` is the base clip, usually a blink.
- After a special clip, the base clip plays 1–3 times (chosen at random). Then the next
  special clip plays.
- A special clip is any `idle` clip other than 0. The same special never plays twice in a row
  when there are two or more.
- When the mood is hungry, sad or tired and the pack has that mood's clip, each special pick
  has a 50% chance of being the mood clip instead.
- Reactions and other states also start at frame 0 when they begin.

## Pack format v2

Version 2 keeps the BPK1 magic and layout but grows the state table from 7 to 14 entries.
The fixed header becomes 104 bytes.

State order: `sleep, idle, busy, attention, celebrate, dizzy, heart, eat, play, pat, refuse,
hungry, sad, tired`.

The firmware reads both versions. In version 1 packs the new states are empty.

When a state has no clips, it falls back as follows:

| Missing state | Plays |
| --- | --- |
| eat, pat | heart |
| play | celebrate |
| refuse | dizzy, then idle |
| hungry, sad, tired | nothing; the scheduler skips the mood clip |
| any other | idle |

`pack_builder.py` writes version 2 only when the folder has at least one new state, so packs
without new sheets still work on older firmware.

| Sheet | Default frame time |
| --- | --- |
| eat, play, pat, refuse | 200 ms |
| hungry, tired | 300 ms |
| sad | 400 ms |

## Pet page

The pet page shows real values in place of the placeholder MOOD hearts and ENERGY bars:

- Three labelled 8-segment bars: FULL, HAPPY, ENERGY. A bar lights `(value * 8 + 50) / 100`
  segments, so 0 lights none, 50 lights 4 and 100 lights 8.
- A mood word: HAPPY, OK, HUNGRY, TIRED or SAD.
- The care selection row `FEED  PLAY  PAT`, with the selected item highlighted.
- The level badge and token counts stay.

## Built-in species

Built-in species reuse existing art for the new reactions: eat and pat → heart, play →
celebrate, refuse → dizzy. They have no mood idles.

## Acceptance

- Host tests cover the stat rules, mood order, persistence packing, offline catch-up, the
  approval guard, the pat burst, the idle scheduler and pack v2 parsing with fallbacks.
- On the device: specials appear between blinks; feeding, playing and patting each show their
  reaction; stats survive a power cycle; an OK press in the first second of a new prompt does
  not approve it.

## Poop (added after the first release)

- Claude busy time (the bridge reports `running > 0`) adds up across turns. Each 20 minutes
  of it drops one poop the next time Claude is fully idle: nothing running and no prompt
  waiting. Busy time is not saved across reboots.
- The pile holds at most 4 poops. Busy time that would overflow a full pile is discarded.
- Each uncleaned poop costs 1 extra happiness point per 1800 s, including offline catch-up.
- Mood order becomes: hungry, stinky (any poop), tired, sad, happy, normal.
- A double OK press on the home or pet page cleans the whole pile and plays the `clean`
  reaction for 3000 ms. It gives no happiness. Without poop, a double press counts as one
  click. A double OK never approves a prompt or confirms a reset, but still counts as an OK
  press for the approval guard's quiet second.
- The poop count is saved in bits 24–31 of the `pet` value.
- Pack format version 3 adds `clean`, `stinky` and `poop` (17 states, 116-byte fixed header).
  `clean` falls back to heart; `stinky` and `poop` have no fallback. Poop frames hold the
  poop in their bottom-left quarter, which the device stacks in a column right of the buddy,
  24 px apart. Without poop frames the device draws a built-in poop.
