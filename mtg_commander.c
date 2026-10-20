#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <mtg_commander_icons.h>

#define TAG "MtgCommander"
#define MAX_PLAYERS 4
#define CD_LETHAL 21
#define POISON_LETHAL 10
#define SAVE_DIR EXT_PATH("apps_data/mtg_commander")
#define SAVE_FILE EXT_PATH("apps_data/mtg_commander/save.bin")
#define SAVE_MAGIC 0x4D544731 // "MTG1"

typedef enum {
    ViewQuad = 0,
    ViewDetail,
    ViewMenu,
    ViewRoll,
} AppView;

typedef enum {
    LethalNone = 0,
    LethalLife,
    LethalCmdDmg,
    LethalPoison,
} LethalCause;

typedef enum {
    RollTypeD20 = 0,
    RollTypeD6,
    RollTypeCoin,
    RollTypeFirstPlayer,
} RollType;

typedef struct {
    int16_t life;
    uint8_t cmd_dmg[MAX_PLAYERS];
    uint8_t poison;
    uint8_t cmd_tax;
} Player;

typedef struct {
    uint32_t magic;
    int16_t starting_life;
    Player players[MAX_PLAYERS];
    bool sound_enabled;
    bool vibro_enabled;
} SaveData;

typedef enum {
    MenuItemFirstPlayer = 0,
    MenuItemResetGame,
    MenuItemToggleLife,
    MenuItemClearCD,
    MenuItemRollD20,
    MenuItemRollD6,
    MenuItemFlipCoin,
    MenuItemToggleVibro,
    MenuItemToggleSound,
    MenuItemBackToGame,
    MenuItemCount,
} MenuItem;

typedef struct {
    AppView current_view;
    uint8_t selected_player; // 0..3
    uint8_t detail_cursor;   // 0..4 (opp0 CD, opp1 CD, opp2 CD, poison, tax)
    int16_t starting_life;   // 40, 30, 20
    Player players[MAX_PLAYERS];
    
    // Menu state
    uint8_t menu_cursor;
    bool ignore_next_ok_short; // Prevents long-press OK bleed-through into menu
    
    // Dice / Coin state
    RollType roll_type;
    uint8_t roll_result; // 1-20 for D20, 1-6 for D6, 0-1 for coin, 1-4 for player
    
    // Settings
    bool sound_enabled;
    bool vibro_enabled;
    
    // Furi services
    FuriMutex* mutex;
    NotificationApp* notifications;
} MtgApp;

typedef enum {
    EventTypeInput,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

static const char* player_labels[MAX_PLAYERS] = {"P1", "P2", "P3", "P4"};

static uint8_t get_opponent_idx(uint8_t player, uint8_t slot) {
    uint8_t count = 0;
    for(uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if(i != player) {
            if(count == slot) return i;
            count++;
        }
    }
    return 0;
}

static LethalCause check_lethal(const Player* p) {
    for(uint8_t i = 0; i < MAX_PLAYERS; i++) {
        if(p->cmd_dmg[i] >= CD_LETHAL) return LethalCmdDmg;
    }
    if(p->poison >= POISON_LETHAL) return LethalPoison;
    if(p->life <= 0) return LethalLife;
    return LethalNone;
}

static void notify_click(MtgApp* app) {
    if(!app->notifications) return;
    if(app->vibro_enabled) {
        notification_message(app->notifications, &sequence_single_vibro);
    }
}

static void notify_alert(MtgApp* app) {
    if(!app->notifications) return;
    if(app->vibro_enabled) {
        notification_message(app->notifications, &sequence_double_vibro);
    }
    if(app->sound_enabled) {
        notification_message(app->notifications, &sequence_error);
    }
}

static void save_game_state(MtgApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return;

    storage_simply_mkdir(storage, SAVE_DIR);

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SAVE_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        SaveData data;
        data.magic = SAVE_MAGIC;
        data.starting_life = app->starting_life;
        data.sound_enabled = app->sound_enabled;
        data.vibro_enabled = app->vibro_enabled;
        memcpy(data.players, app->players, sizeof(app->players));

        storage_file_write(file, &data, sizeof(SaveData));
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static bool load_game_state(MtgApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return false;

    File* file = storage_file_alloc(storage);
    bool success = false;
    if(storage_file_open(file, SAVE_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        SaveData data;
        if(storage_file_read(file, &data, sizeof(SaveData)) == sizeof(SaveData)) {
            if(data.magic == SAVE_MAGIC) {
                app->starting_life = data.starting_life;
                app->sound_enabled = data.sound_enabled;
                app->vibro_enabled = data.vibro_enabled;
                memcpy(app->players, data.players, sizeof(app->players));
                success = true;
            }
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

static void reset_game(MtgApp* app, int16_t starting_life) {
    app->starting_life = starting_life;
    for(uint8_t i = 0; i < MAX_PLAYERS; i++) {
        app->players[i].life = starting_life;
        app->players[i].poison = 0;
        app->players[i].cmd_tax = 0;
        memset(app->players[i].cmd_dmg, 0, sizeof(app->players[i].cmd_dmg));
    }
}

static void roll_current_type(MtgApp* app) {
    switch(app->roll_type) {
    case RollTypeD20:
        app->roll_result = (furi_hal_random_get() % 20) + 1;
        break;
    case RollTypeD6:
        app->roll_result = (furi_hal_random_get() % 6) + 1;
        break;
    case RollTypeCoin:
        app->roll_result = (furi_hal_random_get() % 2); // 0 = Heads, 1 = Tails
        break;
    case RollTypeFirstPlayer:
        app->roll_result = (furi_hal_random_get() % 4) + 1;
        break;
    }
    notify_alert(app);
}

static void draw_quad_view(Canvas* canvas, MtgApp* app) {
    canvas_set_color(canvas, ColorBlack);
    // Draw crosshair grid dividers
    canvas_draw_line(canvas, 64, 0, 64, 63);
    canvas_draw_line(canvas, 0, 32, 127, 32);

    for(uint8_t i = 0; i < MAX_PLAYERS; i++) {
        uint8_t qx = (i % 2) * 64;
        uint8_t qy = (i / 2) * 32;
        Player* p = &app->players[i];
        LethalCause lethal = check_lethal(p);

        // Highlight active selection
        if(i == app->selected_player) {
            canvas_draw_rframe(canvas, qx + 1, qy + 1, 62, 30, 2);
            // Inverted badge for active player label
            canvas_draw_box(canvas, qx + 3, qy + 3, 16, 9);
            canvas_set_color(canvas, ColorWhite);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, qx + 5, qy + 10, player_labels[i]);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, qx + 4, qy + 10, player_labels[i]);
        }

        // Draw lethal status or badges
        if(lethal != LethalNone) {
            canvas_set_font(canvas, FontSecondary);
            const char* cause_str = "DEAD";
            if(lethal == LethalCmdDmg) cause_str = "CD 21";
            else if(lethal == LethalPoison) cause_str = "PSN 10";
            canvas_draw_str(canvas, qx + 24, qy + 10, cause_str);
            // Strike-through line across life box
            canvas_draw_line(canvas, qx + 22, qy + 20, qx + 58, qy + 20);
        }

        // Life total in Big Numbers
        char life_str[16];
        snprintf(life_str, sizeof(life_str), "%d", p->life);
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, qx + 22, qy + 23, life_str);

        // Summary bottom line:
        // Only show CD if max_cd > 0
        // Only show Poison if poison > 0
        uint8_t max_cd = 0;
        for(uint8_t c = 0; c < MAX_PLAYERS; c++) {
            if(p->cmd_dmg[c] > max_cd) max_cd = p->cmd_dmg[c];
        }

        char sub_str[24];
        sub_str[0] = '\0';
        if(max_cd > 0 && p->poison > 0) {
            snprintf(sub_str, sizeof(sub_str), "CD:%u P:%u", max_cd, p->poison);
        } else if(max_cd > 0) {
            snprintf(sub_str, sizeof(sub_str), "CD:%u", max_cd);
        } else if(p->poison > 0) {
            snprintf(sub_str, sizeof(sub_str), "Poison:%u", p->poison);
        } else if(p->cmd_tax > 0) {
            snprintf(sub_str, sizeof(sub_str), "Tax:+%u", p->cmd_tax);
        }

        if(sub_str[0] != '\0') {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, qx + 4, qy + 30, sub_str);
        }
    }
}

static void draw_detail_view(Canvas* canvas, MtgApp* app) {
    uint8_t sel = app->selected_player;
    Player* p = &app->players[sel];
    LethalCause lethal = check_lethal(p);

    // Top Header Banner
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 12);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);

    char header_str[36];
    if(lethal != LethalNone) {
        snprintf(header_str, sizeof(header_str), "%s [LETHAL!] LIFE:%d", player_labels[sel], p->life);
    } else {
        snprintf(header_str, sizeof(header_str), "%s DETAILS - LIFE:%d", player_labels[sel], p->life);
    }
    canvas_draw_str(canvas, 3, 9, header_str);
    canvas_set_color(canvas, ColorBlack);

    const uint8_t visible_count = 4;
    uint8_t scroll_offset = 0;
    if(app->detail_cursor >= visible_count) {
        scroll_offset = app->detail_cursor - visible_count + 1;
    }

    for(uint8_t i = 0; i < visible_count; i++) {
        uint8_t item_idx = i + scroll_offset;
        if(item_idx > 4) break;

        uint8_t y = 14 + (i * 12);
        bool is_selected = (item_idx == app->detail_cursor);

        if(is_selected) {
            canvas_draw_box(canvas, 2, y, 124, 11);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }

        canvas_set_font(canvas, FontSecondary);
        char label_str[32];
        char val_str[16];

        if(item_idx < 3) {
            uint8_t opp = get_opponent_idx(sel, item_idx);
            snprintf(label_str, sizeof(label_str), "CD from %s:", player_labels[opp]);
            snprintf(val_str, sizeof(val_str), "[%2u/21]", p->cmd_dmg[opp]);
        } else if(item_idx == 3) {
            snprintf(label_str, sizeof(label_str), "Poison Counters:");
            snprintf(val_str, sizeof(val_str), "[%2u/10]", p->poison);
        } else {
            snprintf(label_str, sizeof(label_str), "Commander Tax:");
            snprintf(val_str, sizeof(val_str), "[ +%u ]", p->cmd_tax);
        }

        canvas_draw_str(canvas, 6, y + 8, label_str);
        canvas_draw_str(canvas, 86, y + 8, val_str);

        if(is_selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }
}

static void draw_menu_view(Canvas* canvas, MtgApp* app) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 12);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 28, 9, "GAME OPTIONS");
    canvas_set_color(canvas, ColorBlack);

    const char* menu_names[MenuItemCount] = {
        "Pick First Player",
        "Reset Game",
        "Starting Life: 40/30/20",
        "Clear Cmdr Damage Only",
        "Roll D20 Die",
        "Roll D6 Die",
        "Flip a Coin",
        "Vibration: ON",
        "Sound: ON",
        "Back to Game",
    };

    const uint8_t visible_count = 4;
    uint8_t scroll_offset = 0;
    if(app->menu_cursor >= visible_count) {
        scroll_offset = app->menu_cursor - visible_count + 1;
    }

    for(uint8_t i = 0; i < visible_count; i++) {
        uint8_t item_idx = i + scroll_offset;
        if(item_idx >= MenuItemCount) break;

        uint8_t y = 14 + (i * 12);
        bool is_selected = (item_idx == app->menu_cursor);

        if(is_selected) {
            canvas_draw_box(canvas, 2, y, 124, 11);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }

        canvas_set_font(canvas, FontSecondary);
        char row_str[36];
        if(item_idx == MenuItemToggleLife) {
            snprintf(row_str, sizeof(row_str), "Start Life: %d", app->starting_life);
        } else if(item_idx == MenuItemToggleVibro) {
            snprintf(row_str, sizeof(row_str), "Vibration: %s", app->vibro_enabled ? "ON" : "OFF");
        } else if(item_idx == MenuItemToggleSound) {
            snprintf(row_str, sizeof(row_str), "Sound: %s", app->sound_enabled ? "ON" : "OFF");
        } else {
            strncpy(row_str, menu_names[item_idx], sizeof(row_str));
        }

        canvas_draw_str(canvas, 6, y + 8, row_str);

        if(is_selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }
}

static void draw_roll_view(Canvas* canvas, MtgApp* app) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, 4, 4, 120, 56, 3);
    canvas_draw_rframe(canvas, 6, 6, 116, 52, 2);

    canvas_set_font(canvas, FontSecondary);

    switch(app->roll_type) {
    case RollTypeD20: {
        canvas_draw_str(canvas, 38, 18, "D20 ROLL");
        char num_str[16];
        snprintf(num_str, sizeof(num_str), "%u", app->roll_result);
        canvas_set_font(canvas, FontBigNumbers);
        uint8_t x_offset = (app->roll_result >= 10) ? 54 : 59;
        canvas_draw_str(canvas, x_offset, 38, num_str);

        canvas_set_font(canvas, FontSecondary);
        if(app->roll_result == 20) {
            canvas_draw_str(canvas, 26, 50, "* NATURAL 20! *");
        } else if(app->roll_result == 1) {
            canvas_draw_str(canvas, 22, 50, "! CRITICAL FAIL !");
        } else {
            canvas_draw_str(canvas, 25, 50, "[OK: Roll Again]");
        }
        break;
    }
    case RollTypeD6: {
        canvas_draw_str(canvas, 42, 18, "D6 ROLL");
        char num_str[8];
        snprintf(num_str, sizeof(num_str), "%u", app->roll_result);
        // Draw centered die box outline
        canvas_draw_rframe(canvas, 52, 23, 24, 18, 2);
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, 59, 38, num_str);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 25, 50, "[OK: Roll Again]");
        break;
    }
    case RollTypeCoin: {
        canvas_draw_str(canvas, 36, 18, "COIN FLIP");
        const char* side = (app->roll_result == 0) ? "HEADS" : "TAILS";
        // Draw round coin frame
        canvas_draw_circle(canvas, 64, 30, 11);
        canvas_draw_circle(canvas, 64, 30, 9);
        canvas_set_font(canvas, FontPrimary);
        uint8_t x_pos = (app->roll_result == 0) ? 46 : 48;
        canvas_draw_str(canvas, x_pos, 34, side);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 25, 50, "[OK: Flip Again]");
        break;
    }
    case RollTypeFirstPlayer: {
        canvas_draw_str(canvas, 24, 18, "FIRST PLAYER");
        char res_str[24];
        snprintf(res_str, sizeof(res_str), "PLAYER %u", app->roll_result);
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, 30, 38, res_str);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 22, 50, "Takes Turn 1! [OK]");
        break;
    }
    }
}

static void app_draw_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    MtgApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    switch(app->current_view) {
    case ViewQuad:
        draw_quad_view(canvas, app);
        break;
    case ViewDetail:
        draw_detail_view(canvas, app);
        break;
    case ViewMenu:
        draw_menu_view(canvas, app);
        break;
    case ViewRoll:
        draw_roll_view(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    AppEvent event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void handle_input_quad(MtgApp* app, InputEvent* input) {
    if(input->type == InputTypeShort || input->type == InputTypeRepeat) {
        if(input->key == InputKeyRight) {
            app->selected_player = (app->selected_player + 1) % MAX_PLAYERS;
            notify_click(app);
        } else if(input->key == InputKeyLeft) {
            app->selected_player = (app->selected_player + MAX_PLAYERS - 1) % MAX_PLAYERS;
            notify_click(app);
        } else if(input->key == InputKeyUp) {
            app->players[app->selected_player].life++;
            notify_click(app);
        } else if(input->key == InputKeyDown) {
            app->players[app->selected_player].life--;
            notify_click(app);
            if(app->players[app->selected_player].life == 0) {
                notify_alert(app);
            }
        } else if(input->key == InputKeyOk) {
            app->current_view = ViewDetail;
            app->detail_cursor = 0;
            notify_click(app);
        }
    } else if(input->type == InputTypeLong) {
        if(input->key == InputKeyUp) {
            app->players[app->selected_player].life += 5;
            notify_click(app);
        } else if(input->key == InputKeyDown) {
            app->players[app->selected_player].life -= 5;
            notify_click(app);
        } else if(input->key == InputKeyOk) {
            app->current_view = ViewMenu;
            app->menu_cursor = 0;
            // Prevent the long-press release from bleeding into the menu
            app->ignore_next_ok_short = true;
            notify_click(app);
        }
    }
}

static void handle_input_detail(MtgApp* app, InputEvent* input) {
    uint8_t sel = app->selected_player;
    Player* p = &app->players[sel];

    if(input->type == InputTypeShort || input->type == InputTypeRepeat) {
        if(input->key == InputKeyUp) {
            if(app->detail_cursor > 0) app->detail_cursor--;
            notify_click(app);
        } else if(input->key == InputKeyDown) {
            if(app->detail_cursor < 4) app->detail_cursor++;
            notify_click(app);
        } else if(input->key == InputKeyRight) {
            if(app->detail_cursor < 3) {
                uint8_t opp = get_opponent_idx(sel, app->detail_cursor);
                p->cmd_dmg[opp]++;
                p->life--; // MTG Rule: Commander combat damage reduces life!
                if(p->cmd_dmg[opp] == CD_LETHAL) {
                    notify_alert(app);
                } else {
                    notify_click(app);
                }
            } else if(app->detail_cursor == 3) {
                p->poison++;
                if(p->poison == POISON_LETHAL) {
                    notify_alert(app);
                } else {
                    notify_click(app);
                }
            } else if(app->detail_cursor == 4) {
                p->cmd_tax += 2;
                notify_click(app);
            }
        } else if(input->key == InputKeyLeft) {
            if(app->detail_cursor < 3) {
                uint8_t opp = get_opponent_idx(sel, app->detail_cursor);
                if(p->cmd_dmg[opp] > 0) {
                    p->cmd_dmg[opp]--;
                    p->life++;
                    notify_click(app);
                }
            } else if(app->detail_cursor == 3) {
                if(p->poison > 0) {
                    p->poison--;
                    notify_click(app);
                }
            } else if(app->detail_cursor == 4) {
                if(p->cmd_tax >= 2) {
                    p->cmd_tax -= 2;
                    notify_click(app);
                }
            }
        } else if(input->key == InputKeyBack) {
            app->current_view = ViewQuad;
            notify_click(app);
        } else if(input->key == InputKeyOk) {
            // Quick jump +5 or +2
            if(app->detail_cursor < 3) {
                uint8_t opp = get_opponent_idx(sel, app->detail_cursor);
                p->cmd_dmg[opp] += 5;
                p->life -= 5;
                if(p->cmd_dmg[opp] >= CD_LETHAL) {
                    notify_alert(app);
                } else {
                    notify_click(app);
                }
            } else if(app->detail_cursor == 3) {
                p->poison += 2;
                if(p->poison >= POISON_LETHAL) {
                    notify_alert(app);
                } else {
                    notify_click(app);
                }
            } else if(app->detail_cursor == 4) {
                p->cmd_tax += 2;
                notify_click(app);
            }
        }
    }
}

static void handle_input_menu(MtgApp* app, InputEvent* input) {
    if(input->type == InputTypeRelease && input->key == InputKeyOk) {
        app->ignore_next_ok_short = false;
        return;
    }

    if(input->type == InputTypeShort || input->type == InputTypeRepeat) {
        if(input->key == InputKeyUp) {
            if(app->menu_cursor > 0) app->menu_cursor--;
            notify_click(app);
        } else if(input->key == InputKeyDown) {
            if(app->menu_cursor < MenuItemCount - 1) app->menu_cursor++;
            notify_click(app);
        } else if(input->key == InputKeyBack) {
            app->current_view = ViewQuad;
            notify_click(app);
        }
    }

    // Only handle OK on fresh Short presses (NEVER repeat!)
    if(input->type == InputTypeShort && input->key == InputKeyOk) {
        if(app->ignore_next_ok_short) {
            app->ignore_next_ok_short = false;
            return;
        }

        switch(app->menu_cursor) {
        case MenuItemFirstPlayer:
            app->roll_type = RollTypeFirstPlayer;
            app->current_view = ViewRoll;
            roll_current_type(app);
            break;
        case MenuItemResetGame:
            reset_game(app, app->starting_life);
            save_game_state(app);
            app->current_view = ViewQuad;
            notify_alert(app);
            break;
        case MenuItemToggleLife:
            if(app->starting_life == 40) app->starting_life = 30;
            else if(app->starting_life == 30) app->starting_life = 20;
            else app->starting_life = 40;
            notify_click(app);
            break;
        case MenuItemClearCD:
            for(uint8_t i = 0; i < MAX_PLAYERS; i++) {
                memset(app->players[i].cmd_dmg, 0, sizeof(app->players[i].cmd_dmg));
            }
            save_game_state(app);
            app->current_view = ViewQuad;
            notify_alert(app);
            break;
        case MenuItemRollD20:
            app->roll_type = RollTypeD20;
            app->current_view = ViewRoll;
            roll_current_type(app);
            break;
        case MenuItemRollD6:
            app->roll_type = RollTypeD6;
            app->current_view = ViewRoll;
            roll_current_type(app);
            break;
        case MenuItemFlipCoin:
            app->roll_type = RollTypeCoin;
            app->current_view = ViewRoll;
            roll_current_type(app);
            break;
        case MenuItemToggleVibro:
            app->vibro_enabled = !app->vibro_enabled;
            notify_click(app);
            break;
        case MenuItemToggleSound:
            app->sound_enabled = !app->sound_enabled;
            notify_click(app);
            break;
        case MenuItemBackToGame:
            app->current_view = ViewQuad;
            notify_click(app);
            break;
        default:
            break;
        }
    }
}

static void handle_input_roll(MtgApp* app, InputEvent* input) {
    if(input->type == InputTypeShort) {
        if(input->key == InputKeyBack) {
            app->current_view = ViewQuad;
            notify_click(app);
        } else if(input->key == InputKeyOk) {
            roll_current_type(app);
        }
    }
}

int32_t mtg_commander_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "MTG Commander App Starting");

    MtgApp* app = malloc(sizeof(MtgApp));
    memset(app, 0, sizeof(MtgApp));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->starting_life = 40;
    app->sound_enabled = true;
    app->vibro_enabled = true;
    app->current_view = ViewQuad;

    reset_game(app, 40);

    // Try loading saved game
    load_game_state(app);

    // Open notification record
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, app_draw_callback, app);
    view_port_input_callback_set(view_port, app_input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    AppEvent event;
    bool running = true;

    while(running) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 1000);

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        if(event_status == FuriStatusOk) {
            if(event.type == EventTypeInput) {
                if(app->current_view == ViewQuad) {
                    if(event.input.type == InputTypeShort && event.input.key == InputKeyBack) {
                        running = false;
                    } else {
                        handle_input_quad(app, &event.input);
                    }
                } else if(app->current_view == ViewDetail) {
                    handle_input_detail(app, &event.input);
                } else if(app->current_view == ViewMenu) {
                    handle_input_menu(app, &event.input);
                } else if(app->current_view == ViewRoll) {
                    handle_input_roll(app, &event.input);
                }
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(view_port);
    }

    // Save game before exiting
    save_game_state(app);

    // Clean up
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    furi_record_close(RECORD_GUI);
    if(app->notifications) {
        furi_record_close(RECORD_NOTIFICATION);
    }

    furi_mutex_free(app->mutex);
    free(app);

    FURI_LOG_I(TAG, "MTG Commander App Exited cleanly");
    return 0;
}
