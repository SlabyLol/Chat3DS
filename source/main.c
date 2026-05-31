/**
 * 3DS-Chat - Homebrew LAN/IP Chat App
 * Kompiliert mit devkitARM + libctru + citro2d
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

// 3DS-spezifische Netzwerk-Headers (libctru / newlib-nano)
#include <3ds.h>
#include <3ds/types.h>
#include <3ds/services/soc.h>

// POSIX-Sockets kommen aus libctru's newlib – erst NACH 3ds.h einbinden
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include <citro2d.h>

// ─── Konstanten ──────────────────────────────────────────────────────────────
#define SCREEN_TOP_W    400
#define SCREEN_TOP_H    240
#define SCREEN_BOT_W    320
#define SCREEN_BOT_H    240

#define CHAT_PORT       6464
#define MAX_MESSAGES    20
#define MSG_MAX_LEN     200
#define NAME_MAX_LEN    24
#define PASS_MAX_LEN    32
#define IP_MAX_LEN      16

#define SOC_BUFFER_SIZE (128 * 1024)

// ─── Farben ──────────────────────────────────────────────────────────────────
#define CLR_BG        C2D_Color32(0x1A,0x1A,0x2E,0xFF)
#define CLR_PANEL     C2D_Color32(0x16,0x21,0x3E,0xFF)
#define CLR_ACCENT    C2D_Color32(0x0F,0x3C,0x78,0xFF)
#define CLR_ACCENT2   C2D_Color32(0xE9,0x4C,0x60,0xFF)
#define CLR_TEXT      C2D_Color32(0xE0,0xE0,0xFF,0xFF)
#define CLR_TEXT_DIM  C2D_Color32(0x80,0x80,0xA0,0xFF)
#define CLR_SELF_MSG  C2D_Color32(0x0F,0x3C,0x78,0xFF)
#define CLR_PEER_MSG  C2D_Color32(0x2A,0x2A,0x4A,0xFF)
#define CLR_SYS_MSG   C2D_Color32(0x1A,0x3A,0x1A,0xFF)
#define CLR_WHITE     C2D_Color32(0xFF,0xFF,0xFF,0xFF)
#define CLR_GREEN     C2D_Color32(0x4C,0xFF,0x91,0xFF)
#define CLR_RED       C2D_Color32(0xFF,0x4C,0x4C,0xFF)
#define CLR_YELLOW    C2D_Color32(0xFF,0xD7,0x00,0xFF)
#define CLR_BTN       C2D_Color32(0x0F,0x3C,0x78,0xFF)
#define CLR_BTN_HL    C2D_Color32(0x1A,0x5C,0xA8,0xFF)
#define CLR_INPUT_BG  C2D_Color32(0x0A,0x0A,0x1E,0xFF)

// ─── Typen ───────────────────────────────────────────────────────────────────
typedef enum { SCREEN_MENU, SCREEN_HOST, SCREEN_JOIN, SCREEN_CHAT, SCREEN_ERROR } AppScreen;
typedef enum { MSG_SELF, MSG_PEER, MSG_SYSTEM } MsgType;

typedef struct {
    char    text[MSG_MAX_LEN];
    char    sender[NAME_MAX_LEN];
    MsgType type;
} Message;

typedef struct {
    AppScreen screen;
    char      username[NAME_MAX_LEN];
    char      password[PASS_MAX_LEN];
    bool      use_password;
    int       server_sock;
    int       client_sock;
    bool      is_host;
    bool      connected;
    char      peer_ip[IP_MAX_LEN];
    char      my_ip[IP_MAX_LEN];
    Message   messages[MAX_MESSAGES];
    int       msg_count;
    int       scroll_offset;
    char      error_msg[200];
    u32       frame;
    int       selected_btn; // 1=HOST, 2=JOIN
} AppState;

// ─── Globals ─────────────────────────────────────────────────────────────────
static u32*     soc_buffer = NULL;
static AppState app;

// ─── Hilfsfunktionen ─────────────────────────────────────────────────────────
static void add_message(const char *sender, const char *text, MsgType type) {
    if (app.msg_count >= MAX_MESSAGES) {
        memmove(&app.messages[0], &app.messages[1],
                sizeof(Message) * (MAX_MESSAGES - 1));
        app.msg_count = MAX_MESSAGES - 1;
    }
    Message *m = &app.messages[app.msg_count++];
    strncpy(m->sender, sender, NAME_MAX_LEN - 1);
    m->sender[NAME_MAX_LEN-1] = '\0';
    strncpy(m->text, text, MSG_MAX_LEN - 1);
    m->text[MSG_MAX_LEN-1] = '\0';
    m->type = type;
    // Scroll automatisch nach unten
    app.scroll_offset = 0;
}

static void sys_msg(const char *fmt, ...) {
    char buf[MSG_MAX_LEN];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    add_message("System", buf, MSG_SYSTEM);
}

static bool open_keyboard(const char *hint, char *out, size_t max_len, bool secret) {
    SwkbdState kbd;
    swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, (int)(max_len - 1));
    swkbdSetHintText(&kbd, hint);
    if (secret) swkbdSetPasswordMode(&kbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetButton(&kbd, SWKBD_BUTTON_LEFT,  "Abbrechen", false);
    swkbdSetButton(&kbd, SWKBD_BUTTON_RIGHT, "OK", true);
    char tmp[MSG_MAX_LEN] = {0};
    SwkbdButton btn = swkbdInputText(&kbd, tmp, sizeof(tmp));
    if (btn == SWKBD_BUTTON_RIGHT && strlen(tmp) > 0) {
        strncpy(out, tmp, max_len - 1);
        out[max_len-1] = '\0';
        return true;
    }
    return false;
}

static void get_my_ip(void) {
    struct in_addr addr;
    addr.s_addr = (u32)gethostid();
    strncpy(app.my_ip, inet_ntoa(addr), IP_MAX_LEN - 1);
}

static void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// ─── Protokoll ───────────────────────────────────────────────────────────────
// Header: [1 Byte Typ] [2 Byte Länge (big-endian)]
#define PKT_HANDSHAKE 'H'
#define PKT_MESSAGE   'M'
#define PKT_DISCONNECT 'D'

static int send_raw(int sock, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int send_packet(int sock, char type, const void *data, u16 dlen) {
    u8 hdr[3];
    hdr[0] = (u8)type;
    hdr[1] = (u8)(dlen >> 8);
    hdr[2] = (u8)(dlen & 0xFF);
    if (send_raw(sock, hdr, 3) < 0) return -1;
    if (dlen > 0 && data) {
        if (send_raw(sock, data, dlen) < 0) return -1;
    }
    return 0;
}

static int send_handshake(int sock) {
    char buf[NAME_MAX_LEN + PASS_MAX_LEN + 2];
    int pos = 0;
    strncpy(buf + pos, app.username, NAME_MAX_LEN);
    pos += (int)strlen(app.username) + 1;
    strncpy(buf + pos, app.password, PASS_MAX_LEN);
    pos += (int)strlen(app.password) + 1;
    return send_packet(sock, PKT_HANDSHAKE, buf, (u16)pos);
}

static int send_chat(const char *text) {
    char buf[NAME_MAX_LEN + MSG_MAX_LEN + 2];
    int pos = 0;
    strncpy(buf + pos, app.username, NAME_MAX_LEN);
    pos += (int)strlen(app.username) + 1;
    strncpy(buf + pos, text, MSG_MAX_LEN);
    pos += (int)strlen(text) + 1;
    return send_packet(app.client_sock, PKT_MESSAGE, buf, (u16)pos);
}

// ─── Netzwerk ────────────────────────────────────────────────────────────────
static bool net_host(void) {
    app.server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (app.server_sock < 0) return false;
    int opt = 1;
    setsockopt(app.server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(CHAT_PORT);
    if (bind(app.server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(app.server_sock); app.server_sock = -1; return false;
    }
    if (listen(app.server_sock, 1) < 0) {
        closesocket(app.server_sock); app.server_sock = -1; return false;
    }
    set_nonblocking(app.server_sock);
    app.client_sock = -1;
    return true;
}

static bool net_join(const char *ip) {
    app.client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (app.client_sock < 0) return false;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(CHAT_PORT);
    if (inet_aton(ip, &addr.sin_addr) == 0) {
        closesocket(app.client_sock); app.client_sock = -1; return false;
    }
    if (connect(app.client_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(app.client_sock); app.client_sock = -1; return false;
    }
    set_nonblocking(app.client_sock);
    send_handshake(app.client_sock);
    return true;
}

static void net_disconnect(void) {
    if (app.client_sock >= 0) {
        send_packet(app.client_sock, PKT_DISCONNECT, NULL, 0);
        closesocket(app.client_sock);
        app.client_sock = -1;
    }
    if (app.server_sock >= 0) {
        closesocket(app.server_sock);
        app.server_sock = -1;
    }
    app.connected = false;
}

static void poll_network(void) {
    // Host: auf neue Verbindung warten
    if (app.is_host && app.client_sock < 0 && app.server_sock >= 0) {
        struct sockaddr_in peer_addr;
        socklen_t addr_len = sizeof(peer_addr);
        int ns = accept(app.server_sock, (struct sockaddr*)&peer_addr, &addr_len);
        if (ns >= 0) {
            app.client_sock = ns;
            set_nonblocking(ns);
            strncpy(app.peer_ip, inet_ntoa(peer_addr.sin_addr), IP_MAX_LEN - 1);
            send_handshake(ns);
            app.connected = true;
            app.screen    = SCREEN_CHAT;
            sys_msg("Verbunden mit %s", app.peer_ip);
        }
    }

    if (app.client_sock < 0) return;

    // Paket-Header lesen (3 Bytes)
    u8 hdr[3];
    ssize_t n = recv(app.client_sock, hdr, 3, MSG_PEEK);
    if (n == 0) {
        sys_msg("Verbindung getrennt.");
        closesocket(app.client_sock); app.client_sock = -1;
        app.connected = false;
        return;
    }
    if (n < 3) return; // noch nicht genug Daten

    // Header konsumieren
    recv(app.client_sock, hdr, 3, 0);
    char   type = (char)hdr[0];
    u16    dlen = (u16)((hdr[1] << 8) | hdr[2]);

    char data[NAME_MAX_LEN + MSG_MAX_LEN + 4];
    memset(data, 0, sizeof(data));
    if (dlen > 0 && dlen < sizeof(data)) {
        int got = 0;
        while (got < dlen) {
            ssize_t r = recv(app.client_sock, data + got, dlen - got, 0);
            if (r <= 0) break;
            got += (int)r;
        }
    }

    if (type == PKT_HANDSHAKE) {
        char peer_name[NAME_MAX_LEN] = {0};
        char peer_pass[PASS_MAX_LEN] = {0};
        strncpy(peer_name, data, NAME_MAX_LEN - 1);
        size_t nlen = strlen(peer_name) + 1;
        if (dlen > nlen) strncpy(peer_pass, data + nlen, PASS_MAX_LEN - 1);

        if (app.use_password && strcmp(peer_pass, app.password) != 0) {
            send_packet(app.client_sock, PKT_DISCONNECT, "Falsches Passwort", 18);
            closesocket(app.client_sock); app.client_sock = -1;
            app.connected = false;
            sys_msg("Abgelehnt: Falsches Passwort");
            return;
        }
        sys_msg("%s ist beigetreten!", peer_name);
        if (!app.is_host) {
            app.connected = true;
            app.screen    = SCREEN_CHAT;
        }
    } else if (type == PKT_MESSAGE) {
        char sender[NAME_MAX_LEN] = {0};
        strncpy(sender, data, NAME_MAX_LEN - 1);
        size_t slen = strlen(sender) + 1;
        if (dlen > slen) add_message(sender, data + slen, MSG_PEER);
    } else if (type == PKT_DISCONNECT) {
        sys_msg("Gegenseite hat getrennt.");
        closesocket(app.client_sock); app.client_sock = -1;
        app.connected = false;
    }
}

// ─── Zeichnen ────────────────────────────────────────────────────────────────
static void draw_rect(float x, float y, float w, float h, u32 clr) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, clr);
}

static void draw_text(float x, float y, float scale, u32 clr, const char *txt) {
    C2D_Text t;
    C2D_TextBuf buf = C2D_TextBufNew(512);
    C2D_TextParse(&t, buf, txt);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_AtBaseline, x, y, 0.5f, scale, scale, clr);
    C2D_TextBufDelete(buf);
}

static void draw_textf(float x, float y, float scale, u32 clr, const char *fmt, ...) {
    char buf[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    draw_text(x, y, scale, clr, buf);
}

static void draw_button(float x, float y, float w, float h, const char *label, bool hl) {
    draw_rect(x, y, w, h, hl ? CLR_BTN_HL : CLR_BTN);
    draw_rect(x,         y,     w, 1, CLR_ACCENT2);
    draw_rect(x,     y+h-1,     w, 1, CLR_ACCENT2);
    draw_rect(x,         y,     1, h, CLR_ACCENT2);
    draw_rect(x+w-1,     y,     1, h, CLR_ACCENT2);
    float tx = x + w/2 - (float)strlen(label) * 3.0f;
    draw_text(tx, y + h/2 + 5, 0.55f, CLR_WHITE, label);
}

// ── Menü ─────────────────────────────────────────────────────────────────────
static void draw_menu(void) {
    // TOP
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 38, CLR_PANEL);
    draw_text(12, 26, 0.75f, CLR_ACCENT2, "3DS-CHAT");
    draw_text(120, 26, 0.50f, CLR_TEXT_DIM, "Homebrew LAN/IP Chat");
    draw_rect(0, 38, SCREEN_TOP_W, 1, CLR_ACCENT);
    draw_text(12, 65, 0.50f, CLR_TEXT_DIM, "Deine IP:");
    draw_textf(90, 65, 0.50f, CLR_GREEN, "%s", app.my_ip[0] ? app.my_ip : "...");
    draw_textf(90, 85, 0.50f, CLR_TEXT_DIM, "Port: %d", CHAT_PORT);
    draw_rect(10, 108, 380, 1, CLR_ACCENT);
    draw_text(12, 128, 0.48f, CLR_TEXT, "HOST: Warte auf Verbindung, gib deine IP weiter");
    draw_text(12, 148, 0.48f, CLR_TEXT, "JOIN: Verbinde zu einem Host per IP");
    draw_text(12, 168, 0.48f, CLR_TEXT_DIM, "Beide 3DS muessen im gleichen WLAN sein.");
    // BOTTOM
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_rect(0, 0, SCREEN_BOT_W, 28, CLR_PANEL);
    draw_text(10, 19, 0.52f, CLR_TEXT_DIM, "Was moechtest du tun?");
    draw_button(15,  48, 130, 48, "HOST", app.selected_btn == 1);
    draw_button(175, 48, 130, 48, "JOIN", app.selected_btn == 2);
    draw_text(10, 115, 0.46f, CLR_TEXT_DIM, "Dpad: wahlen  A: bestatigen  B: Passwort");
    draw_textf(10, 133, 0.46f, app.use_password ? CLR_YELLOW : CLR_TEXT_DIM,
               "Passwort: %s", app.use_password ? "AN" : "AUS");
    draw_text(10, 210, 0.42f, CLR_TEXT_DIM, "START = Beenden");
}

// ── Host-Warte ────────────────────────────────────────────────────────────────
static void draw_host_wait(void) {
    static const char *spin[] = {"|","/","-","\\"};
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 38, CLR_PANEL);
    draw_text(12, 26, 0.65f, CLR_ACCENT2, "3DS-CHAT / HOST");
    draw_rect(0, 38, SCREEN_TOP_W, 1, CLR_ACCENT);
    draw_textf(12, 75, 0.65f, CLR_GREEN, "%s  Warte auf Verbindung...",
               spin[(app.frame / 12) % 4]);
    draw_text(12, 108, 0.50f, CLR_TEXT, "Gib diese IP an den anderen Spieler:");
    draw_textf(12, 132, 0.70f, CLR_YELLOW, "%s", app.my_ip);
    draw_textf(12, 158, 0.48f, CLR_TEXT_DIM, "Port: %d", CHAT_PORT);
    if (app.use_password) draw_text(12, 180, 0.48f, CLR_YELLOW, "Passwort-Schutz: AKTIV");
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_button(85, 95, 150, 40, "Abbrechen (B)", false);
}

// ── Join ─────────────────────────────────────────────────────────────────────
static void draw_join(void) {
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 38, CLR_PANEL);
    draw_text(12, 26, 0.65f, CLR_ACCENT2, "3DS-CHAT / JOIN");
    draw_rect(0, 38, SCREEN_TOP_W, 1, CLR_ACCENT);
    draw_text(12, 75, 0.52f, CLR_TEXT, "Gib die IP des Hosts ein (A).");
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_button(85,  50, 150, 40, "IP eingeben (A)", true);
    draw_button(85, 105, 150, 40, "Zurueck   (B)", false);
}

// ── Chat ─────────────────────────────────────────────────────────────────────
static void draw_chat(void) {
    // TOP: Nachrichten
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 20, CLR_PANEL);
    draw_text(6, 14, 0.46f, CLR_ACCENT2, "3DS-Chat");
    u32 status_clr = app.connected ? CLR_GREEN : CLR_RED;
    draw_rect(80, 7, 6, 6, status_clr);
    draw_text(90, 14, 0.42f, CLR_TEXT_DIM, app.connected ? "Verbunden" : "Getrennt");
    draw_rect(0, 20, SCREEN_TOP_W, 1, CLR_ACCENT);

    int vis = (SCREEN_TOP_H - 22) / 20;
    int start = app.msg_count - vis + app.scroll_offset;
    if (start < 0) start = 0;
    float y = 26;
    for (int i = start; i < app.msg_count && y < SCREEN_TOP_H - 4; i++) {
        Message *m = &app.messages[i];
        u32 bg = (m->type == MSG_SELF) ? CLR_SELF_MSG :
                 (m->type == MSG_PEER) ? CLR_PEER_MSG : CLR_SYS_MSG;
        draw_rect(2, y-1, SCREEN_TOP_W-4, 18, bg);
        if (m->type == MSG_SYSTEM) {
            draw_textf(5, y+12, 0.42f, CLR_GREEN, "** %s **", m->text);
        } else {
            draw_textf(5, y+12, 0.40f, CLR_TEXT_DIM, "%s:", m->sender);
            int off = (int)strlen(m->sender)*5 + 10;
            draw_textf(5+off, y+12, 0.43f, CLR_WHITE, "%s", m->text);
        }
        y += 20;
    }

    // BOTTOM: Eingabe
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_rect(0, 0, SCREEN_BOT_W, 20, CLR_PANEL);
    draw_textf(6, 14, 0.44f, CLR_TEXT_DIM, "Als: %s", app.username);
    draw_rect(0, 20, SCREEN_BOT_W, 1, CLR_ACCENT);
    draw_rect(4, 28, 312, 28, CLR_INPUT_BG);
    draw_rect(4, 28, 312, 1, CLR_ACCENT);
    draw_rect(4, 55, 312, 1, CLR_ACCENT);
    draw_text(8, 47, 0.46f, CLR_TEXT_DIM, "A = Nachricht schreiben...");
    draw_button(4,   65, 148, 28, "Senden (A)", app.connected);
    draw_button(168, 65, 148, 28, "Trennen (B)", false);
    draw_text(4, 108, 0.40f, CLR_TEXT_DIM, "Oben/Unten scrollen  START = Menu");
    draw_rect(0, SCREEN_BOT_H-4, SCREEN_BOT_W, 4, status_clr);
}

// ── Fehler ───────────────────────────────────────────────────────────────────
static void draw_error(void) {
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 38, C2D_Color32(0x4A,0x10,0x10,0xFF));
    draw_text(12, 26, 0.65f, CLR_RED, "FEHLER");
    draw_rect(0, 38, SCREEN_TOP_W, 1, CLR_RED);
    draw_textf(12, 75, 0.48f, CLR_WHITE, "%s", app.error_msg);
    draw_text(12, 120, 0.48f, CLR_TEXT_DIM, "B = Zurueck");
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_button(85, 95, 150, 40, "Zurueck (B)", true);
}

// ─── Input ───────────────────────────────────────────────────────────────────
static void handle_menu(u32 down) {
    if (down & KEY_DLEFT)  app.selected_btn = 1;
    if (down & KEY_DRIGHT) app.selected_btn = 2;
    if (down & KEY_B) {
        app.use_password = !app.use_password;
        if (app.use_password)
            open_keyboard("Chat-Passwort (optional)", app.password, PASS_MAX_LEN, true);
    }
    if (down & KEY_A) {
        if (!app.username[0])
            if (!open_keyboard("Dein Nickname", app.username, NAME_MAX_LEN, false))
                strncpy(app.username, "Spieler", NAME_MAX_LEN);
        if (app.selected_btn == 1) {
            if (net_host()) { app.is_host = true; app.screen = SCREEN_HOST; }
            else { snprintf(app.error_msg, sizeof(app.error_msg),
                            "Server konnte nicht gestartet werden."); app.screen = SCREEN_ERROR; }
        } else {
            app.screen = SCREEN_JOIN;
        }
    }
}

static void handle_host(u32 down) {
    if (down & KEY_B) { net_disconnect(); app.screen = SCREEN_MENU; }
}

static void handle_join(u32 down) {
    if (down & KEY_B) { app.screen = SCREEN_MENU; return; }
    if (down & KEY_A) {
        char ip[IP_MAX_LEN] = {0};
        if (open_keyboard("Host-IP (z.B. 192.168.1.5)", ip, IP_MAX_LEN, false)) {
            if (app.use_password)
                open_keyboard("Host-Passwort", app.password, PASS_MAX_LEN, true);
            strncpy(app.peer_ip, ip, IP_MAX_LEN-1);
            app.is_host = false;
            if (!net_join(ip)) {
                snprintf(app.error_msg, sizeof(app.error_msg),
                         "Verbindung zu %s fehlgeschlagen!", ip);
                app.screen = SCREEN_ERROR;
            } else {
                sys_msg("Verbinde zu %s...", ip);
            }
        }
    }
}

static void handle_chat(u32 down) {
    if (down & KEY_START) { net_disconnect(); app.screen = SCREEN_MENU; return; }
    if (down & KEY_B) { if (app.connected) { net_disconnect(); sys_msg("Getrennt."); } return; }
    if (down & KEY_DUP)   { if (app.scroll_offset > -(app.msg_count)) app.scroll_offset--; }
    if (down & KEY_DDOWN) { if (app.scroll_offset < 0) app.scroll_offset++; }
    if ((down & KEY_A) && app.connected) {
        char msg[MSG_MAX_LEN] = {0};
        if (open_keyboard("Nachricht eingeben", msg, MSG_MAX_LEN, false)) {
            add_message(app.username, msg, MSG_SELF);
            if (send_chat(msg) < 0) sys_msg("Sendefehler!");
        }
    }
}

static void handle_error(u32 down) {
    if (down & KEY_B) { net_disconnect(); app.screen = SCREEN_MENU; }
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // SOC init
    soc_buffer = (u32*)memalign(0x1000, SOC_BUFFER_SIZE);
    if (!soc_buffer) { gfxExit(); return 1; }
    socInit(soc_buffer, SOC_BUFFER_SIZE);

    // State init
    memset(&app, 0, sizeof(app));
    app.screen       = SCREEN_MENU;
    app.server_sock  = -1;
    app.client_sock  = -1;
    app.selected_btn = 1;
    get_my_ip();
    sys_msg("3DS-Chat bereit. IP: %s", app.my_ip);

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_START && app.screen == SCREEN_MENU) break;

        if (app.screen == SCREEN_HOST || app.screen == SCREEN_CHAT)
            poll_network();

        switch (app.screen) {
            case SCREEN_MENU:  handle_menu(down);  break;
            case SCREEN_HOST:  handle_host(down);  break;
            case SCREEN_JOIN:  handle_join(down);  break;
            case SCREEN_CHAT:  handle_chat(down);  break;
            case SCREEN_ERROR: handle_error(down); break;
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        switch (app.screen) {
            case SCREEN_MENU:  draw_menu();      break;
            case SCREEN_HOST:  draw_host_wait(); break;
            case SCREEN_JOIN:  draw_join();      break;
            case SCREEN_CHAT:  draw_chat();      break;
            case SCREEN_ERROR: draw_error();     break;
        }
        C3D_FrameEnd(0);
        app.frame++;
    }

    net_disconnect();
    socExit();
    free(soc_buffer);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
