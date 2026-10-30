#pragma once

#include "buddy_pet.h"
#include "buddy_types.h"

typedef struct {
    buddy_connection_t connection;
    buddy_character_t character;
    buddy_page_t page;
    buddy_heartbeat_t heartbeat;
    buddy_prompt_t prompt;
    buddy_settings_snapshot_t settings;
    char last_attempted_prompt_id[BUDDY_PROMPT_ID_MAX];
    char last_successful_decision_id[BUDDY_PROMPT_ID_MAX];
    char name[BUDDY_NAME_MAX];
    char owner[BUDDY_OWNER_MAX];
    char time[BUDDY_MESSAGE_MAX];
    char message[BUDDY_MESSAGE_MAX];
    char entries[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
    unsigned total;
    unsigned running;
    unsigned waiting;
    uint64_t tokens;
    uint64_t tokens_today;
    int64_t epoch_seconds;
    int32_t timezone_offset_seconds;
    uint64_t time_received_ms;
    uint64_t highest_celebrated_level;
    uint64_t last_heartbeat_ms;
    uint64_t temporary_until_ms;
    uint32_t prompt_connection_generation;
    uint32_t confirmation_connection_generation;
    uint32_t ble_connection_generation;
    buddy_character_t temporary_character;
    bool connected;
    bool heartbeat_stale;
    bool confirmation_pending;
    buddy_confirmation_t confirmation;
    bool confirmation_acknowledge;
    buddy_settings_item_t settings_selection;
    buddy_reset_item_t reset_selection;
    buddy_menu_item_t menu_selection;
    buddy_care_t care_selection;
    uint8_t info_page;
    bool menu_open;
    bool reset_open;
    bool transcript_enabled;
    bool screen_off;
    uint8_t brightness_level;
    uint8_t species;
    bool pack_available;
    buddy_permission_delivery_t permission_delivery;
    bool approval_locked;
    bool ble_connected;
    bool ble_encrypted;
    bool battery_available;
    bool passkey_visible;
    uint32_t passkey;
    uint8_t battery_percent;
    uint16_t battery_mv;
    buddy_pet_t pet;
    uint64_t pet_updated_ms;
    uint64_t prompt_shown_ms;
    uint32_t prompt_penalties;
    uint64_t pet_saved_ms;
    uint32_t pet_saved_epoch;
    bool pet_catch_up_done;
    uint64_t pat_ms[4];
    uint8_t pat_count;
    bool pet_clock_started;
    bool prompt_visible;
    uint64_t prompt_visible_ms;
    bool has_last_ok;
    uint64_t last_ok_ms;
    /* Busy time not yet turned into poop, and when it was last counted. */
    uint64_t busy_ms;
    uint64_t busy_counted_ms;
    char care_message[BUDDY_CARE_MESSAGE_MAX];
    uint64_t care_message_until_ms;
} buddy_state_t;

void buddy_state_init(buddy_state_t *state, const buddy_settings_snapshot_t *settings);
/* buddy_state_init() that keeps the running pet, its clocks and catch-up
 * status, for resets that should not touch the pet (unpair). */
void buddy_state_reinit_keep_pet(buddy_state_t *state, const buddy_settings_snapshot_t *settings);
void buddy_state_reduce(buddy_state_t *state, const buddy_event_t *event,
                        uint64_t now_ms, buddy_action_t *action);
void buddy_state_snapshot(const buddy_state_t *state, buddy_ui_snapshot_t *snapshot);
/* Records whether an uploaded character is installed; select switches to it. */
void buddy_state_set_pack(buddy_state_t *state, bool available, bool select);
