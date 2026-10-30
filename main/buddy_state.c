#include "buddy_state.h"

#include <stddef.h>
#include <string.h>

#define BUDDY_HEART_ANIMATION_MS 5000
#define BUDDY_CELEBRATION_ANIMATION_MS 1500
#define BUDDY_HEARTBEAT_TIMEOUT_MS 30000
#define BUDDY_PROMPT_PENALTY_MS 120000ULL
#define BUDDY_APPROVAL_HAPPINESS 3
#define BUDDY_NIGHT_START_HOUR 23
#define BUDDY_NIGHT_END_HOUR 7
#define BUDDY_PET_SAVE_INTERVAL_MS 600000ULL
#define BUDDY_PET_CATCH_UP_MAX_S 86400U
#define BUDDY_REACTION_MS 3000ULL
#define BUDDY_APPROVAL_GUARD_MS 1000ULL
#define BUDDY_PAT_BURST_MS 3000ULL
#define BUDDY_TOKEN_CELEBRATION_STEP 50000

static void buddy_copy(char *destination, size_t destination_size, const char *source)
{
    size_t length = 0;

    if (destination_size == 0) {
        return;
    }
    if (source != NULL) {
        while (length + 1 < destination_size && source[length] != '\0') {
            ++length;
        }
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static void buddy_copy_entries(char destination[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX],
                               const char source[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX])
{
    unsigned index;

    for (index = 0; index < BUDDY_ENTRY_COUNT; ++index) {
        buddy_copy(destination[index], BUDDY_ENTRY_MAX, source[index]);
    }
}

static bool buddy_string_matches_length(const char *value, size_t value_size, size_t length)
{
    size_t index;

    if (length == 0 || length >= value_size) {
        return false;
    }
    for (index = 0; index < value_size; ++index) {
        if (value[index] == '\0') {
            return index == length;
        }
    }
    return false;
}

static bool buddy_prompt_id_is_valid(const buddy_prompt_t *prompt)
{
    return !prompt->id_truncated &&
           buddy_string_matches_length(prompt->id, sizeof(prompt->id), prompt->id_length);
}

static void buddy_invalidate_prompt(buddy_state_t *state)
{
    memset(&state->prompt, 0, sizeof(state->prompt));
    state->prompt_connection_generation = 0;
    state->approval_locked = false;
}

static void buddy_clear_logical_session(buddy_state_t *state)
{
    state->connected = false;
    state->connection = BUDDY_CONNECTION_OFFLINE;
    state->total = 0;
    state->running = 0;
    state->waiting = 0;
    state->tokens = 0;
    state->tokens_today = 0;
    state->message[0] = '\0';
    memset(state->entries, 0, sizeof(state->entries));
    state->heartbeat.total = 0;
    state->heartbeat.running = 0;
    state->heartbeat.waiting = 0;
    state->heartbeat.tokens = 0;
    state->heartbeat.tokens_today = 0;
    state->heartbeat.message[0] = '\0';
    memset(state->heartbeat.entries, 0, sizeof(state->heartbeat.entries));
    buddy_invalidate_prompt(state);
}

static buddy_character_t buddy_character_for(const buddy_state_t *state, uint64_t now_ms)
{
    bool has_prompt = state->prompt.id[0] != '\0' && !state->heartbeat_stale &&
                      !state->approval_locked;

    if (state->confirmation_pending || state->connection == BUDDY_CONNECTION_CONFIRMING) {
        return BUDDY_CHARACTER_CONFIRMATION;
    }
    if (state->connection == BUDDY_CONNECTION_PAIRING) {
        return BUDDY_CHARACTER_PAIRING;
    }
    if (has_prompt) {
        return BUDDY_CHARACTER_ATTENTION;
    }
    if (state->temporary_until_ms > now_ms) {
        return state->temporary_character;
    }
    if (state->running > 0) {
        return BUDDY_CHARACTER_BUSY;
    }
    if (state->connected && !state->heartbeat_stale) {
        return BUDDY_CHARACTER_IDLE;
    }
    return BUDDY_CHARACTER_SLEEP;
}

static void buddy_refresh_character(buddy_state_t *state, uint64_t now_ms)
{
    state->character = buddy_character_for(state, now_ms);
}

static void buddy_clear_stale_prompt(buddy_state_t *state, uint64_t now_ms)
{
    if (!state->heartbeat_stale && now_ms - state->last_heartbeat_ms >= BUDDY_HEARTBEAT_TIMEOUT_MS) {
        state->heartbeat_stale = true;
        buddy_clear_logical_session(state);
    }
}

static void buddy_set_ui_refresh(buddy_action_t *action)
{
    if (action != NULL) {
        action->type = BUDDY_ACTION_UI_REFRESH;
    }
}

static bool buddy_has_actionable_prompt(const buddy_state_t *state)
{
    return state->prompt.id[0] != '\0' && !state->heartbeat_stale &&
           !state->approval_locked && buddy_prompt_id_is_valid(&state->prompt);
}

static bool buddy_has_prompt(const buddy_state_t *state)
{
    return state->prompt.id[0] != '\0';
}

static void buddy_open_confirmation(buddy_state_t *state, buddy_confirmation_t confirmation,
                                    bool acknowledge, uint32_t connection_generation,
                                    buddy_action_t *action)
{
    buddy_invalidate_prompt(state);
    state->confirmation = confirmation;
    state->confirmation_pending = confirmation != BUDDY_CONFIRM_NONE;
    state->confirmation_acknowledge = acknowledge;
    state->confirmation_connection_generation = connection_generation;
    state->connection = BUDDY_CONNECTION_CONFIRMING;
    buddy_set_ui_refresh(action);
}

static void buddy_close_confirmation(buddy_state_t *state)
{
    state->confirmation = BUDDY_CONFIRM_NONE;
    state->confirmation_pending = false;
    state->confirmation_acknowledge = false;
    state->confirmation_connection_generation = 0;
    state->connection = state->connected ? BUDDY_CONNECTION_CONNECTED
                                         : BUDDY_CONNECTION_OFFLINE;
}

static void buddy_settings_click(buddy_state_t *state, buddy_key_t key,
                                 buddy_action_t *action)
{
    if (state->reset_open) {
        if (key == BUDDY_KEY_UP) {
            state->reset_selection = (buddy_reset_item_t)(
                (state->reset_selection + BUDDY_RESET_COUNT - 1) % BUDDY_RESET_COUNT);
            buddy_set_ui_refresh(action);
        } else if (key == BUDDY_KEY_DOWN) {
            state->reset_selection =
                (buddy_reset_item_t)((state->reset_selection + 1) % BUDDY_RESET_COUNT);
            buddy_set_ui_refresh(action);
        } else if (key == BUDDY_KEY_OK) {
            if (state->reset_selection == BUDDY_RESET_FACTORY_RESET) {
                buddy_open_confirmation(state, BUDDY_CONFIRM_FACTORY_RESET, false, 0, action);
            } else if (state->reset_selection == BUDDY_RESET_UNPAIR) {
                buddy_open_confirmation(state, BUDDY_CONFIRM_UNPAIR, false, 0, action);
            } else if (state->reset_selection == BUDDY_RESET_BACK) {
                state->reset_open = false;
                buddy_set_ui_refresh(action);
            } else if (state->pack_available) {
                buddy_state_set_pack(state, false, false);
                buddy_copy(state->message, sizeof(state->message), "Character deleted");
                if (action != NULL) {
                    action->type = BUDDY_ACTION_DELETE_CHARACTER;
                }
            } else {
                buddy_copy(state->message, sizeof(state->message), "No custom character installed");
                buddy_set_ui_refresh(action);
            }
        }
        return;
    }
    if (key == BUDDY_KEY_UP) {
        state->settings_selection =
            (buddy_settings_item_t)((state->settings_selection + BUDDY_SETTINGS_COUNT - 1) %
                                    BUDDY_SETTINGS_COUNT);
        buddy_set_ui_refresh(action);
        return;
    }
    if (key == BUDDY_KEY_DOWN) {
        state->settings_selection =
            (buddy_settings_item_t)((state->settings_selection + 1) % BUDDY_SETTINGS_COUNT);
        buddy_set_ui_refresh(action);
        return;
    }
    if (key != BUDDY_KEY_OK) {
        return;
    }

    switch (state->settings_selection) {
    case BUDDY_SETTINGS_BRIGHTNESS:
        state->brightness_level = (uint8_t)((state->brightness_level + 1U) % 5U);
        if (action != NULL) {
            action->type = BUDDY_ACTION_DISPLAY_BACKLIGHT;
            action->brightness_percent = (uint8_t)(20U + state->brightness_level * 20U);
        }
        break;
    case BUDDY_SETTINGS_SOUND:
    case BUDDY_SETTINGS_WIFI:
    case BUDDY_SETTINGS_LED:
    case BUDDY_SETTINGS_CLOCK_ROTATION:
        buddy_copy(state->message, sizeof(state->message), "Unavailable on this hardware");
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_SETTINGS_ASCII_PET:
        state->species = (uint8_t)((state->species + 1U) %
                                   (state->pack_available ? BUDDY_SPECIES_BUILTIN_COUNT + 1U
                                                          : BUDDY_SPECIES_BUILTIN_COUNT));
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_SETTINGS_BLE:
        state->settings.ble_enabled = !state->settings.ble_enabled;
        if (action != NULL) {
            action->type = BUDDY_ACTION_BLE_TOGGLE;
            action->ble_enabled = state->settings.ble_enabled;
        }
        break;
    case BUDDY_SETTINGS_TRANSCRIPT:
        state->transcript_enabled = !state->transcript_enabled;
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_SETTINGS_RESET:
        state->reset_open = true;
        state->reset_selection = BUDDY_RESET_DELETE_CHARACTER;
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_SETTINGS_BACK:
    case BUDDY_SETTINGS_COUNT:
        state->page = BUDDY_PAGE_HOME;
        buddy_set_ui_refresh(action);
        break;
    }
}

static void buddy_pet_save_action(buddy_state_t *state, uint64_t now_ms, buddy_action_t *action);

static void buddy_react(buddy_state_t *state, buddy_character_t character, uint64_t now_ms)
{
    state->temporary_character = character;
    state->temporary_until_ms = now_ms + BUDDY_REACTION_MS;
}

/* True when this pat is the fifth within BUDDY_PAT_BURST_MS; the history then
 * starts over. pat_ms holds up to four earlier pats, oldest first. */
static bool buddy_pat_burst(buddy_state_t *state, uint64_t now_ms)
{
    const size_t slots = sizeof(state->pat_ms) / sizeof(state->pat_ms[0]);

    if (state->pat_count == slots && now_ms - state->pat_ms[0] <= BUDDY_PAT_BURST_MS) {
        state->pat_count = 0;
        return true;
    }
    if (state->pat_count == slots) {
        memmove(&state->pat_ms[0], &state->pat_ms[1], (slots - 1U) * sizeof(state->pat_ms[0]));
        --state->pat_count;
    }
    state->pat_ms[state->pat_count++] = now_ms;
    return false;
}

static void buddy_care(buddy_state_t *state, buddy_care_t care, uint64_t now_ms,
                       buddy_action_t *action)
{
    switch (care) {
    case BUDDY_CARE_FEED:
        if (buddy_pet_feed(&state->pet)) {
            buddy_react(state, BUDDY_CHARACTER_EAT, now_ms);
        } else {
            buddy_react(state, BUDDY_CHARACTER_REFUSE, now_ms);
            buddy_copy(state->care_message, sizeof(state->care_message), "Not hungry");
            state->care_message_until_ms = now_ms + BUDDY_REACTION_MS;
        }
        break;
    case BUDDY_CARE_PLAY:
        buddy_pet_play(&state->pet);
        buddy_react(state, BUDDY_CHARACTER_PLAY, now_ms);
        break;
    case BUDDY_CARE_PAT:
    default:
        if (buddy_pat_burst(state, now_ms)) {
            buddy_react(state, BUDDY_CHARACTER_DIZZY, now_ms);
        } else {
            buddy_pet_pat(&state->pet);
            buddy_react(state, BUDDY_CHARACTER_PAT, now_ms);
        }
        break;
    }
    buddy_pet_save_action(state, now_ms, action);
}

static void buddy_normal_click(buddy_state_t *state, buddy_key_t key, uint64_t now_ms,
                               buddy_action_t *action)
{
    if (state->menu_open) {
        if (key == BUDDY_KEY_UP) {
            state->menu_selection = (buddy_menu_item_t)(
                (state->menu_selection + BUDDY_MENU_COUNT - 1) % BUDDY_MENU_COUNT);
        } else if (key == BUDDY_KEY_DOWN) {
            state->menu_selection =
                (buddy_menu_item_t)((state->menu_selection + 1) % BUDDY_MENU_COUNT);
        } else if (key == BUDDY_KEY_OK) {
            if (state->menu_selection == BUDDY_MENU_SETTINGS) {
                state->page = BUDDY_PAGE_SETTINGS;
                state->settings_selection = BUDDY_SETTINGS_BRIGHTNESS;
            } else if (state->menu_selection == BUDDY_MENU_TURN_OFF) {
                state->screen_off = true;
                if (action != NULL) {
                    action->type = BUDDY_ACTION_SCREEN_OFF;
                }
            } else if (state->menu_selection == BUDDY_MENU_HELP) {
                state->page = BUDDY_PAGE_INFO;
                state->info_page = 1;
            } else if (state->menu_selection == BUDDY_MENU_ABOUT) {
                state->page = BUDDY_PAGE_INFO;
                state->info_page = 5;
            } else if (state->menu_selection == BUDDY_MENU_DEMO) {
                buddy_copy(state->message, sizeof(state->message), "Demo unavailable");
            }
            state->menu_open = false;
        }
        buddy_set_ui_refresh(action);
        return;
    }
    if (state->page == BUDDY_PAGE_SETTINGS) {
        buddy_settings_click(state, key, action);
    } else if (key == BUDDY_KEY_UP) {
        state->page = state->page == BUDDY_PAGE_HOME
                          ? BUDDY_PAGE_PET
                          : (state->page == BUDDY_PAGE_PET ? BUDDY_PAGE_INFO
                                                          : BUDDY_PAGE_HOME);
        buddy_set_ui_refresh(action);
    } else if (key == BUDDY_KEY_DOWN && state->page == BUDDY_PAGE_PET) {
        state->care_selection = (buddy_care_t)((state->care_selection + 1U) % BUDDY_CARE_COUNT);
        buddy_set_ui_refresh(action);
    } else if (key == BUDDY_KEY_OK && state->page == BUDDY_PAGE_PET) {
        buddy_care(state, state->care_selection, now_ms, action);
    } else if (key == BUDDY_KEY_OK && state->page == BUDDY_PAGE_HOME) {
        buddy_care(state, BUDDY_CARE_PAT, now_ms, action);
    } else if (key == BUDDY_KEY_DOWN && state->page == BUDDY_PAGE_INFO) {
        state->info_page = (uint8_t)((state->info_page + 1U) % 6U);
        buddy_set_ui_refresh(action);
    } else if (key == BUDDY_KEY_DOWN && state->page == BUDDY_PAGE_HOME) {
        if (action != NULL) {
            action->type = BUDDY_ACTION_UI_SCROLL;
            action->scroll_delta = -24;
        }
    }
}

static bool buddy_prompt_ids_match(const buddy_prompt_t *left, const buddy_prompt_t *right)
{
    return left->id_length == right->id_length &&
           left->id_length > 0 &&
           memcmp(left->id, right->id, left->id_length) == 0;
}

/* Starts the approval guard and waiting penalty clock for a prompt that is not
 * already on screen. */
static void buddy_note_prompt_shown(buddy_state_t *state, const buddy_prompt_t *prompt,
                                    uint64_t now_ms)
{
    if (!buddy_prompt_ids_match(&state->prompt, prompt)) {
        state->prompt_shown_ms = now_ms;
        state->prompt_penalties = 0;
        state->prompt_visible = false;
    }
}

static bool buddy_prompt_was_attempted(const buddy_state_t *state,
                                       const buddy_prompt_t *prompt)
{
    return buddy_string_matches_length(state->last_attempted_prompt_id,
                                       sizeof(state->last_attempted_prompt_id),
                                       prompt->id_length) &&
           memcmp(state->last_attempted_prompt_id, prompt->id, prompt->id_length) == 0;
}

static void buddy_apply_heartbeat(buddy_state_t *state, const buddy_heartbeat_t *heartbeat,
                                  uint32_t connection_generation, uint64_t now_ms,
                                  buddy_action_t *action)
{
    uint64_t level = heartbeat->tokens / BUDDY_TOKEN_CELEBRATION_STEP;

    state->heartbeat = *heartbeat;
    state->connected = heartbeat->connected;
    state->connection = heartbeat->connected ? BUDDY_CONNECTION_CONNECTED : BUDDY_CONNECTION_OFFLINE;
    state->heartbeat_stale = !heartbeat->connected;
    if (!heartbeat->connected) {
        buddy_invalidate_prompt(state);
    } else if (heartbeat->prompt.id[0] == '\0') {
        buddy_invalidate_prompt(state);
    } else if (!buddy_prompt_id_is_valid(&heartbeat->prompt)) {
        buddy_invalidate_prompt(state);
    } else if (state->prompt.id[0] != '\0' &&
               buddy_prompt_ids_match(&state->prompt, &heartbeat->prompt)) {
        state->prompt = heartbeat->prompt;
        state->prompt_connection_generation = connection_generation;
    } else if (buddy_prompt_was_attempted(state, &heartbeat->prompt)) {
        buddy_invalidate_prompt(state);
    } else {
        buddy_note_prompt_shown(state, &heartbeat->prompt, now_ms);
        state->prompt = heartbeat->prompt;
        state->prompt_connection_generation = connection_generation;
        state->approval_locked = false;
        state->permission_delivery = BUDDY_PERMISSION_DELIVERY_NONE;
    }
    state->last_heartbeat_ms = now_ms;
    state->running = heartbeat->running;
    state->total = heartbeat->total;
    state->waiting = heartbeat->waiting;
    state->tokens = heartbeat->tokens;
    state->tokens_today = heartbeat->tokens_today;
    buddy_copy(state->message, sizeof(state->message), heartbeat->message);
    buddy_copy_entries(state->entries, heartbeat->entries);

    if (level > state->highest_celebrated_level) {
        state->highest_celebrated_level = level;
        state->settings.highest_celebrated_level = level;
        state->temporary_character = BUDDY_CHARACTER_CELEBRATE;
        state->temporary_until_ms = now_ms + BUDDY_CELEBRATION_ANIMATION_MS;
        if (action != NULL) {
            action->type = BUDDY_ACTION_SETTINGS;
            action->settings = state->settings;
        }
        return;
    }
    buddy_set_ui_refresh(action);
}

static void buddy_apply_prompt(buddy_state_t *state, const buddy_prompt_t *prompt,
                               uint32_t connection_generation, uint64_t now_ms,
                               buddy_action_t *action)
{
    if (!prompt->connected || !buddy_prompt_id_is_valid(prompt) ||
        buddy_prompt_was_attempted(state, prompt)) {
        return;
    }

    buddy_note_prompt_shown(state, prompt, now_ms);
    state->prompt = *prompt;
    state->prompt_connection_generation = connection_generation;
    state->approval_locked = false;
    state->permission_delivery = BUDDY_PERMISSION_DELIVERY_NONE;
    state->connected = true;
    state->connection = BUDDY_CONNECTION_CONNECTED;
    state->heartbeat_stale = false;
    state->last_heartbeat_ms = now_ms;
    state->running = prompt->running;
    buddy_set_ui_refresh(action);
}

static bool buddy_observed_prompt_matches(const buddy_event_t *event, const buddy_prompt_t *prompt)
{
    if (!event->has_observed_prompt_id) {
        return true;
    }
    if (event->observed_prompt_id_truncated ||
        !buddy_string_matches_length(event->observed_prompt_id,
                                     sizeof(event->observed_prompt_id),
                                     event->observed_prompt_id_length)) {
        return false;
    }
    return event->observed_prompt_id_length == prompt->id_length &&
           memcmp(event->observed_prompt_id, prompt->id, prompt->id_length) == 0;
}

static void buddy_decide_prompt(buddy_state_t *state, const buddy_event_t *event,
                                buddy_permission_decision_t decision,
                                buddy_action_t *action)
{
    if (state->approval_locked || state->prompt.id[0] == '\0' || state->heartbeat_stale ||
        !buddy_prompt_id_is_valid(&state->prompt) ||
        !buddy_observed_prompt_matches(event, &state->prompt)) {
        return;
    }

    if (action != NULL) {
        action->type = BUDDY_ACTION_PERMISSION;
        buddy_copy(action->permission.id, sizeof(action->permission.id), state->prompt.id);
        buddy_copy(action->permission.tool, sizeof(action->permission.tool), state->prompt.tool);
        buddy_copy(action->permission.hint, sizeof(action->permission.hint), state->prompt.hint);
        action->permission.decision = decision;
        action->permission.connection_generation = state->prompt_connection_generation;
    }
    buddy_copy(state->last_attempted_prompt_id, sizeof(state->last_attempted_prompt_id),
               state->prompt.id);
    state->approval_locked = true;
    state->permission_delivery = BUDDY_PERMISSION_DELIVERY_SENDING;
}

static void buddy_apply_permission_result(buddy_state_t *state,
                                          const buddy_permission_result_event_t *result,
                                          uint64_t now_ms, buddy_action_t *action)
{
    if (state->permission_delivery != BUDDY_PERMISSION_DELIVERY_SENDING ||
        !buddy_string_matches_length(result->id, sizeof(result->id), result->id_length) ||
        !buddy_string_matches_length(state->last_attempted_prompt_id,
                                     sizeof(state->last_attempted_prompt_id),
                                     result->id_length) ||
        memcmp(state->last_attempted_prompt_id, result->id, result->id_length) != 0) {
        return;
    }
    if (result->success) {
        buddy_copy(state->last_successful_decision_id,
                   sizeof(state->last_successful_decision_id), result->id);
        state->permission_delivery = BUDDY_PERMISSION_DELIVERY_SENT;
        if (result->decision == BUDDY_PERMISSION_ONCE ||
            result->decision == BUDDY_PERMISSION_ALWAYS) {
            buddy_pet_add(&state->pet, BUDDY_PET_HAPPINESS, BUDDY_APPROVAL_HAPPINESS);
        }
        if (result->decision == BUDDY_PERMISSION_ONCE) {
            state->temporary_character = BUDDY_CHARACTER_HEART;
            state->temporary_until_ms = now_ms + BUDDY_HEART_ANIMATION_MS;
        }
    } else {
        state->permission_delivery = BUDDY_PERMISSION_DELIVERY_FAILED;
    }
    buddy_set_ui_refresh(action);
}

/* Local time from the last sync is 23:00-06:59. */
static bool buddy_is_night(const buddy_state_t *state, uint64_t now_ms)
{
    int64_t local;
    int64_t hour;

    if (state->epoch_seconds <= 0 || now_ms < state->time_received_ms) {
        return false;
    }
    local = state->epoch_seconds + state->timezone_offset_seconds +
            (int64_t)((now_ms - state->time_received_ms) / 1000U);
    hour = ((local % 86400) + 86400) % 86400 / 3600;
    return hour >= BUDDY_NIGHT_START_HOUR || hour < BUDDY_NIGHT_END_HOUR;
}

static buddy_pet_activity_t buddy_pet_activity(const buddy_state_t *state, uint64_t now_ms)
{
    if (!state->connected || state->heartbeat_stale || buddy_is_night(state, now_ms)) {
        return BUDDY_PET_SLEEPING;
    }
    return state->running > 0 ? BUDDY_PET_BUSY : BUDDY_PET_AWAKE;
}

/* Runs before the event, so the time since the last event counts with the
 * activity the pet had during it. */
static void buddy_update_pet(buddy_state_t *state, uint64_t now_ms)
{
    uint64_t seconds;

    /* now_ms counts from boot, so a state initialised later (unpair, factory
     * reset) starts its pet clock at its first event. */
    if (!state->pet_clock_started || now_ms < state->pet_updated_ms) {
        state->pet_clock_started = true;
        state->pet_updated_ms = now_ms;
        state->pet_saved_ms = now_ms;
    }
    seconds = (now_ms - state->pet_updated_ms) / 1000U;
    if (seconds > 0) {
        state->pet_updated_ms += seconds * 1000U;
        buddy_pet_elapse(&state->pet, seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds,
                         buddy_pet_activity(state, now_ms));
    }
    if (buddy_has_actionable_prompt(state) && now_ms >= state->prompt_shown_ms) {
        uint64_t due = (now_ms - state->prompt_shown_ms) / BUDDY_PROMPT_PENALTY_MS;

        if (due > state->prompt_penalties) {
            uint64_t missed = due - state->prompt_penalties;

            buddy_pet_add(&state->pet, BUDDY_PET_HAPPINESS, missed > 100U ? -100 : -(int)missed);
            state->prompt_penalties = due > UINT32_MAX ? UINT32_MAX : (uint32_t)due;
        }
    }
}

/* Synced UTC epoch now, or 0 before the first time sync. */
static uint32_t buddy_current_epoch(const buddy_state_t *state, uint64_t now_ms)
{
    int64_t epoch;

    if (state->epoch_seconds <= 0 || now_ms < state->time_received_ms) {
        return 0;
    }
    epoch = state->epoch_seconds + (int64_t)((now_ms - state->time_received_ms) / 1000U);
    return epoch > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)epoch;
}

static void buddy_pet_save_action(buddy_state_t *state, uint64_t now_ms, buddy_action_t *action)
{
    state->pet_saved_ms = now_ms;
    if (action != NULL) {
        action->type = BUDDY_ACTION_PET_SAVE;
        action->pet = buddy_pet_pack(&state->pet, buddy_current_epoch(state, now_ms));
    }
}

/* Time spent switched off counts as sleep, once per boot, capped at a day. */
static void buddy_pet_catch_up(buddy_state_t *state, uint64_t now_ms)
{
    int64_t boot_epoch = state->epoch_seconds - (int64_t)(now_ms / 1000U);
    int64_t gap = boot_epoch - (int64_t)state->pet_saved_epoch;

    if (state->pet_catch_up_done || state->epoch_seconds <= 0) {
        return;
    }
    state->pet_catch_up_done = true;
    if (state->pet_saved_epoch == 0U || gap <= 0) {
        return;
    }
    buddy_pet_elapse(&state->pet,
                     gap > (int64_t)BUDDY_PET_CATCH_UP_MAX_S ? BUDDY_PET_CATCH_UP_MAX_S
                                                            : (uint32_t)gap,
                     BUDDY_PET_SLEEPING);
}

void buddy_state_init(buddy_state_t *state, const buddy_settings_snapshot_t *settings)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->connection = BUDDY_CONNECTION_OFFLINE;
    state->page = BUDDY_PAGE_HOME;
    state->heartbeat_stale = true;
    state->brightness_level = 4;
    state->transcript_enabled = true;
    buddy_pet_init(&state->pet);
    if (settings != NULL && settings->pet != 0U &&
        !buddy_pet_unpack(settings->pet, &state->pet, &state->pet_saved_epoch)) {
        buddy_pet_init(&state->pet);
    }
    if (settings != NULL) {
        state->settings = *settings;
        state->highest_celebrated_level = settings->highest_celebrated_level;
        buddy_copy(state->name, sizeof(state->name), settings->name);
        buddy_copy(state->owner, sizeof(state->owner), settings->owner);
    }
    buddy_refresh_character(state, 0);
}

void buddy_state_reinit_keep_pet(buddy_state_t *state, const buddy_settings_snapshot_t *settings)
{
    buddy_pet_t pet;
    uint64_t updated_ms;
    uint64_t saved_ms;
    uint32_t saved_epoch;
    bool clock_started;
    bool catch_up_done;

    if (state == NULL) {
        return;
    }
    pet = state->pet;
    updated_ms = state->pet_updated_ms;
    saved_ms = state->pet_saved_ms;
    saved_epoch = state->pet_saved_epoch;
    clock_started = state->pet_clock_started;
    catch_up_done = state->pet_catch_up_done;
    buddy_state_init(state, settings);
    state->pet = pet;
    state->pet_updated_ms = updated_ms;
    state->pet_saved_ms = saved_ms;
    state->pet_saved_epoch = saved_epoch;
    state->pet_clock_started = clock_started;
    state->pet_catch_up_done = catch_up_done;
    buddy_refresh_character(state, 0);
}

/* The approval guard counts from when the prompt became visible: a hidden
 * prompt (screen off, confirmation or passkey on top) restarts it. */
static void buddy_update_prompt_visibility(buddy_state_t *state, uint64_t now_ms)
{
    bool visible = buddy_has_actionable_prompt(state) && !state->screen_off &&
                   !state->confirmation_pending && state->confirmation == BUDDY_CONFIRM_NONE &&
                   !state->passkey_visible;

    if (visible && !state->prompt_visible) {
        state->prompt_visible_ms = now_ms;
    }
    state->prompt_visible = visible;
}

void buddy_state_reduce(buddy_state_t *state, const buddy_event_t *event,
                        uint64_t now_ms, buddy_action_t *action)
{
    if (action != NULL) {
        memset(action, 0, sizeof(*action));
    }
    if (state == NULL || event == NULL) {
        return;
    }

    buddy_update_pet(state, now_ms);
    buddy_clear_stale_prompt(state, now_ms);

    switch (event->type) {
    case BUDDY_EVENT_HEARTBEAT:
        buddy_apply_heartbeat(state, &event->heartbeat, event->ble.connection_generation,
                              now_ms, action);
        break;
    case BUDDY_EVENT_PROMPT:
        buddy_apply_prompt(state, &event->prompt, event->ble.connection_generation,
                           now_ms, action);
        break;
    case BUDDY_EVENT_TIME:
        state->epoch_seconds = event->time.epoch_seconds;
        state->timezone_offset_seconds = event->time.timezone_offset_seconds;
        state->time_received_ms = now_ms;
        buddy_pet_catch_up(state, now_ms);
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_NAME:
        buddy_copy(state->name, sizeof(state->name), event->command.value);
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_OWNER:
        buddy_copy(state->owner, sizeof(state->owner), event->command.value);
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_STATUS:
        buddy_copy(state->message, sizeof(state->message), event->command.value);
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_STATUS_REQUEST:
        if (action != NULL) {
            action->type = BUDDY_ACTION_STATUS;
            action->connection_generation = event->ble.connection_generation;
        }
        break;
    case BUDDY_EVENT_UNPAIR_CONFIRMATION:
        if (state->confirmation == BUDDY_CONFIRM_NONE) {
            buddy_open_confirmation(state, BUDDY_CONFIRM_UNPAIR, true,
                                    event->ble.connection_generation, action);
        }
        break;
    case BUDDY_EVENT_BLE_CONNECTED:
        if (state->ble_connection_generation != event->ble.connection_generation) {
            buddy_invalidate_prompt(state);
            state->passkey_visible = false;
            state->connected = false;
            state->heartbeat_stale = true;
            if (state->confirmation_acknowledge) {
                buddy_close_confirmation(state);
            }
        }
        state->ble_connection_generation = event->ble.connection_generation;
        state->ble_connected = true;
        state->ble_encrypted = false;
        if (state->confirmation == BUDDY_CONFIRM_NONE) {
            state->connection = BUDDY_CONNECTION_PAIRING;
        }
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_BLE_DISCONNECTED:
        state->ble_connection_generation = event->ble.connection_generation;
        state->ble_connected = false;
        state->ble_encrypted = false;
        state->passkey_visible = false;
        state->connected = false;
        state->heartbeat_stale = true;
        buddy_clear_logical_session(state);
        if (state->confirmation_acknowledge) {
            buddy_close_confirmation(state);
        } else if (state->confirmation == BUDDY_CONFIRM_NONE) {
            state->connection = BUDDY_CONNECTION_OFFLINE;
        }
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_BLE_PASSKEY:
        if (event->ble.connection_generation != state->ble_connection_generation) {
            break;
        }
        state->passkey = event->ble.passkey;
        state->passkey_visible = true;
        state->connection = BUDDY_CONNECTION_PAIRING;
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_BLE_ENCRYPTION:
        if (event->ble.connection_generation != state->ble_connection_generation) {
            break;
        }
        state->ble_encrypted = event->ble.secure;
        if (event->ble.secure) {
            state->passkey_visible = false;
            if (state->confirmation == BUDDY_CONFIRM_NONE) {
                state->connection = state->connected && !state->heartbeat_stale
                                        ? BUDDY_CONNECTION_CONNECTED
                                        : BUDDY_CONNECTION_OFFLINE;
            }
        } else if (state->ble_connected && state->confirmation == BUDDY_CONFIRM_NONE) {
            state->connection = BUDDY_CONNECTION_PAIRING;
        }
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_BOND_DELETE_RESULT:
        buddy_copy(state->message, sizeof(state->message),
                   event->ble.success ? "Unpaired" : "Unpair failed");
        buddy_set_ui_refresh(action);
        break;
    case BUDDY_EVENT_PERMISSION_SEND_RESULT:
        buddy_apply_permission_result(state, &event->permission_result, now_ms, action);
        break;
    case BUDDY_EVENT_KEY_CLICK: {
        /* A run of OK presses (pats) never approves: OK needs a second of quiet. */
        bool ok_quiet = !state->has_last_ok ||
                        now_ms >= state->last_ok_ms + BUDDY_APPROVAL_GUARD_MS;

        if (event->key == BUDDY_KEY_OK) {
            state->has_last_ok = true;
            state->last_ok_ms = now_ms;
        }
        if (state->screen_off) {
            state->screen_off = false;
            if (action != NULL) {
                action->type = BUDDY_ACTION_DISPLAY_BACKLIGHT;
                action->brightness_percent =
                    (uint8_t)(20U + state->brightness_level * 20U);
            }
            break;
        }
        if (state->confirmation != BUDDY_CONFIRM_NONE && event->key == BUDDY_KEY_OK) {
            buddy_confirmation_t confirmation = state->confirmation;
            bool acknowledge = state->confirmation_acknowledge;
            uint32_t connection_generation = state->confirmation_connection_generation;

            buddy_close_confirmation(state);
            if (action != NULL) {
                action->type = confirmation == BUDDY_CONFIRM_UNPAIR
                                   ? BUDDY_ACTION_UNPAIR_CONFIRMED
                                   : BUDDY_ACTION_FACTORY_RESET_CONFIRMED;
                action->confirmation_acknowledge = acknowledge;
                action->connection_generation = connection_generation;
            }
            break;
        }
        if (state->confirmation != BUDDY_CONFIRM_NONE && event->key == BUDDY_KEY_DOWN) {
            buddy_close_confirmation(state);
            buddy_set_ui_refresh(action);
            break;
        }
        if (state->confirmation != BUDDY_CONFIRM_NONE) {
            break;
        }
        if (buddy_has_actionable_prompt(state) && event->key == BUDDY_KEY_OK) {
            /* A press in the prompt's first visible second was meant for
             * something else. */
            if (state->prompt_visible && ok_quiet &&
                now_ms >= state->prompt_visible_ms + BUDDY_APPROVAL_GUARD_MS) {
                buddy_decide_prompt(state, event, BUDDY_PERMISSION_ONCE, action);
            }
        } else if (buddy_has_actionable_prompt(state) && event->key == BUDDY_KEY_DOWN) {
            buddy_decide_prompt(state, event, BUDDY_PERMISSION_DENY, action);
        } else if (buddy_has_actionable_prompt(state) && event->key == BUDDY_KEY_UP) {
            if (action != NULL) {
                action->type = BUDDY_ACTION_UI_SCROLL;
                action->scroll_delta = -48;
            }
        } else if (!buddy_has_prompt(state)) {
            buddy_normal_click(state, event->key, now_ms, action);
        }
        break;
    }
    case BUDDY_EVENT_KEY_LONG:
        if (event->key == BUDDY_KEY_OK && state->confirmation == BUDDY_CONFIRM_NONE &&
            !buddy_has_prompt(state)) {
            state->menu_open = !state->menu_open;
            state->menu_selection = BUDDY_MENU_SETTINGS;
            state->reset_open = false;
            buddy_set_ui_refresh(action);
        }
        break;
    case BUDDY_EVENT_TICK:
        buddy_set_ui_refresh(action);
        if (state->care_message[0] != '\0' && now_ms >= state->care_message_until_ms) {
            state->care_message[0] = '\0';
        }
        if (now_ms - state->pet_saved_ms >= BUDDY_PET_SAVE_INTERVAL_MS) {
            buddy_pet_save_action(state, now_ms, action);
        }
        break;
    case BUDDY_EVENT_NONE:
        break;
    }

    buddy_update_prompt_visibility(state, now_ms);
    buddy_refresh_character(state, now_ms);
}

void buddy_state_snapshot(const buddy_state_t *state, buddy_ui_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->connection = state->connection;
    snapshot->character = state->character;
    snapshot->page = state->page;
    snapshot->running = state->running;
    snapshot->total = state->total;
    snapshot->waiting = state->waiting;
    snapshot->tokens = state->tokens;
    snapshot->tokens_today = state->tokens_today;
    snapshot->epoch_seconds = state->epoch_seconds;
    snapshot->timezone_offset_seconds = state->timezone_offset_seconds;
    snapshot->time_received_ms = state->time_received_ms;
    snapshot->heartbeat_stale = state->heartbeat_stale;
    snapshot->confirmation_pending = state->confirmation_pending;
    snapshot->confirmation = state->confirmation;
    snapshot->settings_selection = state->settings_selection;
    snapshot->reset_selection = state->reset_selection;
    snapshot->menu_selection = state->menu_selection;
    snapshot->care_selection = state->care_selection;
    snapshot->info_page = state->info_page;
    snapshot->menu_open = state->menu_open;
    snapshot->reset_open = state->reset_open;
    snapshot->transcript_enabled = state->transcript_enabled;
    snapshot->brightness_level = state->brightness_level;
    snapshot->screen_off = state->screen_off;
    snapshot->species = state->species;
    snapshot->pack_available = state->pack_available;
    snapshot->approval_locked = state->approval_locked;
    snapshot->permission_delivery = state->permission_delivery;
    snapshot->ble_connected = state->ble_connected;
    snapshot->ble_encrypted = state->ble_encrypted;
    snapshot->ble_enabled = state->settings.ble_enabled;
    snapshot->battery_available = state->battery_available;
    snapshot->passkey_visible = state->passkey_visible;
    snapshot->prompt_connection_generation = state->prompt_connection_generation;
    snapshot->confirmation_connection_generation =
        state->confirmation_connection_generation;
    snapshot->passkey = state->passkey;
    snapshot->battery_percent = state->battery_percent;
    snapshot->battery_mv = state->battery_mv;
    snapshot->pet_fullness = state->pet.fullness;
    snapshot->pet_happiness = state->pet.happiness;
    snapshot->pet_energy = state->pet.energy;
    snapshot->mood = buddy_pet_mood(&state->pet);
    buddy_copy(snapshot->care_message, sizeof(snapshot->care_message), state->care_message);
    buddy_copy(snapshot->name, sizeof(snapshot->name), state->name);
    buddy_copy(snapshot->owner, sizeof(snapshot->owner), state->owner);
    buddy_copy(snapshot->time, sizeof(snapshot->time), state->time);
    buddy_copy(snapshot->message, sizeof(snapshot->message), state->message);
    buddy_copy_entries(snapshot->entries, state->entries);
    buddy_copy(snapshot->prompt_id, sizeof(snapshot->prompt_id), state->prompt.id);
    buddy_copy(snapshot->prompt_tool, sizeof(snapshot->prompt_tool), state->prompt.tool);
    buddy_copy(snapshot->prompt_hint, sizeof(snapshot->prompt_hint), state->prompt.hint);
}

void buddy_state_set_pack(buddy_state_t *state, bool available, bool select)
{
    if (state == NULL) {
        return;
    }
    state->pack_available = available;
    if (available && select) {
        state->species = BUDDY_SPECIES_PACK;
    } else if (!available && state->species >= BUDDY_SPECIES_BUILTIN_COUNT) {
        state->species = 0;
    }
}
