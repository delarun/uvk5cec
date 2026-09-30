/* Hermes Link — UI (chat, compose, neighbours, settings)
 *
 * Open with a long press of MENU on the main screen.
 *   Chat:       UP/DOWN scroll, MENU write, * neighbours, F settings, EXIT back
 *   Compose:    multi-tap 0-9 (1 = .,?!@-/:), * backspace (hold = clear),
 *               UP/DOWN recipient (ALL / heard neighbours), MENU send, EXIT cancel.
 *               A leading "@CALL " sends to CALL even if it was never heard.
 *   Neighbours: UP/DOWN select, MENU write to, * send beacon now
 *   Settings:   UP/DOWN select, MENU toggle, EXIT save.
 *               Callsign = menu U.Info "MY CALL", passphrase = U.Info "MESHKEY".
 * PTT keeps working as voice PTT on every Hermes screen.
 */
#ifdef ENABLE_HERMES

#include <string.h>

#include "hermes/hermes.h"
#include "hermes/hm_messaging.h"
#include "hermes/hm_discovery.h"
#include "hermes/hm_addressing.h"

#include "app/generic.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "radio.h"
#include "ui/helper.h"
#include "ui/ui.h"
#include "ceccommon.h"

enum { V_CHAT, V_COMPOSE, V_NEIGH, V_SETTINGS };

#define ROWS      8
#define COLS      31
#define N_SETTINGS 8

static uint8_t view;
static uint8_t chat_top;                     // first message shown
static uint8_t sel;                          // neighbours / settings cursor
static int8_t  dest_idx;                     // -1 = ALL, else neighbour index

static char    edit_buf[HM_MSG_MAX_TEXT + 1];
static uint8_t edit_len;
static uint8_t edit_max;
static KEY_Code_t mt_key = KEY_INVALID;
static uint8_t mt_idx;
static uint8_t mt_timer;

static const char *const MT[10] = {
    " 0", ".,?!@-/:1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9"
};

// ─────────────────────────── drawing ───────────────────────────

static void row(uint8_t r, uint8_t x, const char *s) {
    char buf[COLS + 2];
    uint8_t i = 0;
    for (; s[i] && i < COLS + 1 - x / 4; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32;               // 3x5 font is upper case only
        buf[i] = c;
    }
    buf[i] = '\0';
    CEC_DisplaySmallest(buf, x, r * 7, false, true);
}

static void call_of(const uint8_t id[6], char *out) {
    HERMES_Addr_Decode(id, out, 11);            // FF..FF decodes to a non-Base40 string
    if (HM_IsBroadcast(id))
        strcpy(out, "ALL");
}

static uint8_t msg_text(const HermesMessage_t *m, char *out) {
    return HERMES_MSG_UnpackGSM7(m->payload, HM_PAYLOAD_SIZE, out, HM_MSG_MAX_TEXT + 1);
}

static uint8_t msg_rows(const HermesMessage_t *m) {
    char t[HM_MSG_MAX_TEXT + 1];
    uint8_t n = msg_text(m, t);
    uint8_t r = (n + COLS - 1) / COLS;
    if (r == 0) r = 1;
    return m->is_system ? r : r + 1;
}

static void scroll_to_end(void) {
    uint8_t used = 0;
    chat_top = gHermesMsgCount;
    while (chat_top > 0) {
        uint8_t r = msg_rows(&gHermesMessages[chat_top - 1]);
        if (used + r > ROWS) break;
        used += r;
        chat_top--;
    }
    if (chat_top == gHermesMsgCount && chat_top > 0)
        chat_top--;
}

static void draw_chat(void) {
    char s[40], call[12];

    if (gHermesMsgCount == 0) {
        row(0, 0, !(gHermesConfig.flags & HM_F_ENABLED) ? "HERMES OFF" : gHermesActive ? gHermesConfig.alias : "HERMES: FM ONLY");
        row(2, 0, "NO MESSAGES");
        row(5, 0, "M:WRITE  *:NODES  F:SETUP");
        return;
    }

    uint8_t r = 0;
    for (uint8_t i = chat_top; i < gHermesMsgCount && r < ROWS; i++) {
        HermesMessage_t *m = &gHermesMessages[i];
        m->is_read = 1;
        if (!m->is_system) {
            call_of(m->addr, call);
            if (m->is_outgoing)
                sprintf(s, ">%s %s", call,
                        m->is_pending ? "..." : m->is_failed ? "FAIL" : m->is_acked ?
                        (m->addressing == HM_ADDR_UNICAST ? "OK" : "RLY") : "SENT");
            else
                sprintf(s, "<%s%s %ddBm", call, m->addressing == HM_ADDR_UNICAST ? "" : ">ALL", m->rssi);
            row(r++, 0, s);
        }
        char t[HM_MSG_MAX_TEXT + 1];
        uint8_t n = msg_text(m, t);
        for (uint8_t o = 0; o < n && r < ROWS; o += COLS) {
            char part[COLS + 1];
            strncpy(part, t + o, COLS);
            part[COLS] = '\0';
            row(r++, m->is_system ? 0 : 4, part);
        }
    }
}

static void draw_compose(void) {
    char s[40];
    const HermesNeighbor_t *n = dest_idx >= 0 ? HERMES_DISC_GetNeighbor(dest_idx) : NULL;
    char call[12];
    if (n) call_of(n->node_id, call); else strcpy(call, "ALL");
    sprintf(s, "TO:%s  %u/%u", call, edit_len, edit_max);
    row(0, 0, s);

    char buf[HM_MSG_MAX_TEXT + 2];
    strcpy(buf, edit_buf);
    if (mt_key == KEY_INVALID)
        strcat(buf, "_");
    for (uint8_t o = 0, r = 1; r < 7; o += COLS, r++) {
        if (o > strlen(buf)) break;
        char part[COLS + 1];
        strncpy(part, buf + o, COLS);
        part[COLS] = '\0';
        row(r, 0, part);
    }
    row(7, 0, "M:SEND *:DEL UP/DN:TO");
}

static void draw_neigh(void) {
    char s[40], call[12];
    const uint8_t cnt = HERMES_DISC_GetNeighborCount();
    sprintf(s, "NODES %u  M:MSG  *:BEACON", cnt);
    row(0, 0, s);
    for (uint8_t i = 0; i < cnt && i < ROWS - 1; i++) {
        const HermesNeighbor_t *n = HERMES_DISC_GetNeighbor(i);
        call_of(n->node_id, call);
        sprintf(s, "%c%-10s %4ddBm %3u%%", i == sel ? '>' : ' ', call, n->rssi, n->battery);
        row(i + 1, 0, s);
    }
}

static const char *const SET_NAMES[N_SETTINGS] = {
    "HERMES", "CALL", "KEY", "TTL", "RELAY", "ACK", "MUTE FSK", "BEACON"
};
static const uint8_t SET_FLAGS[N_SETTINGS] = { HM_F_ENABLED, 0, 0, 0, HM_F_RELAY, HM_F_ACK, HM_F_MUTE, HM_F_BEACON };

static void draw_settings(void) {
    char s[40];
    for (uint8_t i = 0; i < N_SETTINGS; i++) {
        const char *v;
        char num[4];
        if (SET_FLAGS[i])
            v = (gHermesConfig.flags & SET_FLAGS[i]) ? "ON" : "OFF";
        else if (i == 1)
            v = gHermesConfig.alias;
        else if (i == 2)
            v = gHermesConfig.passcode[0] ? "SET" : "OPEN";
        else {
            sprintf(num, "%u", gHermesConfig.ttl);
            v = num;
        }
        sprintf(s, "%c%-9s%s", i == sel ? '>' : ' ', SET_NAMES[i], v);
        row(i, 0, s);
    }
}

void UI_DisplayHermes(void) {
    UI_DisplayClear();
    switch (view) {
        case V_CHAT:     draw_chat();     break;
        case V_COMPOSE:  draw_compose();  break;
        case V_NEIGH:    draw_neigh();    break;
        case V_SETTINGS: draw_settings(); break;
    }
    ST7565_BlitFullScreen();
}

// ─────────────────────────── input ───────────────────────────

static void compose_start(int8_t dest) {
    view     = V_COMPOSE;
    dest_idx = dest;
    edit_max = HM_MSG_MAX_TEXT;
    edit_len = 0;
    edit_buf[0] = '\0';
    mt_key   = KEY_INVALID;
}

static void edit_key(KEY_Code_t key, bool held) {
    if (key == KEY_STAR) {
        mt_key = KEY_INVALID;
        if (held)
            edit_len = 0;
        else if (edit_len)
            edit_len--;
        edit_buf[edit_len] = '\0';
        return;
    }
    if (key > KEY_9 || held)
        return;
    const char *set = MT[key];
    if (key == mt_key && mt_timer && edit_len) {
        mt_idx = (mt_idx + 1) % strlen(set);
        edit_buf[edit_len - 1] = set[mt_idx];
    } else if (edit_len < edit_max) {
        mt_idx = 0;
        edit_buf[edit_len++] = set[0];
        edit_buf[edit_len] = '\0';
    }
    mt_key   = key;
    mt_timer = 100;                          // 1 s to cycle the same key
}

static void compose_send(void) {
    uint8_t dest[HM_NODE_ID_SIZE];
    const char *text = edit_buf;

    memcpy(dest, HM_ADDR_BCAST, HM_NODE_ID_SIZE);
    if (dest_idx >= 0) {
        const HermesNeighbor_t *n = HERMES_DISC_GetNeighbor(dest_idx);
        if (n) memcpy(dest, n->node_id, HM_NODE_ID_SIZE);
    }
    if (edit_buf[0] == '@') {                // "@CALL text"
        char call[11];
        uint8_t i = 0;
        while (edit_buf[1 + i] && edit_buf[1 + i] != ' ' && i < 9) {
            call[i] = edit_buf[1 + i];
            i++;
        }
        call[i] = '\0';
        if (i) HERMES_Addr_Encode(call, dest);
        text = edit_buf + 1 + i;
        while (*text == ' ') text++;
    }
    if (*text)
        HERMES_QueueText(text, dest);
}

void HERMES_Open(void) {
    HERMES_RefreshIdentity();                // U.Info MY CALL / MESHKEY may have changed
    view     = V_CHAT;
    dest_idx = -1;
    gHermesHasNewMessage = false;
    gUpdateStatus = true;
    scroll_to_end();
    gRequestDisplayScreen = DISPLAY_HERMES;
}

void HERMES_UI_Tick10ms(void) {
    if (mt_timer && --mt_timer == 0) {
        mt_key = KEY_INVALID;
        if (gScreenToDisplay == DISPLAY_HERMES)
            gUpdateDisplay = true;
    }
    if (gScreenToDisplay == DISPLAY_HERMES && view == V_CHAT && gHermesHasNewMessage) {
        gHermesHasNewMessage = false;
        gUpdateStatus = true;
        scroll_to_end();
        gUpdateDisplay = true;
    }
}

void HERMES_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) {
    if (Key == KEY_PTT) {
        GENERIC_Key_PTT(bKeyPressed);
        return;
    }
    if (!bKeyPressed)
        return;
    if (bKeyHeld && !(Key == KEY_STAR && view == V_COMPOSE))
        return;

    gUpdateDisplay = true;
    const uint8_t ncnt = HERMES_DISC_GetNeighborCount();

    switch (view) {
        case V_CHAT:
            if (Key == KEY_UP && chat_top > 0)                         chat_top--;
            else if (Key == KEY_DOWN && chat_top + 1 < gHermesMsgCount) chat_top++;
            else if (Key == KEY_MENU) compose_start(-1);
            else if (Key == KEY_STAR) { view = V_NEIGH; sel = 0; }
            else if (Key == KEY_F)    { view = V_SETTINGS; sel = 0; }
            else if (Key == KEY_EXIT) gRequestDisplayScreen = DISPLAY_MAIN;
            break;

        case V_COMPOSE:
            if (Key == KEY_EXIT)
                view = V_CHAT;
            else if (Key == KEY_MENU) {
                compose_send();
                view = V_CHAT;
                scroll_to_end();
            } else if (Key == KEY_UP) {
                dest_idx = (dest_idx + 1 >= (int8_t)ncnt) ? -1 : dest_idx + 1;
            } else if (Key == KEY_DOWN) {
                dest_idx = (dest_idx < 0) ? (int8_t)ncnt - 1 : dest_idx - 1;
            } else
                edit_key(Key, bKeyHeld);
            break;

        case V_NEIGH:
            if (Key == KEY_UP && sel > 0)             sel--;
            else if (Key == KEY_DOWN && sel + 1 < ncnt) sel++;
            else if (Key == KEY_MENU && ncnt) compose_start(sel);
            else if (Key == KEY_STAR)         HERMES_QueueBeacon();
            else if (Key == KEY_EXIT)         view = V_CHAT;
            break;

        case V_SETTINGS:
            if (Key == KEY_UP)        sel = (sel + N_SETTINGS - 1) % N_SETTINGS;
            else if (Key == KEY_DOWN) sel = (sel + 1) % N_SETTINGS;
            else if (Key == KEY_MENU) {
                if (SET_FLAGS[sel]) {
                    gHermesConfig.flags ^= SET_FLAGS[sel];
                    if (sel == 0) {
                        HERMES_SaveSettings();
                        HERMES_ApplySettings();
                    }
                } else if (sel == 3) {
                    gHermesConfig.ttl = gHermesConfig.ttl >= 7 ? 1 : gHermesConfig.ttl + 1;
                }
            } else if (Key == KEY_EXIT) {
                HERMES_SaveSettings();
                view = V_CHAT;
            }
            break;
    }
}

#endif // ENABLE_HERMES
