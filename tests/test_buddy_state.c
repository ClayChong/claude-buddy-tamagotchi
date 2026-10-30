#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "buddy_protocol.h"
#include "buddy_state.h"

static buddy_event_t test_prompt_event(const char *id, const char *tool,
                                       const char *hint, unsigned running,
                                       unsigned connected)
{
    buddy_event_t event = {0};
    size_t id_length = strlen(id);

    event.type = BUDDY_EVENT_PROMPT;
    event.prompt.id_length = id_length;
    event.prompt.id_truncated = id_length >= sizeof(event.prompt.id);
    snprintf(event.prompt.id, sizeof(event.prompt.id), "%s", id);
    snprintf(event.prompt.tool, sizeof(event.prompt.tool), "%s", tool);
    snprintf(event.prompt.hint, sizeof(event.prompt.hint), "%s", hint);
    event.prompt.running = running;
    event.prompt.connected = connected != 0;
    return event;
}

static void test_set_observed_prompt_id(buddy_event_t *event, const char *id)
{
    size_t id_length = strlen(id);

    event->has_observed_prompt_id = true;
    event->observed_prompt_id_length = id_length;
    event->observed_prompt_id_truncated = id_length >= sizeof(event->observed_prompt_id);
    snprintf(event->observed_prompt_id, sizeof(event->observed_prompt_id), "%s", id);
}

static buddy_event_t test_permission_result_event(const char *id,
                                                   buddy_permission_decision_t decision,
                                                   bool success)
{
    buddy_event_t event = {0};

    event.type = BUDDY_EVENT_PERMISSION_SEND_RESULT;
    event.permission_result.id_length = strlen(id);
    snprintf(event.permission_result.id, sizeof(event.permission_result.id), "%s", id);
    event.permission_result.decision = decision;
    event.permission_result.success = success;
    return event;
}

static buddy_event_t test_heartbeat_event(uint64_t tokens, unsigned running)
{
    buddy_event_t event = {0};

    event.type = BUDDY_EVENT_HEARTBEAT;
    event.heartbeat.connected = true;
    event.heartbeat.total = running + 2U;
    event.heartbeat.running = running;
    event.heartbeat.waiting = 1U;
    event.heartbeat.tokens = tokens;
    event.heartbeat.tokens_today = tokens / 2U;
    snprintf(event.heartbeat.message, sizeof(event.heartbeat.message), "%s", "Working");
    snprintf(event.heartbeat.entries[0], sizeof(event.heartbeat.entries[0]), "%s", "entry");
    return event;
}

static void test_offline_initialization(void)
{
    buddy_state_t state;
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    buddy_state_snapshot(&state, &snapshot);

    assert(state.connection == BUDDY_CONNECTION_OFFLINE);
    assert(snapshot.character == BUDDY_CHARACTER_SLEEP);
    assert(snapshot.page == BUDDY_PAGE_HOME);
}

static void test_heartbeat_mapping(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(1234, 2);
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);
    buddy_state_snapshot(&state, &snapshot);

    assert(action.type == BUDDY_ACTION_UI_REFRESH);
    assert(state.connection == BUDDY_CONNECTION_CONNECTED);
    assert(state.tokens == 1234);
    assert(state.running == 2);
    assert(snapshot.total == 4);
    assert(snapshot.waiting == 1);
    assert(snapshot.character == BUDDY_CHARACTER_BUSY);
}

static void test_character_priority(void)
{
    struct priority_case {
        buddy_connection_t connection;
        bool has_prompt;
        bool temporary;
        unsigned running;
        buddy_character_t want;
    } cases[] = {
        {BUDDY_CONNECTION_PAIRING, true, true, 1, BUDDY_CHARACTER_PAIRING},
        {BUDDY_CONNECTION_CONFIRMING, true, true, 1, BUDDY_CHARACTER_CONFIRMATION},
        {BUDDY_CONNECTION_CONNECTED, true, true, 1, BUDDY_CHARACTER_ATTENTION},
        {BUDDY_CONNECTION_CONNECTED, false, true, 1, BUDDY_CHARACTER_HEART},
        {BUDDY_CONNECTION_CONNECTED, false, false, 1, BUDDY_CHARACTER_BUSY},
        {BUDDY_CONNECTION_CONNECTED, false, false, 0, BUDDY_CHARACTER_IDLE},
        {BUDDY_CONNECTION_OFFLINE, false, false, 0, BUDDY_CHARACTER_SLEEP},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        buddy_state_t state;
        buddy_ui_snapshot_t snapshot;

        buddy_state_init(&state, NULL);
        state.connection = cases[index].connection;
        state.connected = cases[index].connection == BUDDY_CONNECTION_CONNECTED;
        state.heartbeat_stale = false;
        state.running = cases[index].running;
        if (cases[index].has_prompt) {
            snprintf(state.prompt.id, sizeof(state.prompt.id), "%s", "req-1");
        }
        if (cases[index].temporary) {
            state.temporary_character = BUDDY_CHARACTER_HEART;
            state.temporary_until_ms = 1001;
        }

        buddy_event_t tick = {.type = BUDDY_EVENT_TICK};
        buddy_state_reduce(&state, &tick, 1000, NULL);
        buddy_state_snapshot(&state, &snapshot);
        assert(snapshot.character == cases[index].want);
    }
}

static void test_timeout_clears_prompt(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(1200, 0);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_state_reduce(&state, &prompt, 1001, &action);
    memset(&action, 0, sizeof(action));
    buddy_state_reduce(&state, &tick, 31001, &action);

    assert(action.type == BUDDY_ACTION_UI_REFRESH);
    assert(state.heartbeat_stale);
    assert(state.prompt.id[0] == '\0');
    assert(!state.connected);
    assert(state.connection == BUDDY_CONNECTION_OFFLINE);
    assert(state.total == 0);
    assert(state.running == 0);
    assert(state.waiting == 0);
    assert(state.tokens == 0);
    assert(state.tokens_today == 0);
    assert(state.message[0] == '\0');
    assert(state.entries[0][0] == '\0');
    assert(state.character == BUDDY_CHARACTER_SLEEP);
}

static void test_heartbeat_does_not_overwrite_persisted_identity(void)
{
    buddy_settings_snapshot_t settings = {0};
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(1, 0);

    snprintf(settings.name, sizeof(settings.name), "%s", "Clawd");
    snprintf(settings.owner, sizeof(settings.owner), "%s", "Felix");
    buddy_state_init(&state, &settings);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);

    assert(strcmp(state.name, "Clawd") == 0);
    assert(strcmp(state.owner, "Felix") == 0);
}

static void test_token_boundaries_celebrate_once(void)
{
    buddy_state_t state;
    buddy_settings_snapshot_t settings = {.highest_celebrated_level = 0};
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(49999, 0);

    buddy_state_init(&state, &settings);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);

    heartbeat = test_heartbeat_event(50000, 0);
    buddy_state_reduce(&state, &heartbeat, 1001, &action);
    assert(action.type == BUDDY_ACTION_SETTINGS);
    assert(action.settings.highest_celebrated_level == 1);
    assert(state.character == BUDDY_CHARACTER_CELEBRATE);

    memset(&action, 0, sizeof(action));
    buddy_state_reduce(&state, &heartbeat, 1002, &action);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);
    assert(state.highest_celebrated_level == 1);

    heartbeat = test_heartbeat_event(100000, 0);
    buddy_state_reduce(&state, &heartbeat, 1003, &action);
    assert(action.type == BUDDY_ACTION_SETTINGS);
    assert(action.settings.highest_celebrated_level == 2);
}

static void test_persisted_celebration_level_is_not_replayed(void)
{
    buddy_state_t state;
    buddy_settings_snapshot_t settings = {.highest_celebrated_level = 2};
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(100000, 0);

    buddy_state_init(&state, &settings);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);

    assert(action.type == BUDDY_ACTION_UI_REFRESH);
    assert(state.highest_celebrated_level == 2);
}

static void test_approval_locks_until_a_new_prompt(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_ui_snapshot_t snapshot;
    buddy_event_t prompt;
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);

    prompt = test_prompt_event("req-1", "Bash", "git push", 1, 1);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    assert(state.character == BUDDY_CHARACTER_ATTENTION);

    buddy_state_reduce(&state, &approve, 2000, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(strcmp(action.permission.id, "req-1") == 0);
    assert(action.permission.decision == BUDDY_PERMISSION_ONCE);
    assert(state.character != BUDDY_CHARACTER_HEART);
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.approval_locked);
    assert(strcmp(snapshot.prompt_id, "req-1") == 0);

    memset(&action, 0, sizeof(action));
    buddy_state_reduce(&state, &approve, 2100, &action);
    assert(action.type == BUDDY_ACTION_NONE);

    prompt = test_prompt_event("req-2", "Bash", "git status", 1, 1);
    buddy_state_reduce(&state, &prompt, 2200, &action);
    buddy_state_snapshot(&state, &snapshot);
    assert(!snapshot.approval_locked);
    buddy_state_reduce(&state, &approve, 3200, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(strcmp(action.permission.id, "req-2") == 0);
}

static void test_denial_locks_and_emits_exactly_once(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-deny", "Bash", "rm guarded", 1, 1);
    buddy_event_t deny = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &deny, 1001, &action);

    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(strcmp(action.permission.id, "req-deny") == 0);
    assert(action.permission.decision == BUDDY_PERMISSION_DENY);
    assert(state.approval_locked);

    buddy_state_reduce(&state, &deny, 1002, &action);
    assert(action.type == BUDDY_ACTION_NONE);
}

static void test_permission_action_is_bound_to_the_prompt_connection(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-generation", "Bash", "git push", 1, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    prompt.ble.connection_generation = 7;
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);

    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(action.permission.connection_generation == 7);
}

static void test_permission_success_moves_from_attempted_to_successful_state(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_ui_snapshot_t snapshot;
    buddy_event_t prompt = test_prompt_event("req-success", "Bash", "git push", 1, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_event_t result = test_permission_result_event(
        "req-success", BUDDY_PERMISSION_ONCE, true);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(strcmp(state.last_attempted_prompt_id, "req-success") == 0);
    assert(state.last_successful_decision_id[0] == '\0');
    assert(state.permission_delivery == BUDDY_PERMISSION_DELIVERY_SENDING);
    assert(state.character != BUDDY_CHARACTER_HEART);

    buddy_state_reduce(&state, &result, 2001, &action);
    buddy_state_snapshot(&state, &snapshot);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);
    assert(strcmp(state.last_successful_decision_id, "req-success") == 0);
    assert(snapshot.permission_delivery == BUDDY_PERMISSION_DELIVERY_SENT);
    assert(state.character == BUDDY_CHARACTER_HEART);
}

static void test_permission_failure_is_visible_and_same_id_replay_stays_locked(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_ui_snapshot_t snapshot;
    buddy_event_t prompt = test_prompt_event("req-failed", "Bash", "git push", 1, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_event_t failure = test_permission_result_event(
        "req-failed", BUDDY_PERMISSION_ONCE, false);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);
    buddy_state_reduce(&state, &failure, 2001, &action);
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.permission_delivery == BUDDY_PERMISSION_DELIVERY_FAILED);
    assert(strcmp(state.last_attempted_prompt_id, "req-failed") == 0);
    assert(state.last_successful_decision_id[0] == '\0');
    assert(state.approval_locked);
    assert(state.character != BUDDY_CHARACTER_HEART);

    buddy_state_reduce(&state, &prompt, 2002, &action);
    buddy_state_reduce(&state, &approve, 3002, &action);
    assert(action.type == BUDDY_ACTION_NONE);
    assert(state.permission_delivery == BUDDY_PERMISSION_DELIVERY_FAILED);
}

static void test_denial_success_and_ticks_never_show_heart(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-denied", "Bash", "rm guarded", 1, 1);
    buddy_event_t deny = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};
    buddy_event_t result = test_permission_result_event(
        "req-denied", BUDDY_PERMISSION_DENY, true);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &deny, 1001, &action);
    assert(state.permission_delivery == BUDDY_PERMISSION_DELIVERY_SENDING);
    assert(state.character != BUDDY_CHARACTER_HEART);
    buddy_state_reduce(&state, &result, 1002, &action);
    buddy_state_reduce(&state, &tick, 4000, &action);
    assert(state.permission_delivery == BUDDY_PERMISSION_DELIVERY_SENT);
    assert(state.character == BUDDY_CHARACTER_BUSY);
}

static void test_successful_approval_heart_expires_at_five_seconds(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-heart", "Read", "README", 1, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_event_t result = test_permission_result_event(
        "req-heart", BUDDY_PERMISSION_ONCE, true);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);
    buddy_state_reduce(&state, &result, 2001, &action);
    buddy_state_reduce(&state, &tick, 7000, &action);
    assert(state.character == BUDDY_CHARACTER_HEART);
    buddy_state_reduce(&state, &tick, 7001, &action);
    assert(state.character == BUDDY_CHARACTER_BUSY);
}

static void test_heartbeat_prompt_snapshot_clears_or_preserves_approval_lock(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_ui_snapshot_t snapshot;
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);
    assert(state.approval_locked);

    heartbeat.heartbeat.connected = true;
    buddy_state_reduce(&state, &heartbeat, 2001, &action);
    buddy_state_snapshot(&state, &snapshot);
    assert(!snapshot.approval_locked);
    assert(snapshot.prompt_id[0] == '\0');

    heartbeat.heartbeat.prompt = prompt.prompt;
    buddy_state_reduce(&state, &heartbeat, 2002, &action);
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.prompt_id[0] == '\0');
    /* With no prompt on screen, OK pats the buddy instead of answering. */
    buddy_state_reduce(&state, &approve, 3002, &action);
    assert(action.type != BUDDY_ACTION_PERMISSION);

    heartbeat.heartbeat.prompt = test_prompt_event("req-2", "Read", "README", 0, 1).prompt;
    buddy_state_reduce(&state, &heartbeat, 3003, &action);
    buddy_state_snapshot(&state, &snapshot);
    assert(!snapshot.approval_locked);
    assert(strcmp(snapshot.prompt_id, "req-2") == 0);
    buddy_state_reduce(&state, &approve, 4003, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(strcmp(action.permission.id, "req-2") == 0);
}

static void test_ui_snapshot_runtime_indicators_default_off(void)
{
    buddy_state_t state;
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    buddy_state_snapshot(&state, &snapshot);

    assert(!snapshot.ble_connected);
    assert(!snapshot.ble_encrypted);
    assert(!snapshot.battery_available);
    assert(snapshot.battery_percent == 0);
    assert(snapshot.battery_mv == 0);

    state.ble_connected = true;
    state.ble_encrypted = true;
    state.battery_available = true;
    state.battery_percent = 73;
    state.battery_mv = 3875;
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.ble_connected);
    assert(snapshot.ble_encrypted);
    assert(snapshot.battery_available);
    assert(snapshot.battery_percent == 73);
    assert(snapshot.battery_mv == 3875);
}

static void test_absent_prompt_is_ignored(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &approve, 1001, &action);
    /* OK with no prompt pats the buddy; no permission is sent. */
    assert(action.type != BUDDY_ACTION_PERMISSION);
}

static void test_timeout_stale_prompt_is_ignored(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);
    buddy_state_reduce(&state, &prompt, 1001, &action);
    buddy_state_reduce(&state, &tick, 31001, &action);
    buddy_state_reduce(&state, &approve, 31002, &action);
    /* The stale prompt is gone, so OK pats the buddy; no permission is sent. */
    assert(action.type != BUDDY_ACTION_PERMISSION);
}

static void test_standalone_prompt_refreshes_liveness_clock(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-late", "Bash", "git status", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 60000, &action);
    buddy_state_reduce(&state, &approve, 61000, &action);

    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(strcmp(action.permission.id, "req-late") == 0);
}

static void test_disconnect_then_reconnect_does_not_restore_prompt(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);
    buddy_state_reduce(&state, &prompt, 1001, &action);

    heartbeat.heartbeat.connected = false;
    buddy_state_reduce(&state, &heartbeat, 1002, &action);
    heartbeat.heartbeat.connected = true;
    buddy_state_reduce(&state, &heartbeat, 1003, &action);
    buddy_state_reduce(&state, &approve, 2003, &action);

    assert(state.prompt.id[0] == '\0');
    assert(action.type != BUDDY_ACTION_PERMISSION); /* OK with no prompt pats. */
}

static void test_mismatched_observed_prompt_is_ignored(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    test_set_observed_prompt_id(&approve, "req-2");
    buddy_state_reduce(&state, &approve, 2000, &action);

    assert(action.type == BUDDY_ACTION_NONE);
    assert(strcmp(state.prompt.id, "req-1") == 0);
}

static void test_nonterminated_prompt_id_is_ignored(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    memset(prompt.prompt.id, 'a', sizeof(prompt.prompt.id));
    prompt.prompt.id_length = sizeof(prompt.prompt.id);
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);

    assert(action.type != BUDDY_ACTION_PERMISSION); /* OK with no prompt pats. */
}

static void test_truncated_prompt_id_is_ignored(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    prompt.prompt.id_truncated = true;
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);

    assert(action.type != BUDDY_ACTION_PERMISSION); /* OK with no prompt pats. */
}

static void test_long_ok_opens_settings(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t long_ok = {.type = BUDDY_EVENT_KEY_LONG, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &long_ok, 1000, &action);

    assert(state.menu_open);
    assert(state.menu_selection == BUDDY_MENU_SETTINGS);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);
}

static void test_original_screen_and_menu_navigation(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t up = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_UP};
    buddy_event_t down = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_event_t menu = {.type = BUDDY_EVENT_KEY_LONG, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &up, 1, &action);
    assert(state.page == BUDDY_PAGE_PET);
    buddy_state_reduce(&state, &down, 2, &action);
    assert(state.care_selection == BUDDY_CARE_PLAY);
    buddy_state_reduce(&state, &up, 3, &action);
    assert(state.page == BUDDY_PAGE_INFO);
    buddy_state_reduce(&state, &down, 4, &action);
    assert(state.info_page == 1);
    buddy_state_reduce(&state, &up, 5, &action);
    assert(state.page == BUDDY_PAGE_HOME);

    buddy_state_reduce(&state, &menu, 6, &action);
    buddy_state_reduce(&state, &down, 7, &action);
    assert(state.menu_selection == BUDDY_MENU_TURN_OFF);
    buddy_state_reduce(&state, &up, 8, &action);
    buddy_state_reduce(&state, &ok, 9, &action);
    assert(!state.menu_open);
    assert(state.page == BUDDY_PAGE_SETTINGS);
}

static void test_protocol_command_events_refresh_the_display(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t event = {.type = BUDDY_EVENT_NAME};

    buddy_state_init(&state, NULL);
    snprintf(event.command.value, sizeof(event.command.value), "%s", "Buddy");
    buddy_state_reduce(&state, &event, 1000, &action);
    assert(strcmp(state.name, "Buddy") == 0);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);

    event.type = BUDDY_EVENT_OWNER;
    snprintf(event.command.value, sizeof(event.command.value), "%s", "Claude");
    buddy_state_reduce(&state, &event, 1001, &action);
    assert(strcmp(state.owner, "Claude") == 0);

    event.type = BUDDY_EVENT_STATUS;
    snprintf(event.command.value, sizeof(event.command.value), "%s", "Ready");
    buddy_state_reduce(&state, &event, 1002, &action);
    assert(strcmp(state.message, "Ready") == 0);

    event.type = BUDDY_EVENT_TIME;
    event.time.epoch_seconds = 1775731234;
    event.time.timezone_offset_seconds = -25200;
    buddy_state_reduce(&state, &event, 1003, &action);
    assert(state.epoch_seconds == 1775731234);
    assert(state.timezone_offset_seconds == -25200);
    assert(state.time_received_ms == 1003);

    buddy_ui_snapshot_t time_snapshot;
    buddy_state_snapshot(&state, &time_snapshot);
    assert(time_snapshot.time_received_ms == 1003);

    event.type = BUDDY_EVENT_UNPAIR_CONFIRMATION;
    buddy_state_reduce(&state, &event, 1004, &action);
    assert(state.connection == BUDDY_CONNECTION_CONFIRMING);
    assert(state.character == BUDDY_CHARACTER_CONFIRMATION);
}

static void test_status_request_does_not_overwrite_message(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t event = {.type = BUDDY_EVENT_STATUS_REQUEST};

    buddy_state_init(&state, NULL);
    event.ble.connection_generation = 11;
    snprintf(state.message, sizeof(state.message), "%s", "Keep this");
    buddy_state_reduce(&state, &event, 1000, &action);

    assert(action.type == BUDDY_ACTION_STATUS);
    assert(action.connection_generation == 11);
    assert(strcmp(state.message, "Keep this") == 0);
}

static void test_unpair_confirmation_survives_a_heartbeat(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t unpair = {.type = BUDDY_EVENT_UNPAIR_CONFIRMATION};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &unpair, 1000, &action);
    buddy_state_reduce(&state, &heartbeat, 1001, &action);

    assert(state.confirmation_pending);
    assert(state.connection == BUDDY_CONNECTION_CONNECTED);
    assert(state.character == BUDDY_CHARACTER_CONFIRMATION);
}

static void test_unpair_confirmation_ok_emits_explicit_action(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t unpair = {.type = BUDDY_EVENT_UNPAIR_CONFIRMATION};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &unpair, 1000, &action);
    buddy_state_reduce(&state, &heartbeat, 1001, &action);
    buddy_state_reduce(&state, &ok, 2001, &action);

    assert(!state.confirmation_pending);
    assert(action.type == BUDDY_ACTION_UNPAIR_CONFIRMED);
}

static void test_unpair_confirmation_down_cancels_without_action(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t unpair = {.type = BUDDY_EVENT_UNPAIR_CONFIRMATION};
    buddy_event_t down = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &unpair, 1000, &action);
    buddy_state_reduce(&state, &down, 1001, &action);

    assert(!state.confirmation_pending);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);
}

static void test_parsed_heartbeat_approval_serializes_permission(void)
{
    const char *json =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"entries\":[],\"tokens\":0,\"tokens_today\":0,\"prompt\":{"
        "\"id\":\"req-e2e\",\"tool\":\"Bash\",\"hint\":\"git status\"}}";
    buddy_state_t state;
    buddy_event_t heartbeat = {0};
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_action_t action = {0};
    char output[160];

    assert(buddy_protocol_parse(json, strlen(json), &heartbeat) == BUDDY_EVENT_HEARTBEAT);
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);

    assert(action.type == BUDDY_ACTION_PERMISSION);
    assert(buddy_protocol_permission_json(output, sizeof(output), action.permission.id,
                                          action.permission.decision) > 0);
    assert(strcmp(output,
                  "{\"cmd\":\"permission\",\"id\":\"req-e2e\",\"decision\":\"once\"}\n") ==
           0);
}

static void test_normal_navigation_and_approval_scroll_are_distinct(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t down = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};
    buddy_event_t up = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_UP};
    buddy_event_t prompt = test_prompt_event("req-scroll", "Read", "long hint", 0, 1);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &down, 1000, &action);
    assert(state.page == BUDDY_PAGE_HOME);
    assert(action.type == BUDDY_ACTION_UI_SCROLL);
    buddy_state_reduce(&state, &up, 1001, &action);
    assert(state.page == BUDDY_PAGE_PET);

    buddy_state_reduce(&state, &prompt, 1002, &action);
    buddy_state_reduce(&state, &up, 1003, &action);
    assert(action.type == BUDDY_ACTION_UI_SCROLL);
    assert(action.scroll_delta < 0);
}

static void test_settings_actions_have_separate_confirmations(void)
{
    buddy_settings_snapshot_t settings = {.ble_enabled = true};
    buddy_state_t state;
    buddy_ui_snapshot_t snapshot;
    buddy_action_t action = {0};
    buddy_event_t long_ok = {.type = BUDDY_EVENT_KEY_LONG, .key = BUDDY_KEY_OK};
    buddy_event_t click_ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, &settings);
    buddy_state_reduce(&state, &long_ok, 1000, &action);
    buddy_state_reduce(&state, &click_ok, 1001, &action);
    assert(state.page == BUDDY_PAGE_SETTINGS);
    state.settings_selection = BUDDY_SETTINGS_BLE;
    buddy_state_reduce(&state, &click_ok, 1002, &action);
    assert(action.type == BUDDY_ACTION_BLE_TOGGLE);
    assert(!action.ble_enabled);
    buddy_state_snapshot(&state, &snapshot);
    assert(!snapshot.ble_enabled);

    state.settings_selection = BUDDY_SETTINGS_RESET;
    buddy_state_reduce(&state, &click_ok, 1003, &action);
    assert(state.reset_open);
    state.reset_selection = BUDDY_RESET_UNPAIR;
    buddy_state_reduce(&state, &click_ok, 1004, &action);
    assert(state.confirmation == BUDDY_CONFIRM_UNPAIR);
    assert(!state.confirmation_acknowledge);
    buddy_event_t click_down = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};
    buddy_state_reduce(&state, &click_down, 1005, &action);

    buddy_state_reduce(&state, &long_ok, 1006, &action);
    buddy_state_reduce(&state, &click_ok, 1007, &action);
    state.settings_selection = BUDDY_SETTINGS_RESET;
    buddy_state_reduce(&state, &click_ok, 1008, &action);
    state.reset_selection = BUDDY_RESET_FACTORY_RESET;
    buddy_state_reduce(&state, &click_ok, 1010, &action);
    assert(state.confirmation == BUDDY_CONFIRM_FACTORY_RESET);
    buddy_state_reduce(&state, &click_ok, 1011, &action);
    assert(action.type == BUDDY_ACTION_FACTORY_RESET_CONFIRMED);
}

static void test_original_settings_surface_is_complete_and_bounded(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    assert(BUDDY_SETTINGS_COUNT == 10);
    assert(BUDDY_RESET_COUNT == 4);
    buddy_state_init(&state, NULL);
    state.page = BUDDY_PAGE_SETTINGS;
    state.settings_selection = BUDDY_SETTINGS_BRIGHTNESS;
    buddy_state_reduce(&state, &ok, 1, &action);
    assert(action.type == BUDDY_ACTION_DISPLAY_BACKLIGHT);
    assert(action.brightness_percent <= 100);
}

static void test_remote_unpair_confirmation_remembers_ack(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t unpair = {.type = BUDDY_EVENT_UNPAIR_CONFIRMATION};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    unpair.ble.connection_generation = 13;
    buddy_state_reduce(&state, &unpair, 1000, &action);
    assert(state.confirmation == BUDDY_CONFIRM_UNPAIR);
    assert(state.confirmation_acknowledge);
    buddy_state_reduce(&state, &ok, 1001, &action);
    assert(action.type == BUDDY_ACTION_UNPAIR_CONFIRMED);
    assert(action.confirmation_acknowledge);
    assert(action.connection_generation == 13);
}

static void test_remote_unpair_cannot_replace_a_local_confirmation(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t remote_unpair = {.type = BUDDY_EVENT_UNPAIR_CONFIRMATION};

    buddy_state_init(&state, NULL);
    state.confirmation = BUDDY_CONFIRM_FACTORY_RESET;
    state.confirmation_pending = true;
    remote_unpair.ble.connection_generation = 17;
    buddy_state_reduce(&state, &remote_unpair, 1000, &action);

    assert(state.confirmation == BUDDY_CONFIRM_FACTORY_RESET);
    assert(!state.confirmation_acknowledge);
    assert(action.type == BUDDY_ACTION_NONE);
}

static void test_ble_security_events_update_owned_state_and_clear_sensitive_prompt(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t connected = {.type = BUDDY_EVENT_BLE_CONNECTED};
    buddy_event_t passkey = {.type = BUDDY_EVENT_BLE_PASSKEY};
    buddy_event_t encrypted = {.type = BUDDY_EVENT_BLE_ENCRYPTION};
    buddy_event_t disconnected = {.type = BUDDY_EVENT_BLE_DISCONNECTED};
    buddy_event_t prompt = test_prompt_event("req-secret", "Bash", "secret", 0, 1);

    passkey.ble.passkey = 123456;
    encrypted.ble.secure = true;
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &connected, 1000, &action);
    assert(state.ble_connected);
    assert(!state.ble_encrypted);
    buddy_state_reduce(&state, &passkey, 1001, &action);
    assert(state.passkey_visible);
    assert(state.passkey == 123456);
    buddy_state_reduce(&state, &encrypted, 1002, &action);
    assert(state.ble_encrypted);
    assert(!state.passkey_visible);

    buddy_state_reduce(&state, &prompt, 1003, &action);
    buddy_state_reduce(&state, &disconnected, 1004, &action);
    assert(!state.ble_connected);
    assert(!state.ble_encrypted);
    assert(state.heartbeat_stale);
    assert(state.prompt.id[0] == '\0');
}

static void test_stale_security_mailbox_event_cannot_override_latest_link(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t connected = {.type = BUDDY_EVENT_BLE_CONNECTED};
    buddy_event_t passkey = {.type = BUDDY_EVENT_BLE_PASSKEY};
    buddy_event_t encrypted = {.type = BUDDY_EVENT_BLE_ENCRYPTION};

    connected.ble.connection_generation = 8;
    passkey.ble.connection_generation = 7;
    passkey.ble.passkey = 123456;
    encrypted.ble.connection_generation = 7;
    encrypted.ble.secure = true;
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &connected, 1000, &action);
    assert(state.ble_connection_generation == 8);

    buddy_state_reduce(&state, &passkey, 1001, &action);
    buddy_state_reduce(&state, &encrypted, 1002, &action);
    assert(!state.passkey_visible);
    assert(!state.ble_encrypted);

    passkey.ble.connection_generation = 8;
    buddy_state_reduce(&state, &passkey, 1003, &action);
    assert(state.passkey_visible);
}

static void test_new_link_generation_invalidates_sensitive_state_when_disconnect_was_coalesced(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t connected = {.type = BUDDY_EVENT_BLE_CONNECTED};
    buddy_event_t prompt = test_prompt_event("old-link", "Bash", "deploy", 1, 1);
    buddy_event_t unpair = {.type = BUDDY_EVENT_UNPAIR_CONFIRMATION};

    buddy_state_init(&state, NULL);
    connected.ble.connection_generation = 7;
    buddy_state_reduce(&state, &connected, 1000, &action);
    prompt.ble.connection_generation = 7;
    buddy_state_reduce(&state, &prompt, 1001, &action);

    connected.ble.connection_generation = 8;
    buddy_state_reduce(&state, &connected, 1002, &action);
    assert(state.prompt.id[0] == '\0');
    assert(!state.connected);
    assert(state.heartbeat_stale);

    unpair.ble.connection_generation = 8;
    buddy_state_reduce(&state, &unpair, 1003, &action);
    assert(state.confirmation == BUDDY_CONFIRM_UNPAIR);
    connected.ble.connection_generation = 9;
    buddy_state_reduce(&state, &connected, 1004, &action);
    assert(state.confirmation == BUDDY_CONFIRM_NONE);
    assert(!state.confirmation_pending);
}

/* 2026-01-01 00:00 UTC. */
#define TEST_MIDNIGHT 1767225600LL

static buddy_event_t test_time_event(int64_t epoch_seconds)
{
    buddy_event_t event = {0};

    event.type = BUDDY_EVENT_TIME;
    event.time.epoch_seconds = epoch_seconds;
    return event;
}

static void test_pet_starts_happy_in_snapshot(void)
{
    buddy_state_t state;
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.pet_fullness == 80);
    assert(snapshot.pet_happiness == 80);
    assert(snapshot.pet_energy == 80);
    assert(snapshot.mood == BUDDY_MOOD_HAPPY);
}

static void test_pet_decays_with_ticks(void)
{
    buddy_state_t state;
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &tick, 1000 + 7200000, NULL);
    assert(state.pet.happiness == 70);
}

static void test_busy_drains_energy(void)
{
    buddy_state_t state;
    buddy_event_t heartbeat = test_heartbeat_event(0, 1);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &tick, 1000 + 3000000, NULL);
    assert(state.pet.energy == 70);
}

static uint8_t test_energy_after_gap(int64_t epoch_seconds)
{
    buddy_state_t state;
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t time = test_time_event(epoch_seconds);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &time, 1000, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000 + 1200000, NULL);
    return state.pet.energy;
}

static void test_night_counts_as_sleep(void)
{
    assert(test_energy_after_gap(TEST_MIDNIGHT + 23 * 3600 + 30 * 60) == 90);
    assert(test_energy_after_gap(TEST_MIDNIGHT + 12 * 3600) == 80);
}

static void test_waiting_prompt_lowers_happiness(void)
{
    buddy_state_t state;
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &prompt, 1000, NULL);
    buddy_state_reduce(&state, &tick, 1000 + 360000, NULL);
    assert(state.pet.happiness == 77);
    buddy_state_reduce(&state, &tick, 1000 + 360000, NULL);
    assert(state.pet.happiness == 77);
}

static void test_approval_raises_happiness(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t approve = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_event_t result = test_permission_result_event("req-1", BUDDY_PERMISSION_ONCE, true);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &prompt, 1000, &action);
    buddy_state_reduce(&state, &approve, 2000, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
    buddy_state_reduce(&state, &result, 2000, &action);
    assert(state.pet.happiness == 83);
}

static buddy_settings_snapshot_t test_settings_with_pet(uint8_t fullness, uint8_t happiness,
                                                        uint8_t energy, uint32_t epoch)
{
    buddy_settings_snapshot_t settings = {0};
    buddy_pet_t pet;

    buddy_pet_init(&pet);
    pet.fullness = fullness;
    pet.happiness = happiness;
    pet.energy = energy;
    settings.pet = buddy_pet_pack(&pet, epoch);
    return settings;
}

static void test_state_loads_saved_pet(void)
{
    buddy_settings_snapshot_t settings = test_settings_with_pet(12, 34, 56, 0);
    buddy_settings_snapshot_t empty = {0};
    buddy_state_t state;

    buddy_state_init(&state, &settings);
    assert(state.pet.fullness == 12 && state.pet.happiness == 34 && state.pet.energy == 56);
    buddy_state_init(&state, &empty);
    assert(state.pet.fullness == 80 && state.pet.happiness == 80 && state.pet.energy == 80);
}

static void test_catch_up_once_and_never_negative(void)
{
    const uint32_t saved = (uint32_t)(TEST_MIDNIGHT + 12 * 3600);
    buddy_settings_snapshot_t settings = test_settings_with_pet(80, 80, 50, saved);
    buddy_state_t state;
    buddy_event_t time = test_time_event((int64_t)saved + 3610);

    buddy_state_init(&state, &settings);
    buddy_state_reduce(&state, &time, 10000, NULL);
    assert(state.pet.energy == 80);
    time = test_time_event((int64_t)saved + 3620);
    buddy_state_reduce(&state, &time, 20000, NULL);
    assert(state.pet.energy == 80);

    settings = test_settings_with_pet(80, 80, 50, saved + 100000U);
    buddy_state_init(&state, &settings);
    time = test_time_event((int64_t)saved + 3610);
    buddy_state_reduce(&state, &time, 10000, NULL);
    assert(state.pet.energy == 50);
}

static void test_catch_up_capped_at_one_day(void)
{
    const uint32_t saved = (uint32_t)TEST_MIDNIGHT;
    buddy_settings_snapshot_t settings = test_settings_with_pet(100, 100, 50, saved);
    buddy_state_t state;
    buddy_event_t time = test_time_event((int64_t)saved + 10 * 86400 + 500);

    buddy_state_init(&state, &settings);
    buddy_state_reduce(&state, &time, 0, NULL);
    /* Every stat saturates within a day, so the cap shows only in the leftover
     * seconds: 86400 is a whole number of fullness periods, 864500 is not. */
    assert(state.pet.fullness == 0);
    assert(state.pet.fullness_carry_s == 0);
}

static void test_periodic_pet_save(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};
    buddy_event_t time = test_time_event(TEST_MIDNIGHT);

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &tick, 0, &action);
    buddy_state_reduce(&state, &tick, 599999, &action);
    assert(action.type != BUDDY_ACTION_PET_SAVE);
    buddy_state_reduce(&state, &tick, 600000, &action);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
    assert(action.pet == buddy_pet_pack(&state.pet, 0));

    buddy_state_reduce(&state, &time, 601000, &action);
    buddy_state_reduce(&state, &tick, 1200000, &action);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
    assert(action.pet == buddy_pet_pack(&state.pet, (uint32_t)TEST_MIDNIGHT + 599U));
}

static void test_home_ok_pats(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &ok, 1000, &action);
    assert(state.pet.happiness == 85);
    assert(state.character == BUDDY_CHARACTER_PAT);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
    assert(action.pet == buddy_pet_pack(&state.pet, 0));
}

static void test_pet_page_cycles_and_feeds(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t up = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_UP};
    buddy_event_t down = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_DOWN};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &up, 1000, &action);
    assert(state.page == BUDDY_PAGE_PET && state.care_selection == BUDDY_CARE_FEED);
    buddy_state_reduce(&state, &down, 1000, &action);
    buddy_state_reduce(&state, &down, 1000, &action);
    assert(state.care_selection == BUDDY_CARE_PAT);
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.care_selection == BUDDY_CARE_PAT);
    buddy_state_reduce(&state, &down, 1000, &action);
    assert(state.care_selection == BUDDY_CARE_FEED);
    buddy_state_reduce(&state, &ok, 1000, &action);
    assert(state.pet.fullness == 100);
    assert(state.character == BUDDY_CHARACTER_EAT);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
    buddy_state_reduce(&state, &down, 1000, &action);
    buddy_state_reduce(&state, &ok, 1000, &action);
    assert(state.pet.happiness == 100);
    assert(state.character == BUDDY_CHARACTER_PLAY);
}

static void test_feed_refused(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t up = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_UP};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    state.pet.fullness = 95;
    buddy_state_reduce(&state, &up, 1000, &action);
    buddy_state_reduce(&state, &ok, 1000, &action);
    assert(state.pet.fullness == 95);
    assert(state.character == BUDDY_CHARACTER_REFUSE);
    assert(state.message[0] == '\0');
    buddy_state_snapshot(&state, &snapshot);
    assert(strcmp(snapshot.care_message, "Not hungry") == 0);
    buddy_state_reduce(&state, &tick, 3999, &action);
    assert(state.character == BUDDY_CHARACTER_REFUSE);
    buddy_state_reduce(&state, &tick, 4000, &action);
    assert(state.character != BUDDY_CHARACTER_REFUSE);
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.care_message[0] == '\0');
}

static void test_pat_burst_dizzy(void)
{
    buddy_state_t state;
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    unsigned i;

    buddy_state_init(&state, NULL);
    state.pet.happiness = 50;
    for (i = 0; i < 4U; ++i) {
        buddy_state_reduce(&state, &ok, 1000U + i * 500U, NULL);
        assert(state.character == BUDDY_CHARACTER_PAT);
    }
    assert(state.pet.happiness == 70);
    buddy_state_reduce(&state, &ok, 3000, NULL);
    assert(state.character == BUDDY_CHARACTER_DIZZY);
    assert(state.pet.happiness == 70);
    /* The burst history starts over, so the next pat is an ordinary one. */
    buddy_state_reduce(&state, &ok, 3500, NULL);
    assert(state.character == BUDDY_CHARACTER_PAT);
    assert(state.pet.happiness == 75);
}

static void test_ok_during_guard_is_ignored(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &prompt, 1000, NULL);
    buddy_state_reduce(&state, &ok, 1999, &action);
    assert(action.type == BUDDY_ACTION_NONE);
    assert(state.pet.happiness == 80);
    assert(!state.approval_locked);
    /* The ignored press restarts the quiet second, so 2000 is too early. */
    buddy_state_reduce(&state, &ok, 2000, &action);
    assert(action.type != BUDDY_ACTION_PERMISSION);
    buddy_state_reduce(&state, &ok, 3000, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
}

static void test_pet_clock_starts_on_first_event(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    /* Re-initialised after 10 h of uptime, as unpair and factory reset do. */
    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &tick, 36000000, &action);
    assert(state.pet.fullness == 80 && state.pet.happiness == 80 && state.pet.energy == 80);
    assert(action.type != BUDDY_ACTION_PET_SAVE);
    buddy_state_reduce(&state, &tick, 36000000 + 600000, &action);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
}

static void test_reinit_keeps_live_pet(void)
{
    buddy_settings_snapshot_t older = test_settings_with_pet(90, 90, 90, 0);
    buddy_state_t state;
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &tick, 36000000, NULL);
    state.pet.fullness = 12;
    state.pet.happiness = 34;
    state.pet.energy = 56;
    state.pet_catch_up_done = true;
    buddy_state_reinit_keep_pet(&state, &older);
    buddy_state_reduce(&state, &tick, 36001000, NULL);
    assert(state.pet.fullness == 12 && state.pet.happiness == 34 && state.pet.energy == 56);
    assert(state.pet_catch_up_done);
}

static void test_wake_restarts_approval_guard(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    state.screen_off = true;
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &prompt, 1000, NULL);
    buddy_state_reduce(&state, &ok, 5000, &action);
    assert(!state.screen_off);
    buddy_state_reduce(&state, &ok, 5200, &action);
    assert(action.type != BUDDY_ACTION_PERMISSION);
    buddy_state_reduce(&state, &ok, 6200, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
}

static void test_ok_burst_never_approves(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
    uint64_t t;

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &ok, 1800, NULL);
    buddy_state_reduce(&state, &prompt, 2000, NULL);
    for (t = 2200; t <= 5000; t += 400) {
        buddy_state_reduce(&state, &ok, t, &action);
        assert(action.type != BUDDY_ACTION_PERMISSION);
    }
    buddy_state_reduce(&state, &ok, 5999, &action);
    assert(action.type != BUDDY_ACTION_PERMISSION);
    buddy_state_reduce(&state, &ok, 7000, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);
}

#define TEST_POOP_BUSY_MS (20ULL * 60ULL * 1000ULL)

/* Heartbeats every 10 s from from_ms to to_ms; returns the last action. */
static buddy_action_t test_session(buddy_state_t *state, unsigned running, unsigned waiting,
                                   uint64_t from_ms, uint64_t to_ms)
{
    buddy_event_t heartbeat = test_heartbeat_event(0, running);
    buddy_action_t action = {0};
    uint64_t t;

    heartbeat.heartbeat.waiting = waiting;
    for (t = from_ms; t <= to_ms; t += 10000U) {
        buddy_state_reduce(state, &heartbeat, t, &action);
    }
    return action;
}

static void test_busy_time_drops_poop_when_idle(void)
{
    buddy_state_t state;
    buddy_action_t action;
    buddy_ui_snapshot_t snapshot;

    buddy_state_init(&state, NULL);
    action = test_session(&state, 1, 0, 1000, 1000 + TEST_POOP_BUSY_MS);
    assert(state.pet.poop == 0);
    action = test_session(&state, 0, 0, 1000 + TEST_POOP_BUSY_MS + 10000,
                          1000 + TEST_POOP_BUSY_MS + 10000);
    assert(state.pet.poop == 1);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
    assert(action.pet == buddy_pet_pack(&state.pet, 0));
    buddy_state_snapshot(&state, &snapshot);
    assert(snapshot.poop == 1);
    assert(snapshot.mood == BUDDY_MOOD_STINKY);
    test_session(&state, 0, 0, 1000 + TEST_POOP_BUSY_MS + 20000,
                 1000 + TEST_POOP_BUSY_MS + 60000);
    assert(state.pet.poop == 1);
}

static void test_short_busy_spell_leaves_no_poop(void)
{
    buddy_state_t state;

    buddy_state_init(&state, NULL);
    test_session(&state, 1, 0, 1000, 1000 + TEST_POOP_BUSY_MS - 20000);
    test_session(&state, 0, 0, 1000 + TEST_POOP_BUSY_MS - 10000,
                 1000 + TEST_POOP_BUSY_MS + 60000);
    assert(state.pet.poop == 0);
}

static void test_busy_time_adds_up_across_turns(void)
{
    buddy_state_t state;

    buddy_state_init(&state, NULL);
    test_session(&state, 1, 0, 1000, 601000);
    test_session(&state, 0, 0, 611000, 901000);
    assert(state.pet.poop == 0);
    test_session(&state, 1, 0, 901000, 1491000);
    assert(state.pet.poop == 0);
    test_session(&state, 0, 0, 1501000, 1501000);
    assert(state.pet.poop == 1);
}

static void test_no_poop_while_prompt_waits(void)
{
    buddy_state_t state;

    buddy_state_init(&state, NULL);
    test_session(&state, 1, 0, 1000, 1000 + TEST_POOP_BUSY_MS);
    test_session(&state, 0, 1, 1000 + TEST_POOP_BUSY_MS + 10000,
                 1000 + TEST_POOP_BUSY_MS + 30000);
    assert(state.pet.poop == 0);
    test_session(&state, 0, 0, 1000 + TEST_POOP_BUSY_MS + 40000,
                 1000 + TEST_POOP_BUSY_MS + 40000);
    assert(state.pet.poop == 1);
}

static void test_poop_pile_caps_at_four(void)
{
    buddy_state_t state;

    buddy_state_init(&state, NULL);
    test_session(&state, 1, 0, 1000, 1000 + 5U * TEST_POOP_BUSY_MS);
    test_session(&state, 0, 0, 1000 + 5U * TEST_POOP_BUSY_MS + 10000,
                 1000 + 5U * TEST_POOP_BUSY_MS + 10000);
    assert(state.pet.poop == 4);
    /* The pile stays full; cleaning does not release a backlog. */
    state.pet.poop = 0;
    test_session(&state, 0, 0, 1000 + 5U * TEST_POOP_BUSY_MS + 20000,
                 1000 + 5U * TEST_POOP_BUSY_MS + 20000);
    assert(state.pet.poop == 0);
}

static void test_double_ok_cleans_pile(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t twice = {.type = BUDDY_EVENT_KEY_DOUBLE, .key = BUDDY_KEY_OK};
    buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

    buddy_state_init(&state, NULL);
    state.pet.poop = 3;
    buddy_state_reduce(&state, &twice, 1000, &action);
    assert(state.pet.poop == 0);
    assert(state.pet.happiness == 80);
    assert(state.character == BUDDY_CHARACTER_CLEAN);
    assert(action.type == BUDDY_ACTION_PET_SAVE);
    assert(action.pet == buddy_pet_pack(&state.pet, 0));
    buddy_state_reduce(&state, &tick, 4000, &action);
    assert(state.character != BUDDY_CHARACTER_CLEAN);
}

static void test_double_ok_cleans_from_pet_page(void)
{
    buddy_state_t state;
    buddy_event_t up = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_UP};
    buddy_event_t twice = {.type = BUDDY_EVENT_KEY_DOUBLE, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &up, 1000, NULL);
    state.pet.poop = 2;
    buddy_state_reduce(&state, &twice, 2000, NULL);
    assert(state.pet.poop == 0);
    assert(state.pet.fullness == 80);
}

static void test_double_ok_without_poop_is_one_click(void)
{
    buddy_state_t state;
    buddy_event_t twice = {.type = BUDDY_EVENT_KEY_DOUBLE, .key = BUDDY_KEY_OK};
    buddy_event_t down_twice = {.type = BUDDY_EVENT_KEY_DOUBLE, .key = BUDDY_KEY_DOWN};
    buddy_event_t up = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_UP};

    buddy_state_init(&state, NULL);
    buddy_state_reduce(&state, &twice, 1000, NULL);
    assert(state.pet.happiness == 85);
    assert(state.character == BUDDY_CHARACTER_PAT);
    buddy_state_reduce(&state, &up, 5000, NULL);
    buddy_state_reduce(&state, &down_twice, 5000, NULL);
    assert(state.care_selection == BUDDY_CARE_PLAY);
}

static void test_double_ok_never_approves_or_confirms(void)
{
    buddy_state_t state;
    buddy_action_t action = {0};
    buddy_event_t heartbeat = test_heartbeat_event(0, 0);
    buddy_event_t prompt = test_prompt_event("req-1", "Bash", "git push", 0, 1);
    buddy_event_t twice = {.type = BUDDY_EVENT_KEY_DOUBLE, .key = BUDDY_KEY_OK};
    buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};

    buddy_state_init(&state, NULL);
    state.pet.poop = 2;
    buddy_state_reduce(&state, &heartbeat, 1000, NULL);
    buddy_state_reduce(&state, &prompt, 1000, NULL);
    buddy_state_reduce(&state, &twice, 5000, &action);
    assert(action.type != BUDDY_ACTION_PERMISSION);
    assert(state.pet.poop == 2);
    /* It still counts as an OK press for the quiet second. */
    buddy_state_reduce(&state, &ok, 5500, &action);
    assert(action.type != BUDDY_ACTION_PERMISSION);
    buddy_state_reduce(&state, &ok, 7000, &action);
    assert(action.type == BUDDY_ACTION_PERMISSION);

    buddy_state_init(&state, NULL);
    state.pet.poop = 2;
    state.confirmation = BUDDY_CONFIRM_UNPAIR;
    buddy_state_reduce(&state, &twice, 1000, &action);
    assert(action.type != BUDDY_ACTION_UNPAIR_CONFIRMED);
    assert(state.confirmation == BUDDY_CONFIRM_UNPAIR);
    assert(state.pet.poop == 2);
}

static void test_reinit_keeps_poop(void)
{
    buddy_state_t state;

    buddy_state_init(&state, NULL);
    state.pet.poop = 3;
    buddy_state_reinit_keep_pet(&state, NULL);
    assert(state.pet.poop == 3);
}

int main(void)
{
    test_offline_initialization();
    test_pet_starts_happy_in_snapshot();
    test_pet_decays_with_ticks();
    test_busy_drains_energy();
    test_night_counts_as_sleep();
    test_waiting_prompt_lowers_happiness();
    test_approval_raises_happiness();
    test_state_loads_saved_pet();
    test_catch_up_once_and_never_negative();
    test_catch_up_capped_at_one_day();
    test_periodic_pet_save();
    test_home_ok_pats();
    test_pet_page_cycles_and_feeds();
    test_feed_refused();
    test_pat_burst_dizzy();
    test_ok_during_guard_is_ignored();
    test_pet_clock_starts_on_first_event();
    test_reinit_keeps_live_pet();
    test_wake_restarts_approval_guard();
    test_ok_burst_never_approves();
    test_busy_time_drops_poop_when_idle();
    test_short_busy_spell_leaves_no_poop();
    test_busy_time_adds_up_across_turns();
    test_no_poop_while_prompt_waits();
    test_poop_pile_caps_at_four();
    test_double_ok_cleans_pile();
    test_double_ok_cleans_from_pet_page();
    test_double_ok_without_poop_is_one_click();
    test_double_ok_never_approves_or_confirms();
    test_reinit_keeps_poop();
    test_heartbeat_mapping();
    test_character_priority();
    test_timeout_clears_prompt();
    test_heartbeat_does_not_overwrite_persisted_identity();
    test_token_boundaries_celebrate_once();
    test_persisted_celebration_level_is_not_replayed();
    test_approval_locks_until_a_new_prompt();
    test_denial_locks_and_emits_exactly_once();
    test_permission_action_is_bound_to_the_prompt_connection();
    test_permission_success_moves_from_attempted_to_successful_state();
    test_permission_failure_is_visible_and_same_id_replay_stays_locked();
    test_denial_success_and_ticks_never_show_heart();
    test_successful_approval_heart_expires_at_five_seconds();
    test_heartbeat_prompt_snapshot_clears_or_preserves_approval_lock();
    test_ui_snapshot_runtime_indicators_default_off();
    test_absent_prompt_is_ignored();
    test_timeout_stale_prompt_is_ignored();
    test_standalone_prompt_refreshes_liveness_clock();
    test_disconnect_then_reconnect_does_not_restore_prompt();
    test_mismatched_observed_prompt_is_ignored();
    test_nonterminated_prompt_id_is_ignored();
    test_truncated_prompt_id_is_ignored();
    test_long_ok_opens_settings();
    test_original_screen_and_menu_navigation();
    test_protocol_command_events_refresh_the_display();
    test_status_request_does_not_overwrite_message();
    test_unpair_confirmation_survives_a_heartbeat();
    test_unpair_confirmation_ok_emits_explicit_action();
    test_unpair_confirmation_down_cancels_without_action();
    test_parsed_heartbeat_approval_serializes_permission();
    test_normal_navigation_and_approval_scroll_are_distinct();
    test_settings_actions_have_separate_confirmations();
    test_original_settings_surface_is_complete_and_bounded();
    test_remote_unpair_confirmation_remembers_ack();
    test_remote_unpair_cannot_replace_a_local_confirmation();
    test_ble_security_events_update_owned_state_and_clear_sensitive_prompt();
    test_stale_security_mailbox_event_cannot_override_latest_link();
    test_new_link_generation_invalidates_sensitive_state_when_disconnect_was_coalesced();
    return 0;
}
