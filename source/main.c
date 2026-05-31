/**
 * 3DS-Chat - Homebrew LAN/IP Chat App
 * Kompiliert mit devkitARM + libctru + citro2d
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <3ds.h>
#include <citro2d.h>

// ─── Konstanten ───────────────────────────────────────────────────────────────
#define SCREEN_TOP_W   400
#define SCREEN_TOP_H   240
#define SCREEN_BOT_W   320
#define SCREEN_BOT_H   240

#define CHAT_PORT      6464
#define MAX_MESSAGES   20
#define MSG_MAX_LEN    256
#define NAME_MAX_LEN   24
#define PASS_MAX_LEN   32
#define IP_MAX_LEN     16

#define SOC_BUFFER_SIZE (0x100000)

// ─── Farben ───────────────────────────────────────────────────────────────────
#define CLR_BG          C2D_Color32(0x1A, 0x1A, 0x2E, 0xFF)
#define CLR_PANEL       C2D_Color32(0x16, 0x21, 0x3E, 0xFF)
#define CLR_ACCENT      C2D_Color32(0x0F, 0x3C, 0x78, 0xFF)
#define CLR_ACCENT2     C2D_Color32(0xE9, 0x4C, 0x60, 0xFF)
#define CLR_TEXT        C2D_Color32(0xE0, 0xE0, 0xFF, 0xFF)
#define CLR_TEXT_DIM    C2D_Color32(0x80, 0x80, 0xA0, 0xFF)
#define CLR_SELF_MSG    C2D_Color32(0x0F, 0x3C, 0x78, 0xFF)
#define CLR_PEER_MSG    C2D_Color32(0x2A, 0x2A, 0x4A, 0xFF)
#define CLR_SYS_MSG     C2D_Color32(0x2A, 0x4A, 0x2A, 0xFF)
#define CLR_WHITE       C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_GREEN       C2D_Color32(0x4C, 0xFF, 0x91, 0xFF)
#define CLR_RED         C2D_Color32(0xFF, 0x4C, 0x4C, 0xFF)
#define CLR_YELLOW      C2D_Color32(0xFF, 0xD7, 0x00, 0xFF)
#define CLR_BTN         C2D_Color32(0x0F, 0x3C, 0x78, 0xFF)
#define CLR_BTN_HL      C2D_Color32(0x1A, 0x5C, 0xA8, 0xFF)
#define CLR_INPUT_BG    C2D_Color32(0x0A, 0x0A, 0x1E, 0xFF)
#define CLR_BORDER      C2D_Color32(0x0F, 0x3C, 0x78, 0x80)

// ─── Typen ────────────────────────────────────────────────────────────────────
typedef enum {
    SCREEN_MENU = 0,
    SCREEN_HOST,
    SCREEN_JOIN,
    SCREEN_CHAT,
    SCREEN_ERROR
} AppScreen;

typedef enum {
    MSG_SELF,
    MSG_PEER,
    MSG_SYSTEM
} MsgType;

typedef struct {
    char     text[MSG_MAX_LEN];
    char     sender[NAME_MAX_LEN];
    MsgType  type;
} Message;

typedef struct {
    AppScreen    screen;

    // Identität
    char         username[NAME_MAX_LEN];
    char         password[PASS_MAX_LEN];
    bool         use_password;

    // Netzwerk
    int          server_sock;
    int          client_sock;
    bool         is_host;
    bool         connected;
    char         peer_ip[IP_MAX_LEN];
    char         my_ip[IP_MAX_LEN];

    // Chat
    Message      messages[MAX_MESSAGES];
    int          msg_count;
    int          scroll_offset;

    // UI
    char         input_buf[MSG_MAX_LEN];
    char         error_msg[256];
    u32          frame;

    // Bottom-Screen Buttons
    int          selected_btn;  // 0=keine, 1=Host, 2=Join
} AppState;

// ─── Globals ──────────────────────────────────────────────────────────────────
static u32*      soc_buffer = NULL;
static C2D_Font  sys_font;
static AppState  app;

// ─── Hilfsfunktionen ──────────────────────────────────────────────────────────
static void add_message(const char* sender, const char* text, MsgType type) {
    if (app.msg_count >= MAX_MESSAGES) {
        // Älteste Nachricht rauswürfeln
        memmove(&app.messages[0], &app.messages[1],
                sizeof(Message) * (MAX_MESSAGES - 1));
        app.msg_count = MAX_MESSAGES - 1;
    }
    Message* m = &app.messages[app.msg_count++];
    strncpy(m->sender, sender, NAME_MAX_LEN - 1);
    strncpy(m->text,   text,   MSG_MAX_LEN  - 1);
    m->type = type;
}

static void sys_msg(const char* fmt, ...) {
    char buf[MSG_MAX_LEN];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    add_message("System", buf, MSG_SYSTEM);
}

// Keyboard öffnen und Text holen
static bool open_keyboard(const char* hint, char* out, size_t max_len, bool secret) {
    SwkbdState kbd;
    swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, max_len - 1);
    swkbdSetHintText(&kbd, hint);
    if (secret) swkbdSetPasswordMode(&kbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetButton(&kbd, SWKBD_BUTTON_LEFT,  "Abbrechen", false);
    swkbdSetButton(&kbd, SWKBD_BUTTON_RIGHT, "OK",        true);
    swkbdSetFeatures(&kbd, SWKBD_MULTILINE);

    char tmp[MSG_MAX_LEN] = {0};
    SwkbdButton btn = swkbdInputText(&kbd, tmp, sizeof(tmp));
    if (btn == SWKBD_BUTTON_RIGHT && strlen(tmp) > 0) {
        strncpy(out, tmp, max_len - 1);
        return true;
    }
    return false;
}

// Eigene IP holen
static void get_my_ip(void) {
    struct in_addr addr = {0};
    addr.s_addr = gethostid();
    strncpy(app.my_ip, inet_ntoa(addr), IP_MAX_LEN - 1);
}

// Socket non-blocking setzen
static void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// ─── Netzwerk ─────────────────────────────────────────────────────────────────

// Protokoll: [1 Byte Typ][2 Byte Länge][Daten]
// Typ: 'H' = Handshake, 'M' = Message, 'D' = Disconnect
#define PKT_HANDSHAKE 'H'
#define PKT_MESSAGE   'M'
#define PKT_DISCONNECT 'D'

typedef struct __attribute__((packed)) {
    u8  type;
    u16 length;  // Länge der Daten
} PktHeader;

// Komplettes Paket senden
static int send_packet(int sock, u8 type, const void* data, u16 len) {
    PktHeader hdr = { type, len };
    if (send(sock, &hdr, sizeof(hdr), 0) != sizeof(hdr)) return -1;
    if (len > 0 && data) {
        if (send(sock, data, len, 0) != len) return -1;
    }
    return 0;
}

// Handshake-Payload: username\0password\0
static int send_handshake(int sock) {
    char buf[NAME_MAX_LEN + PASS_MAX_LEN + 2];
    int pos = 0;
    strncpy(buf + pos, app.username, NAME_MAX_LEN); pos += strlen(app.username) + 1;
    strncpy(buf + pos, app.password, PASS_MAX_LEN); pos += strlen(app.password) + 1;
    return send_packet(sock, PKT_HANDSHAKE, buf, (u16)pos);
}

static int sock_active(void) {
    // Beide Modi nutzen client_sock für aktive Kommunikation
    return app.client_sock;
}

// Chat-Nachricht senden
static int send_chat_message(const char* text) {
    char buf[NAME_MAX_LEN + MSG_MAX_LEN + 2];
    int pos = 0;
    strncpy(buf + pos, app.username, NAME_MAX_LEN); pos += strlen(app.username) + 1;
    strncpy(buf + pos, text,         MSG_MAX_LEN);  pos += strlen(text) + 1;
    return send_packet(sock_active(), PKT_MESSAGE, buf, (u16)pos);
}

// Eingehende Daten prüfen (non-blocking)
static void poll_network(void) {
    int sock = app.is_host ? app.client_sock : app.client_sock;
    if (sock < 0) {
        if (app.is_host) {
            // Auf Verbindung warten
            struct sockaddr_in peer_addr;
            socklen_t addr_len = sizeof(peer_addr);
            int new_sock = accept(app.server_sock, (struct sockaddr*)&peer_addr, &addr_len);
            if (new_sock >= 0) {
                app.client_sock = new_sock;
                set_nonblocking(new_sock);
                strncpy(app.peer_ip, inet_ntoa(peer_addr.sin_addr), IP_MAX_LEN - 1);
                // Handshake empfangen + senden
                send_handshake(new_sock);
                sys_msg("Verbindung von %s!", app.peer_ip);
                app.connected = true;
                app.screen    = SCREEN_CHAT;
            }
        }
        return;
    }

    // Paket lesen
    PktHeader hdr;
    int n = recv(sock, &hdr, sizeof(hdr), 0);
    if (n == sizeof(hdr)) {
        char data[MSG_MAX_LEN + NAME_MAX_LEN + 4] = {0};
        if (hdr.length > 0 && hdr.length < sizeof(data)) {
            int total = 0;
            while (total < hdr.length) {
                int r = recv(sock, data + total, hdr.length - total, 0);
                if (r <= 0) break;
                total += r;
            }
        }

        if (hdr.type == PKT_HANDSHAKE) {
            // Peer-Name aus Handshake extrahieren
            char peer_name[NAME_MAX_LEN] = {0};
            char peer_pass[PASS_MAX_LEN] = {0};
            strncpy(peer_name, data, NAME_MAX_LEN - 1);
            if (hdr.length > (u16)(strlen(peer_name) + 1)) {
                strncpy(peer_pass, data + strlen(peer_name) + 1, PASS_MAX_LEN - 1);
            }
            // Passwort prüfen
            if (app.use_password && strcmp(peer_pass, app.password) != 0) {
                send_packet(sock, PKT_DISCONNECT, "Falsches Passwort", 18);
                closesocket(sock);
                app.client_sock = -1;
                app.connected   = false;
                sys_msg("Verbindung abgelehnt: Falsches Passwort");
                return;
            }
            sys_msg("** %s ist beigetreten! **", peer_name);
            if (!app.is_host) {
                app.connected = true;
                app.screen    = SCREEN_CHAT;
            }
        } else if (hdr.type == PKT_MESSAGE) {
            char sender[NAME_MAX_LEN] = {0};
            strncpy(sender, data, NAME_MAX_LEN - 1);
            size_t slen = strlen(sender) + 1;
            if (hdr.length > slen) {
                add_message(sender, data + slen, MSG_PEER);
            }
        } else if (hdr.type == PKT_DISCONNECT) {
            sys_msg("** Verbindung getrennt **");
            closesocket(sock);
            app.client_sock = -1;
            app.connected   = false;
        }
    } else if (n == 0) {
        // Verbindung geschlossen
        sys_msg("** Verbindung getrennt **");
        closesocket(sock);
        app.client_sock = -1;
        app.connected   = false;
    }
}

// ─── Netzwerk starten ─────────────────────────────────────────────────────────
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
        closesocket(app.server_sock);
        return false;
    }
    if (listen(app.server_sock, 1) < 0) {
        closesocket(app.server_sock);
        return false;
    }
    set_nonblocking(app.server_sock);
    app.client_sock = -1;
    return true;
}

static bool net_join(const char* ip) {
    app.client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (app.client_sock < 0) return false;

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(CHAT_PORT);
    if (inet_aton(ip, &addr.sin_addr) == 0) {
        closesocket(app.client_sock);
        return false;
    }

    if (connect(app.client_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(app.client_sock);
        app.client_sock = -1;
        return false;
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

// ─── Zeichnen ─────────────────────────────────────────────────────────────────
static void draw_rect(float x, float y, float w, float h, u32 clr) {
    C2D_DrawRectSolid(x, y, 0, w, h, clr);
}

static void draw_text(float x, float y, float scale, u32 clr, const char* txt) {
    C2D_Text t;
    C2D_TextBuf buf = C2D_TextBufNew(512);
    C2D_TextParse(&t, buf, txt);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_AtBaseline, x, y, 0, scale, scale, clr);
    C2D_TextBufDelete(buf);
}

static void draw_textf(float x, float y, float scale, u32 clr, const char* fmt, ...) {
    char buf[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    draw_text(x, y, scale, clr, buf);
}

static void draw_button(float x, float y, float w, float h,
                        const char* label, bool highlight) {
    u32 bg = highlight ? CLR_BTN_HL : CLR_BTN;
    draw_rect(x, y, w, h, bg);
    // Rand
    draw_rect(x,         y,         w, 1, CLR_ACCENT2);
    draw_rect(x,         y + h - 1, w, 1, CLR_ACCENT2);
    draw_rect(x,         y,         1, h, CLR_ACCENT2);
    draw_rect(x + w - 1, y,         1, h, CLR_ACCENT2);
    // Text zentriert
    float tx = x + w / 2 - strlen(label) * 3.0f;
    float ty = y + h / 2 + 5;
    draw_text(tx, ty, 0.55f, CLR_WHITE, label);
}

// ─── Screen: Menü (Top = Titel + Info, Bottom = Buttons) ──────────────────────
static void draw_menu(void) {
    // TOP SCREEN
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 40, CLR_PANEL);
    draw_text(12, 28, 0.75f, CLR_ACCENT2, "3DS-CHAT");
    draw_text(120, 28, 0.55f, CLR_TEXT_DIM, "Homebrew LAN/IP Chat");

    // Trennlinie
    draw_rect(0, 40, SCREEN_TOP_W, 1, CLR_ACCENT);

    // IP-Info
    draw_text(12, 70, 0.50f, CLR_TEXT_DIM, "Deine IP:");
    draw_textf(90, 70, 0.50f, CLR_GREEN, "%s", app.my_ip);

    draw_text(12, 95, 0.50f, CLR_TEXT_DIM, "Port:");
    draw_textf(90, 95, 0.50f, CLR_GREEN, "%d", CHAT_PORT);

    // Anleitung
    draw_rect(10, 115, 380, 100, CLR_PANEL);
    draw_rect(10, 115, 380, 1, CLR_ACCENT);
    draw_text(20, 135, 0.48f, CLR_TEXT, "HOST:  Warte auf eingehende Verbindung");
    draw_text(20, 155, 0.48f, CLR_TEXT, "JOIN:  Verbinde zu einem Host per IP");
    draw_text(20, 175, 0.48f, CLR_TEXT_DIM, "Beide muessen im gleichen WLAN sein");
    draw_text(20, 193, 0.48f, CLR_TEXT_DIM, "oder IP-Routing muss funktionieren.");

    // Puls-Animation
    float pulse = (sinf(app.frame * 0.05f) + 1.0f) * 0.5f;
    u32 pulse_clr = C2D_Color32(
        (u8)(0x0F + pulse * 0x10),
        (u8)(0x3C + pulse * 0x10),
        (u8)(0x78 + pulse * 0x10),
        0xFF);
    draw_rect(0, 235, SCREEN_TOP_W, 5, pulse_clr);

    // BOTTOM SCREEN
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_rect(0, 0, SCREEN_BOT_W, 30, CLR_PANEL);
    draw_text(10, 20, 0.55f, CLR_TEXT_DIM, "Was moechtest du tun?");

    draw_button(20, 50, 130, 50, "HOST", app.selected_btn == 1);
    draw_button(170, 50, 130, 50, "JOIN", app.selected_btn == 2);

    draw_text(20, 125, 0.48f, CLR_TEXT_DIM, "A = Auswahl  /  B = Passwort toggle");
    draw_textf(20, 145, 0.48f,
               app.use_password ? CLR_YELLOW : CLR_TEXT_DIM,
               "Passwort: %s", app.use_password ? "AN" : "AUS");
    if (app.use_password && strlen(app.password) > 0) {
        draw_text(20, 163, 0.45f, CLR_TEXT_DIM, "(Passwort gesetzt)");
    }
    draw_text(10, 195, 0.45f, CLR_TEXT_DIM, "START = Beenden");

    // D-Pad Hint
    draw_text(10, 215, 0.40f, CLR_TEXT_DIM, "< LINKS: HOST    RECHTS: JOIN >");
}

// ─── Screen: Warten (Host) ───────────────────────────────────────────────────
static void draw_host_wait(void) {
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 40, CLR_PANEL);
    draw_text(12, 28, 0.65f, CLR_ACCENT2, "3DS-CHAT / HOST");
    draw_rect(0, 40, SCREEN_TOP_W, 1, CLR_ACCENT);

    // Spinner
    static const char* spin[] = {"|", "/", "-", "\\"};
    draw_textf(12, 80, 0.70f, CLR_GREEN, "%s Warte auf Verbindung...",
               spin[(app.frame / 10) % 4]);

    draw_text(12, 115, 0.50f, CLR_TEXT, "Teile diese IP mit dem anderen Spieler:");
    draw_textf(12, 140, 0.70f, CLR_YELLOW, "%s", app.my_ip);
    draw_textf(12, 165, 0.50f, CLR_TEXT_DIM, "Port: %d", CHAT_PORT);

    if (app.use_password) {
        draw_text(12, 190, 0.50f, CLR_YELLOW, "Passwort-Schutz: AKTIV");
    }

    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_rect(0, 0, SCREEN_BOT_W, 30, CLR_PANEL);
    draw_text(10, 20, 0.50f, CLR_TEXT_DIM, "Host-Modus aktiv");
    draw_text(10, 60, 0.50f, CLR_TEXT, "Warte auf anderen Spieler...");
    draw_button(85, 100, 150, 40, "Abbrechen (B)", false);
}

// ─── Screen: Join ────────────────────────────────────────────────────────────
static void draw_join_screen(void) {
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 40, CLR_PANEL);
    draw_text(12, 28, 0.65f, CLR_ACCENT2, "3DS-CHAT / JOIN");
    draw_rect(0, 40, SCREEN_TOP_W, 1, CLR_ACCENT);
    draw_text(12, 80, 0.55f, CLR_TEXT, "Gib die IP-Adresse des Hosts ein.");
    draw_text(12, 105, 0.50f, CLR_TEXT_DIM, "Beide 3DS muessen verbunden sein.");

    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_rect(0, 0, SCREEN_BOT_W, 30, CLR_PANEL);
    draw_text(10, 20, 0.50f, CLR_TEXT_DIM, "Verbinden");
    draw_button(85, 50, 150, 40, "IP eingeben (A)", true);
    draw_button(85, 110, 150, 40, "Zurueck (B)", false);
}

// ─── Screen: Chat ─────────────────────────────────────────────────────────────
static void draw_chat(void) {
    // TOP: Nachrichten-Verlauf
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));

    draw_rect(0, 0, SCREEN_TOP_W, 22, CLR_PANEL);
    draw_textf(8, 15, 0.48f, CLR_ACCENT2, "3DS-Chat");
    draw_textf(80, 15, 0.45f, CLR_TEXT_DIM,
               app.connected ? "  Verbunden" : "  Getrennt");
    u32 status_clr = app.connected ? CLR_GREEN : CLR_RED;
    draw_rect(72, 7, 6, 6, status_clr);
    draw_rect(0, 22, SCREEN_TOP_W, 1, CLR_ACCENT);

    // Nachrichten zeichnen (von unten nach oben)
    int vis_msgs = (SCREEN_TOP_H - 23) / 22;  // ca. 9 Nachrichten
    int start    = app.msg_count - vis_msgs + app.scroll_offset;
    if (start < 0) start = 0;

    float y = 30;
    for (int i = start; i < app.msg_count && y < SCREEN_TOP_H - 5; i++) {
        Message* m = &app.messages[i];
        u32 bg_clr;
        switch (m->type) {
            case MSG_SELF:   bg_clr = CLR_SELF_MSG; break;
            case MSG_PEER:   bg_clr = CLR_PEER_MSG; break;
            default:         bg_clr = CLR_SYS_MSG;  break;
        }
        draw_rect(3, y - 1, SCREEN_TOP_W - 6, 19, bg_clr);
        if (m->type != MSG_SYSTEM) {
            draw_textf(7, y + 12, 0.40f, CLR_TEXT_DIM, "%s:", m->sender);
            draw_textf(7 + strlen(m->sender) * 5.5f + 8, y + 12, 0.45f, CLR_WHITE, "%s", m->text);
        } else {
            draw_textf(7, y + 12, 0.43f, CLR_GREEN, "** %s **", m->text);
        }
        y += 21;
    }

    // BOTTOM: Eingabe + Steuerung
    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_rect(0, 0, SCREEN_BOT_W, 22, CLR_PANEL);
    draw_textf(8, 15, 0.48f, CLR_TEXT_DIM, "Als: %s", app.username);
    draw_rect(0, 22, SCREEN_BOT_W, 1, CLR_ACCENT);

    // Eingabefeld
    draw_rect(5, 30, 310, 30, CLR_INPUT_BG);
    draw_rect(5, 30, 310, 1, CLR_ACCENT);
    draw_rect(5, 59, 310, 1, CLR_ACCENT);
    draw_rect(5, 30, 1, 30, CLR_ACCENT);
    draw_rect(314, 30, 1, 30, CLR_ACCENT);

    if (strlen(app.input_buf) > 0) {
        draw_textf(10, 50, 0.50f, CLR_WHITE, "%s", app.input_buf);
    } else {
        draw_text(10, 50, 0.48f, CLR_TEXT_DIM, "A = Schreiben...");
    }

    // Aktions-Buttons
    draw_button(5, 70, 140, 30, "Senden (A)", app.connected);
    draw_button(175, 70, 140, 30, "Trennen (B)", false);

    // Scrolling-Hinweis
    draw_text(5, 115, 0.42f, CLR_TEXT_DIM, "Oben/Unten = Scrollen");
    draw_text(5, 130, 0.42f, CLR_TEXT_DIM, "START = Hauptmenue");

    // Verbindungsstatus unten
    u32 bar_clr = app.connected ? CLR_GREEN : CLR_RED;
    draw_rect(0, SCREEN_BOT_H - 5, SCREEN_BOT_W, 5, bar_clr);
}

// ─── Screen: Fehler ──────────────────────────────────────────────────────────
static void draw_error(void) {
    C2D_TargetClear(C2D_GetTarget(GFX_TOP, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_TOP, GFX_LEFT));
    draw_rect(0, 0, SCREEN_TOP_W, 40, C2D_Color32(0x4A, 0x10, 0x10, 0xFF));
    draw_text(12, 28, 0.65f, CLR_RED, "FEHLER");
    draw_rect(0, 40, SCREEN_TOP_W, 1, CLR_RED);
    draw_textf(12, 80, 0.50f, CLR_WHITE, "%s", app.error_msg);
    draw_text(12, 130, 0.50f, CLR_TEXT_DIM, "B = Zurueck zum Menue");

    C2D_TargetClear(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT), CLR_BG);
    C2D_SceneBegin(C2D_GetTarget(GFX_BOTTOM, GFX_LEFT));
    draw_button(85, 100, 150, 40, "Zurueck (B)", true);
}

// ─── Input-Handler ────────────────────────────────────────────────────────────
static void handle_menu_input(u32 down, u32 held) {
    (void)held;
    if (down & KEY_DLEFT)  app.selected_btn = 1;
    if (down & KEY_DRIGHT) app.selected_btn = 2;

    if (down & KEY_B) {
        // Passwort Toggle
        app.use_password = !app.use_password;
        if (app.use_password && strlen(app.password) == 0) {
            open_keyboard("Chat-Passwort eingeben", app.password, PASS_MAX_LEN, true);
        }
    }

    if (down & KEY_A) {
        // Name eingeben falls noch leer
        if (strlen(app.username) == 0) {
            if (!open_keyboard("Dein Name / Nickname", app.username, NAME_MAX_LEN, false)) {
                strncpy(app.username, "Spieler", NAME_MAX_LEN);
            }
        }

        if (app.selected_btn == 1) {
            // HOST
            if (net_host()) {
                app.is_host = true;
                app.screen  = SCREEN_HOST;
                sys_msg("Host gestartet auf %s:%d", app.my_ip, CHAT_PORT);
            } else {
                snprintf(app.error_msg, sizeof(app.error_msg),
                         "Konnte Server nicht starten!\nPruefen ob WLAN aktiv.");
                app.screen = SCREEN_ERROR;
            }
        } else if (app.selected_btn == 2) {
            // JOIN
            app.screen = SCREEN_JOIN;
        }
    }
}

static void handle_host_input(u32 down, u32 held) {
    (void)held;
    if (down & KEY_B) {
        net_disconnect();
        app.screen = SCREEN_MENU;
    }
}

static void handle_join_input(u32 down, u32 held) {
    (void)held;
    if (down & KEY_B) {
        app.screen = SCREEN_MENU;
        return;
    }
    if (down & KEY_A) {
        char ip[IP_MAX_LEN] = {0};
        if (open_keyboard("Host-IP eingeben (z.B. 192.168.1.5)", ip, IP_MAX_LEN, false)) {
            char pass[PASS_MAX_LEN] = {0};
            if (app.use_password) {
                open_keyboard("Passwort des Hosts", pass, PASS_MAX_LEN, true);
                strncpy(app.password, pass, PASS_MAX_LEN);
            }
            strncpy(app.peer_ip, ip, IP_MAX_LEN);
            app.is_host = false;
            if (net_join(ip)) {
                sys_msg("Verbinde zu %s...", ip);
            } else {
                snprintf(app.error_msg, sizeof(app.error_msg),
                         "Verbindung zu %s fehlgeschlagen!\n"
                         "IP korrekt? Host gestartet?", ip);
                app.screen = SCREEN_ERROR;
            }
        }
    }
}

static void handle_chat_input(u32 down, u32 held) {
    (void)held;
    if (down & KEY_START) {
        net_disconnect();
        app.screen = SCREEN_MENU;
        return;
    }
    if (down & KEY_B) {
        if (app.connected) {
            net_disconnect();
            sys_msg("Verbindung getrennt.");
        }
        return;
    }
    if (down & KEY_DUP)   { if (app.scroll_offset > 0) app.scroll_offset--; }
    if (down & KEY_DDOWN) { app.scroll_offset++; }

    if (down & KEY_A && app.connected) {
        char msg[MSG_MAX_LEN] = {0};
        if (open_keyboard("Nachricht eingeben", msg, MSG_MAX_LEN, false)) {
            add_message(app.username, msg, MSG_SELF);
            if (send_chat_message(msg) < 0) {
                sys_msg("Sendefehler! Verbindung verloren?");
            }
        }
    }
}

static void handle_error_input(u32 down, u32 held) {
    (void)held;
    if (down & KEY_B) {
        net_disconnect();
        app.screen = SCREEN_MENU;
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(void) {
    // Hardware init
    gfxInitDefault();
    romfsInit();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // Render-Targets
    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    (void)top; (void)bottom;

    // SOC (Netzwerk)
    soc_buffer = (u32*)memalign(0x1000, SOC_BUFFER_SIZE);
    socInit(soc_buffer, SOC_BUFFER_SIZE);

    // App-State initialisieren
    memset(&app, 0, sizeof(app));
    app.screen       = SCREEN_MENU;
    app.server_sock  = -1;
    app.client_sock  = -1;
    app.selected_btn = 1;
    app.use_password = false;

    get_my_ip();
    sys_msg("3DS-Chat gestartet! IP: %s", app.my_ip);

    // Haupt-Loop
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();

        if (down & KEY_START && app.screen == SCREEN_MENU) break;

        // Netzwerk pollen
        if (app.screen == SCREEN_HOST || app.screen == SCREEN_CHAT) {
            poll_network();
        }

        // Input
        switch (app.screen) {
            case SCREEN_MENU:  handle_menu_input(down, held);  break;
            case SCREEN_HOST:  handle_host_input(down, held);  break;
            case SCREEN_JOIN:  handle_join_input(down, held);  break;
            case SCREEN_CHAT:  handle_chat_input(down, held);  break;
            case SCREEN_ERROR: handle_error_input(down, held); break;
        }

        // Zeichnen
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        switch (app.screen) {
            case SCREEN_MENU:  draw_menu();       break;
            case SCREEN_HOST:  draw_host_wait();  break;
            case SCREEN_JOIN:  draw_join_screen(); break;
            case SCREEN_CHAT:  draw_chat();        break;
            case SCREEN_ERROR: draw_error();       break;
        }
        C3D_FrameEnd(0);
        app.frame++;
    }

    // Aufräumen
    net_disconnect();
    socExit();
    if (soc_buffer) free(soc_buffer);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
