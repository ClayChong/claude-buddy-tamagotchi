# Tamagotchi Buddy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Claude Buddy stats, care interactions and a random, mood-aware idle animation scheduler.

**Architecture:** Two new pure-C modules carry the logic and are host-tested: `buddy_pet` (stats, mood, persistence packing) and `buddy_anim` (clip playback and idle scheduling over a parsed pack). The reducer in `buddy_state.c` owns the pet and its inputs. `buddy_ui.c` only asks `buddy_anim` for the frame and draws the pet page. Pack format v2 grows the state table to 14 states. The host converter `pack_builder.py` writes v2 only when new sheets exist.

**Tech Stack:** C (ESP-IDF 5.5.3, LVGL 9.5 I4 canvas), host CMake/CTest with `assert`, Python 3 + numpy + Pillow for the converter.

**Spec:** `docs/superpowers/specs/2026-10-30-tamagotchi-buddy-design.md`

## Global Constraints

- The approval flow, BLE protocol, security behaviour and existing settings keys stay unchanged.
- The only change to approvals: OK approves only after the prompt has been visible for at least 1000 ms.
- No heap allocation in new firmware modules.
- Stats are 0–100 and start at 80. Rates and amounts are exactly the spec's values.
- Pack state order: `sleep, idle, busy, attention, celebrate, dizzy, heart, eat, play, pat, refuse, hungry, sad, tired`.
- Host tests compile with `-Wall -Wextra -Werror`.
- Host test commands (run from the firmware repo root):
  `export IDF_PATH=~/esp/esp-idf-v5.5.3 PATH=~/.espressif/tools/cmake/3.30.2/CMake.app/Contents/bin:$PATH`
  then `cmake --build build-host && ctest --test-dir build-host -R <name> --output-on-failure`.
- Firmware build: `. ~/esp/esp-idf-v5.5.3/export.sh && idf.py build`.
- Flashing needs the user's approval. Use segmented `idf.py -p /dev/cu.usbmodem2101 flash` so NVS (settings, bonds, pet) survives.
- Report every device check as NOT RUN until it has been done on the board.
- `~/WebstormProjects/claude-buddy-bridge` is not a git repository. Converter changes there are not committed.

## Review Focus

1. **Screen off for hours, then wake:** the animation and pet catch-up must finish instantly and stats must clamp, not wrap. Tests: Task 2 `test_long_gap_returns_quickly`, Task 4 `test_huge_elapse_clamps`.
2. **A prompt arrives in the middle of a pat burst:** an OK press in its first 1000 ms neither approves nor counts as a pat. Test: Task 7 `test_ok_during_guard_is_ignored`.
3. **A pack is uploaded or deleted mid-clip:** playback restarts and never returns a frame index beyond the new pack. Test: Task 2 `test_pack_change_restarts`.
4. **The clock syncs twice in one session, or the saved epoch is in the future:** catch-up runs at most once and skips negative gaps. Test: Task 6 `test_catch_up_once_and_never_negative`.
5. **Factory reset:** the pet returns to 80/80/80 and is not overwritten by an older saved value. Test: Task 6 `test_factory_reset_clears_pet`.

---

### Task 1: Pack format v2 and state fallbacks

**Files:**
- Modify: `main/buddy_pack_format.h`, `main/buddy_pack_format.c`
- Test: `tests/test_buddy_pack.c`

**Interfaces:**
- Produces:
  - `buddy_pack_state_t` gains `BUDDY_PACK_EAT, BUDDY_PACK_PLAY, BUDDY_PACK_PAT, BUDDY_PACK_REFUSE, BUDDY_PACK_HUNGRY, BUDDY_PACK_SAD, BUDDY_PACK_TIRED` before `BUDDY_PACK_STATE_COUNT` (now 14).
  - `#define BUDDY_PACK_V1_STATE_COUNT 7U`, `BUDDY_PACK_VERSION 2U`, `BUDDY_PACK_FIXED_HEADER_V1 76U`, `BUDDY_PACK_FIXED_HEADER 104U`.
  - `buddy_pack_info_t` gains `uint16_t version`.
  - `bool buddy_pack_resolve_state(const buddy_pack_info_t *info, buddy_pack_state_t state, buddy_pack_state_t *resolved);`
  - `buddy_pack_frame_at()` keeps its signature but falls back through `buddy_pack_resolve_state()`.

- [ ] **Step 1: Write failing tests** in `tests/test_buddy_pack.c`, extending its existing pack-building helper with a version argument:
  - `test_v1_pack_leaves_new_states_empty`: a v1 pack parses, `info.version == 1`, and `info.states[BUDDY_PACK_EAT].clip_count == 0`.
  - `test_v2_pack_reads_fourteen_states`: a v2 pack with one `eat` clip parses, `info.version == 2`, and `info.states[BUDDY_PACK_EAT].clip_count == 1`. The header size is `104 + 8 * clips + 4`.
  - `test_version_3_rejected`: `buddy_pack_parse_header()` returns false.
  - `test_resolve_fallbacks`: on a pack with only idle, heart, celebrate and dizzy clips, eat → heart, pat → heart, play → celebrate, refuse → dizzy, and an empty sleep → idle. Hungry, sad and tired return false. On a pack without dizzy, refuse → idle.
- [ ] **Step 2: Run and see it fail.** Run `ctest --test-dir build-host -R buddy_pack`. Expected: build fails because `BUDDY_PACK_EAT` is undefined.
- [ ] **Step 3: Implement.** Parse version 1 (7 states at offset 48, clips at 76) and version 2 (14 states at offset 48, clips at 104). Reject any other version. The fallback table is the spec's. Update `BUDDY_PACK_MAX_HEADER` to use the v2 fixed header. Update the layout comment at the top of the header.
- [ ] **Step 4: Run and see it pass.** Run `ctest --test-dir build-host --output-on-failure`. Expected: all tests pass.
- [ ] **Step 5: Commit** `feat(pack): read v2 packs with care and mood states`.

### Task 2: `buddy_anim` playback and idle scheduler

**Files:**
- Create: `main/buddy_anim.h`, `main/buddy_anim.c`, `tests/test_buddy_anim.c`
- Modify: `main/buddy_types.h` (mood enum), `main/CMakeLists.txt`, `tests/CMakeLists.txt` (target `test_buddy_anim` with `../main/buddy_anim.c ../main/buddy_pack_format.c`)

**Interfaces:**
- Consumes: Task 1's `buddy_pack_info_t`, `buddy_pack_resolve_state()`, `buddy_pack_frame_at()`.
- Produces:
  - In `buddy_types.h`: `typedef enum { BUDDY_MOOD_NORMAL, BUDDY_MOOD_HAPPY, BUDDY_MOOD_HUNGRY, BUDDY_MOOD_TIRED, BUDDY_MOOD_SAD } buddy_mood_t;`
  - `typedef struct { ... } buddy_anim_t;` (fields are private to the module; it holds the RNG, the playing state, the clip, its start time, the bases left, the last special and the pack CRC).
  - `void buddy_anim_init(buddy_anim_t *anim, uint32_t seed);`
  - `bool buddy_anim_frame(buddy_anim_t *anim, const buddy_pack_info_t *info, buddy_pack_state_t state, buddy_mood_t mood, uint64_t now_ms, uint16_t *frame);` returns false when `info` is NULL or nothing can be shown.

Rules (from the spec):
- A change of `state` or of `info->payload_crc` restarts playback at `now_ms`.
- Non-idle states play `buddy_pack_frame_at(info, state, now_ms - started_ms)`.
- Idle starts on clip 0 with 0–2 more base plays queued. When a base clip ends with none queued, a special is picked. After a special or mood clip, 0–2 more base plays are queued again, so 1–3 bases play in total.
- Special pick: when the mood is hungry, tired or sad and `buddy_pack_resolve_state()` returns that mood's own state, there's a 50% chance of playing its clip 0. Otherwise it picks a random idle clip from 1 to n−1, excluding the last special when two or more exist. With no idle specials and no mood clip, it plays base again.
- When a clip ends, the next clip starts at the previous clip's end time. After 32 clip changes in one call, restart at `now_ms` instead.
- RNG: xorshift32. A seed of 0 becomes 1.

- [ ] **Step 1: Write failing tests** in `tests/test_buddy_anim.c`. They use a `buddy_pack_info_t` filled in directly: idle has 4 clips of 4 frames at 300 ms each; busy, hungry and heart have one clip each.
  - `test_state_change_starts_at_frame_zero`: entering busy at `now_ms = 12345` returns busy's `first_frame`.
  - `test_base_runs_between_specials`: walk 2000 clip boundaries in idle. Every run of base clips between specials is 1–3 long, and no two consecutive specials are equal.
  - `test_single_idle_clip_always_base`: with 1 idle clip and mood NORMAL, every frame belongs to clip 0.
  - `test_mood_clip_share`: with mood HUNGRY, hungry makes up 35–65% of the special slots over 2000 boundaries.
  - `test_missing_mood_clip_ignored`: with mood SAD and no sad clips, no frame ever comes from outside idle.
  - `test_pack_change_restarts`: change `payload_crc` and shrink `frame_count` mid-clip. The next frame is the new idle clip 0's first frame and is less than `frame_count`.
  - `test_long_gap_returns_quickly`: advance `now_ms` by 1,000,000,000 and get a valid idle frame.
- [ ] **Step 2: Run and see it fail.** Run `ctest --test-dir build-host -R buddy_anim`. Expected: build fails because `buddy_anim.h` is missing.
- [ ] **Step 3: Implement** `buddy_anim.c` following the rules above.
- [ ] **Step 4: Run and see it pass.** Run `ctest --test-dir build-host --output-on-failure`. Expected: all tests pass.
- [ ] **Step 5: Commit** `feat(buddy): schedule idle clips at random between blinks`.

### Task 3: Draw packs through `buddy_anim` (phase 1b on device)

**Files:**
- Modify: `main/buddy_pack.h`, `main/buddy_pack.c`, `main/buddy_ui.c`

**Interfaces:**
- Consumes: Task 2's `buddy_anim_init()` and `buddy_anim_frame()`.
- Produces:
  - `const buddy_pack_info_t *buddy_pack_info(void);` returns NULL when no pack is available.
  - `static buddy_pack_state_t pack_state(buddy_character_t c)` in `buddy_ui.c`, mapping characters to pack states. `art_state()` stays for built-in sprites.
  - `buddy_pack_frame()` is removed from `buddy_pack.h` and `buddy_pack.c`.

- [ ] **Step 1: Wire it up.** Add a `static buddy_anim_t s_anim` in `buddy_ui.c` and seed it with `esp_random()` in `buddy_ui_init()`. `buddy_ui_tick()` calls `buddy_anim_frame(&s_anim, buddy_pack_info(), pack_state(...), BUDDY_MOOD_NORMAL, elapsed_ms, &frame)` once per tick. It redraws when the frame changes. `draw_pack()` draws the stored `s_pack_frame` and does no lookup of its own. Task 9 replaces `BUDDY_MOOD_NORMAL` with the snapshot's mood.
- [ ] **Step 2: Build the firmware.** Run `idf.py build`. Expected: build succeeds and `ctest --test-dir build-host` still passes.
- [ ] **Step 3: Device check (needs flash approval).** With the angel pack plus `idle_1`–`idle_8` pushed, watch the home screen for 2 minutes. Specials appear between 1–3 blinks, and no special repeats back to back. A heart reaction starts from its first frame. Record PASS or NOT RUN.
- [ ] **Step 4: Commit** `feat(ui): play pack clips through the idle scheduler`.

### Task 4: `buddy_pet` stats module

**Files:**
- Create: `main/buddy_pet.h`, `main/buddy_pet.c`, `tests/test_buddy_pet.c`
- Modify: `main/CMakeLists.txt`, `tests/CMakeLists.txt` (target `test_buddy_pet`)

**Interfaces:**
- Consumes: `buddy_mood_t` from Task 2.
- Produces:
  ```c
  #define BUDDY_PET_START 80U
  typedef enum { BUDDY_PET_AWAKE, BUDDY_PET_BUSY, BUDDY_PET_SLEEPING } buddy_pet_activity_t;
  typedef enum { BUDDY_PET_FULLNESS, BUDDY_PET_HAPPINESS, BUDDY_PET_ENERGY } buddy_pet_stat_t;
  typedef struct {
      uint8_t fullness, happiness, energy;
      uint32_t fullness_carry_s, happiness_carry_s, energy_carry_s;
      buddy_pet_activity_t energy_activity;
  } buddy_pet_t;
  void buddy_pet_init(buddy_pet_t *pet);
  void buddy_pet_elapse(buddy_pet_t *pet, uint32_t seconds, buddy_pet_activity_t activity);
  void buddy_pet_add(buddy_pet_t *pet, buddy_pet_stat_t stat, int delta);   /* clamps to 0..100 */
  bool buddy_pet_feed(buddy_pet_t *pet);   /* false and no change when fullness >= 90 */
  void buddy_pet_play(buddy_pet_t *pet);   /* happiness +20 */
  void buddy_pet_pat(buddy_pet_t *pet);    /* happiness +5 */
  buddy_mood_t buddy_pet_mood(const buddy_pet_t *pet);
  uint64_t buddy_pet_pack(const buddy_pet_t *pet, uint32_t epoch_seconds);
  bool buddy_pet_unpack(uint64_t packed, buddy_pet_t *pet, uint32_t *epoch_seconds); /* false if a stat > 100 */
  ```
  Energy carry resets when `activity` differs from `energy_activity`.

- [ ] **Step 1: Write failing tests**:
  - `test_init_is_eighty`: all three stats are 80.
  - `test_fullness_decay`: `elapse(864 * 10, AWAKE)` takes fullness 80 → 70. Two calls of 432 s take off exactly 1.
  - `test_happiness_decay`: `elapse(7200, AWAKE)` takes happiness 80 → 70.
  - `test_energy_by_activity`: 3000 s BUSY: 80 → 70. 1200 s SLEEPING: 70 → 80. 18000 s AWAKE: 80 → 70.
  - `test_feed_refused_when_full`: fullness 89 → feed → 100 (true). Feed again returns false and leaves fullness at 100.
  - `test_mood_order`: fullness 24 with energy 10 → HUNGRY. Fullness 50 with energy 19 → TIRED. Happiness 24 → SAD. All at 70 → HAPPY. 69/70/70 → NORMAL.
  - `test_pack_round_trip`: pack(stats 12/34/56, epoch 1767225600) unpacks to the same values. A packed fullness of 101 fails to unpack.
  - `test_huge_elapse_clamps`: `elapse(UINT32_MAX, BUSY)` leaves fullness, happiness and energy at 0.
- [ ] **Step 2: Run and see it fail.** Run `ctest --test-dir build-host -R buddy_pet`. Expected: build fails because `buddy_pet.h` is missing.
- [ ] **Step 3: Implement** `buddy_pet.c`.
- [ ] **Step 4: Run and see it pass.** Run `ctest --test-dir build-host --output-on-failure`. Expected: all tests pass.
- [ ] **Step 5: Commit** `feat(buddy): add pet stats with decay and mood`.

### Task 5: Pet in the reducer, with Claude-linked effects

**Files:**
- Modify: `main/buddy_state.h`, `main/buddy_state.c`, `main/buddy_types.h`, `tests/CMakeLists.txt` (add `../main/buddy_pet.c` to the state and orchestrator targets), `tests/test_buddy_state.c`

**Interfaces:**
- Consumes: Task 4's API.
- Produces:
  - `buddy_state_t` gains `buddy_pet_t pet; uint64_t pet_updated_ms; uint64_t prompt_shown_ms; uint32_t prompt_penalties;`
  - `buddy_ui_snapshot_t` gains `uint8_t pet_fullness, pet_happiness, pet_energy; buddy_mood_t mood;`
  - `static bool buddy_is_night(const buddy_state_t *state, uint64_t now_ms)` is true when a time sync exists and the local hour (`epoch + tz + (now_ms - time_received_ms) / 1000`) is ≥ 23 or < 7.

Behaviour:
- At the start of every `buddy_state_reduce()` call, apply the whole seconds since `pet_updated_ms`, then advance `pet_updated_ms` by those seconds. The activity is SLEEPING when not connected, stale or at night; BUSY when `running > 0`; otherwise AWAKE.
- `prompt_shown_ms` is set and `prompt_penalties` reset whenever a new prompt is adopted, on both the heartbeat and the prompt-event paths. While an actionable prompt is shown, happiness drops 1 for each new whole 120 s since `prompt_shown_ms`.
- A successful permission result with decision ONCE or ALWAYS adds 3 happiness.

- [ ] **Step 1: Write failing tests** in `tests/test_buddy_state.c`:
  - `test_pet_decays_with_ticks`: connected and idle. A tick at +7,200,000 ms leaves happiness at 70.
  - `test_busy_drains_energy`: a heartbeat with `running = 1`, then a tick 3,000,000 ms later, leaves energy at 70.
  - `test_night_counts_as_sleep`: connected, with a time sync at 23:30 local. A heartbeat 1,200,000 ms later, with no tick in between so the bridge is never marked stale, raises energy by 10. The same sequence at 12:00 leaves energy unchanged.
  - `test_waiting_prompt_lowers_happiness`: a prompt is shown. A tick at +360,000 ms costs 3 happiness, and a repeat tick at the same time costs nothing more.
  - `test_approval_raises_happiness`: a successful ONCE result adds 3.
  - The snapshot carries the stats and `buddy_pet_mood()`.
- [ ] **Step 2: Run and see it fail.** Run `ctest --test-dir build-host -R buddy_state`. Expected: build fails because the `pet` field is missing.
- [ ] **Step 3: Implement** the behaviour above.
- [ ] **Step 4: Run and see it pass.** Run `ctest --test-dir build-host --output-on-failure`. Expected: all tests pass.
- [ ] **Step 5: Commit** `feat(buddy): drive pet stats from time and Claude activity`.

### Task 6: Persist the pet and catch up offline time

**Files:**
- Modify: `main/buddy_settings.h`, `main/buddy_settings.c`, `main/buddy_types.h`, `main/buddy_state.h`, `main/buddy_state.c`, `main/buddy_orchestrator.h`, `main/buddy_orchestrator.c`, `main/main.c`
- Test: `tests/test_buddy_settings.c`, `tests/test_buddy_state.c`, `tests/test_buddy_orchestrator.c`

**Interfaces:**
- Consumes: `buddy_pet_pack()` and `buddy_pet_unpack()`.
- Produces:
  - `buddy_settings_snapshot_t` gains `uint64_t pet` (0 means nothing stored). NVS key `"pet"`.
  - `esp_err_t buddy_settings_set_pet(uint64_t packed);` marks settings dirty, like the other setters.
  - `BUDDY_ACTION_PET_SAVE` in `buddy_action_type_t`, and `uint64_t pet` in `buddy_action_t`.
  - `esp_err_t (*persist_pet)(void *context, uint64_t packed);` in `buddy_orchestrator_ops_t`. `main.c` implements it as `buddy_settings_set_pet()` followed by `buddy_settings_flush(false)`.
  - `buddy_state_t` gains `uint32_t pet_saved_epoch; bool pet_catch_up_done; uint64_t pet_saved_ms;`

Behaviour:
- `buddy_state_init()` unpacks `settings->pet` when non-zero; otherwise it calls `buddy_pet_init()`.
- The first `BUDDY_EVENT_TIME` of a session sets `boot_epoch = epoch - now_ms / 1000`. When `pet_saved_epoch > 0` and `boot_epoch > pet_saved_epoch`, it elapses `min(boot_epoch - pet_saved_epoch, 86400)` as SLEEPING. Then `pet_catch_up_done` becomes true.
- On a TICK where 600,000 ms have passed since `pet_saved_ms`, emit `BUDDY_ACTION_PET_SAVE`. The packed epoch is the current UTC epoch when synced, otherwise 0.
- Before relying on `PET_SAVE` replacing `UI_REFRESH`, check that `main.c` renders after every reduce. If it doesn't, make it render after `PET_SAVE` too.

- [ ] **Step 1: Write failing tests**:
  - `test_buddy_settings.c` `test_pet_round_trip`: `set_pet(x)`, flush, then load gives `x`. `test_factory_reset_clears_pet`: after a factory reset, load gives `pet == 0`. In `test_buddy_state.c`, a state initialised from `pet == 0` is 80/80/80.
  - `test_buddy_state.c` `test_catch_up_once_and_never_negative`: saved epoch E with stats 80. Time sync at `now_ms = 10,000` with epoch `E + 3610` elapses 3600 s of sleep. A second sync changes nothing. A fresh state whose saved epoch is ahead of `boot_epoch` changes nothing.
  - `test_buddy_state.c` `test_catch_up_capped_at_one_day`: a gap of 10 days elapses 86,400 s.
  - `test_buddy_state.c` `test_periodic_pet_save`: a tick at +600,000 ms returns `BUDDY_ACTION_PET_SAVE` with `action.pet == buddy_pet_pack(&state.pet, epoch)`.
  - `test_buddy_orchestrator.c`: `BUDDY_ACTION_PET_SAVE` calls `persist_pet` with `action.pet`.
- [ ] **Step 2: Run and see it fail.** Run `ctest --test-dir build-host -R "buddy_(settings|state|orchestrator)"`. Expected: build fails on the missing names.
- [ ] **Step 3: Implement** the behaviour above, including the `main.c` op.
- [ ] **Step 4: Run and see it pass.** Run `ctest --test-dir build-host --output-on-failure` and `idf.py build`. Expected: both succeed.
- [ ] **Step 5: Commit** `feat(buddy): save pet stats and catch up time spent off`.

### Task 7: Care input and the approval guard

**Files:**
- Modify: `main/buddy_types.h`, `main/buddy_state.h`, `main/buddy_state.c`
- Test: `tests/test_buddy_state.c`

**Interfaces:**
- Consumes: `buddy_pet_feed/play/pat()`, `BUDDY_ACTION_PET_SAVE`.
- Produces:
  - `buddy_character_t` gains `BUDDY_CHARACTER_EAT, BUDDY_CHARACTER_PLAY, BUDDY_CHARACTER_PAT, BUDDY_CHARACTER_REFUSE`, appended after `BUDDY_CHARACTER_CONFIRMATION`.
  - `typedef enum { BUDDY_CARE_FEED, BUDDY_CARE_PLAY, BUDDY_CARE_PAT, BUDDY_CARE_COUNT } buddy_care_t;`
  - `buddy_state_t` and `buddy_ui_snapshot_t` gain `buddy_care_t care_selection`. `buddy_state_t` gains `uint64_t pat_ms[4]; uint8_t pat_count;`
  - `pet_page` is removed from the state, the snapshot and the reducer. Grep for every use.
  - Constants: `BUDDY_REACTION_MS 3000`, `BUDDY_APPROVAL_GUARD_MS 1000`, `BUDDY_PAT_BURST_MS 3000`.

Behaviour:
- Home page with no prompt, no menu and no confirmation: OK pats.
- Pet page: DOWN cycles `care_selection`, and OK performs it.
- Each care action sets a temporary character for 3000 ms (EAT, PLAY, PAT, or REFUSE with the message `Not hungry`) and returns `BUDDY_ACTION_PET_SAVE`.
- A pat whose 4 previous pats all fall within the last 3000 ms makes the buddy DIZZY for 3000 ms, adds no happiness, and clears the pat history.
- OK on an actionable prompt shown less than 1000 ms ago does nothing at all. DOWN (deny) has no guard.

- [ ] **Step 1: Write failing tests**:
  - `test_home_ok_pats`: happiness goes from 80 to 85, the character is PAT, and the action is PET_SAVE.
  - `test_pet_page_cycles_and_feeds`: on the pet page, DOWN twice selects PAT, DOWN once more wraps to FEED, and OK feeds (fullness 80 → 100, character EAT).
  - `test_feed_refused`: at fullness 95, OK on FEED leaves fullness at 95, the character is REFUSE and the message is `Not hungry`.
  - `test_pat_burst_dizzy`: five OK pats 500 ms apart. The fifth gives DIZZY with no happiness gain; the first four give +20 in total.
  - `test_ok_during_guard_is_ignored`: OK 999 ms after the prompt appears produces no permission action and no happiness change. OK at 1000 ms approves.
  - Update the existing approval tests to press OK at least 1000 ms after the prompt appears. Their expected outcomes stay the same.
- [ ] **Step 2: Run and see it fail.** Run `ctest --test-dir build-host -R buddy_state`. Expected: the new tests fail.
- [ ] **Step 3: Implement** the behaviour above.
- [ ] **Step 4: Run and see it pass.** Run `ctest --test-dir build-host --output-on-failure`. Expected: all tests pass.
- [ ] **Step 5: Commit** `feat(buddy): feed, play and pat with an approval guard`.

### Task 8: Pack converter v2 (bridge, not under git)

**Files:**
- Modify: `~/WebstormProjects/claude-buddy-bridge/pack_builder.py`, `~/WebstormProjects/claude-buddy-bridge/art/SPRITE_PROMPT.md`
- Create: `~/WebstormProjects/claude-buddy-bridge/tests/test_pack_builder.py` (stdlib `unittest`)

**Interfaces:**
- Produces: `STATES` lists all 14 states in spec order. `NEW_STATES = STATES[7:]`. `pack_bytes(name, clips)` keeps its return value `(bytes, frames, (width, height))`. `load_sheets()` accepts the new file names.

Behaviour:
- Version 2 is written when any of `NEW_STATES` has clips. The fixed header is then 104 bytes and the state table has 14 entries.
- Otherwise it writes version 1, byte-for-byte as before.
- Add these to `DEFAULT_FRAME_MS`: eat, play, pat and refuse 200; hungry and tired 300; sad 400.

- [ ] **Step 1: Write failing tests** using synthetic 4×4 numpy frames:
  - `test_v1_when_no_new_states`: bytes 4–6 decode to version 1 and the header size is `76 + 8 * clips + 4`.
  - `test_v2_with_eat`: version 2, header size `104 + 8 * clips + 4`, and state entry 7 has `count == 1`.
  - `test_frame_ms_defaults`: the eat clip's `frame_ms` is 200.
- [ ] **Step 2: Run and see it fail.** Run `cd ~/WebstormProjects/claude-buddy-bridge && python3 -m unittest tests/test_pack_builder.py -v`. Expected: `test_v2_with_eat` fails.
- [ ] **Step 3: Implement** the behaviour above. Add the 7 care and mood rows to the sheet table in `SPRITE_PROMPT.md`.
- [ ] **Step 4: Run and see it pass.** Run the same command. Expected: OK. Also run `python3 pack_builder.py art/angel` and check that it still writes a version 1 pack while the angel has no new sheets.

### Task 9: Pet page, reaction art and docs

**Files:**
- Modify: `main/buddy_ui.c`, `README.md`

**Interfaces:**
- Consumes: the snapshot's `pet_fullness`, `pet_happiness`, `pet_energy`, `mood` and `care_selection`; the new `BUDDY_CHARACTER_*` values; Task 3's `pack_state()`.

- [ ] **Step 1: Map the reactions.** `pack_state()` maps EAT, PLAY, PAT and REFUSE to their pack states. `art_state()` maps EAT and PAT → 6 (heart), PLAY → 4 (celebrate) and REFUSE → 5 (dizzy). `buddy_ui_tick()` passes `s_snapshot.mood` to `buddy_anim_frame()`.
- [ ] **Step 2: Redraw `draw_pet()`.** Use these row positions:
  - y 168: the mood word (HAPPY, OK, HUNGRY, TIRED, SAD) on the left and the LV badge on the right.
  - y 190, 206 and 222: the FULL, HAPPY and ENERGY bars, each with 8 segments, lighting `(value * 8 + 50) / 100` of them.
  - y 242 and 258: TOKENS and TODAY.
  - y 276: `FEED  PLAY  PAT`, with the selected item on an orange box.
  - Change the `BUDDY_ACTION_PET` hint to `UP page  DOWN pick  OK do`.
- [ ] **Step 3: Update the README.** Document the pet stats, the care buttons, the approval guard and pack v2 states.
- [ ] **Step 4: Build.** Run `idf.py build` and `ctest --test-dir build-host --output-on-failure`. Expected: both succeed.
- [ ] **Step 5: Device acceptance (needs flash approval).** Check each item and record PASS or NOT RUN:
  - Feed, play and pat each show their reaction.
  - Feeding at fullness ≥ 90 shows `Not hungry`.
  - Five quick pats cause dizzy.
  - The stats survive a power cycle.
  - An OK press in the first second of a new prompt doesn't approve it.
  - With a hungry sheet uploaded and fullness < 25, the hungry clip shows up among the idles.
- [ ] **Step 6: Commit** `feat(ui): show pet stats and care actions on the pet page`.
