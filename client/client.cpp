/**
 * cliente.cpp  —  Chat client
 *
 * Usage:  ./cliente <username> <server_ip> <server_port>
 *
 * Depends on: protobuf, ncurses, pthread
 * Compile:
 *   protoc -I ../protos --cpp_out=. ../protos/common.proto \
 *          ../protos/cliente-side/*.proto ../protos/server-side/*.proto
 *   g++ -std=c++17 client.cpp *.pb.cc -o cliente \
 *       $(pkg-config --cflags --libs protobuf) -lncurses -lpthread
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ncurses.h>

// ── protobuf headers ──────────────────────────────────────────────────────────
#include "common.pb.h"
#include "cliente-side/register.pb.h"
#include "cliente-side/message_general.pb.h"
#include "cliente-side/message_dm.pb.h"
#include "cliente-side/change_status.pb.h"
#include "cliente-side/list_users.pb.h"
#include "cliente-side/get_user_info.pb.h"
#include "cliente-side/quit.pb.h"

#include "server-side/server_response.pb.h"
#include "server-side/all_users.pb.h"
#include "server-side/for_dm.pb.h"
#include "server-side/broadcast_messages.pb.h"
#include "server-side/get_user_info_response.pb.h"

// ── message type constants (from protocol_standard.md) ────────────────────────
enum MsgType : uint8_t {
    TYPE_REGISTER        = 1,
    TYPE_MSG_GENERAL     = 2,
    TYPE_MSG_DM          = 3,
    TYPE_CHANGE_STATUS   = 4,
    TYPE_LIST_USERS      = 5,
    TYPE_GET_USER_INFO   = 6,
    TYPE_QUIT            = 7,
    TYPE_SERVER_RESPONSE = 10,
    TYPE_ALL_USERS       = 11,
    TYPE_FOR_DM          = 12,
    TYPE_BROADCAST       = 13,
    TYPE_USER_INFO_RESP  = 14,
};

// ── globals ───────────────────────────────────────────────────────────────────
static int              g_sock      = -1;
static std::string      g_username;
static std::string      g_myip;
static std::atomic<bool> g_running  {true};

static chat::StatusEnum g_myStatus  = chat::ACTIVE;
static std::mutex       g_statusMtx;

// inactivity timer
static std::chrono::steady_clock::time_point g_lastActivity;
static std::mutex       g_actMtx;
static const int        INACTIVITY_SECONDS = 30;   // adjustable

// ── ncurses windows ───────────────────────────────────────────────────────────
static WINDOW *w_chat  = nullptr;   // scrolling message area
static WINDOW *w_input = nullptr;   // single-line input
static WINDOW *w_status= nullptr;   // status bar (top)
static std::mutex g_ncMtx;          // guards all ncurses calls

static std::vector<std::string> g_chatLines;
static const int MAX_LINES = 500;

// ─────────────────────────────────────────────────────────────────────────────
//  Low-level TCP framing
// ─────────────────────────────────────────────────────────────────────────────

static bool send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool recv_all(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

// Send a framed protobuf message: [type(1)] [length(4 BE)] [payload]
static bool send_proto(int fd, uint8_t type, const google::protobuf::MessageLite &msg) {
    std::string payload;
    if (!msg.SerializeToString(&payload)) return false;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    uint8_t header[5];
    header[0] = type;
    std::memcpy(header + 1, &len, 4);
    if (!send_all(fd, header, 5)) return false;
    return send_all(fd, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

// Read a framed message; returns false on disconnect / error
static bool recv_frame(int fd, uint8_t &type, std::string &payload) {
    uint8_t header[5];
    if (!recv_all(fd, header, 5)) return false;
    type = header[0];
    uint32_t nlen;
    std::memcpy(&nlen, header + 1, 4);
    uint32_t len = ntohl(nlen);
    payload.resize(len);
    return recv_all(fd, reinterpret_cast<uint8_t*>(payload.data()), len);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string statusStr(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:         return "ACTIVO";
        case chat::DO_NOT_DISTURB: return "OCUPADO";
        case chat::INVISIBLE:      return "INACTIVO";
        default:                   return "?";
    }
}

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[10];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

static void redraw_status() {
    std::lock_guard<std::mutex> lk(g_ncMtx);
    int cols = getmaxx(w_status);
    werase(w_status);
    wattron(w_status, A_REVERSE);
    std::string stat;
    {
        std::lock_guard<std::mutex> ls(g_statusMtx);
        stat = statusStr(g_myStatus);
    }
    std::string txt = " Chat | " + g_username + " [" + stat + "]  —  IP: " + g_myip
                    + "  | /ayuda para comandos";
    if ((int)txt.size() < cols) txt += std::string(cols - txt.size(), ' ');
    mvwprintw(w_status, 0, 0, "%.*s", cols, txt.c_str());
    wattroff(w_status, A_REVERSE);
    wrefresh(w_status);
}

static void push_line(const std::string &line) {
    std::lock_guard<std::mutex> lk(g_ncMtx);
    g_chatLines.push_back(line);
    if ((int)g_chatLines.size() > MAX_LINES)
        g_chatLines.erase(g_chatLines.begin());

    int rows = getmaxy(w_chat);
    int cols = getmaxx(w_chat);
    werase(w_chat);
    int start = std::max(0, (int)g_chatLines.size() - rows);
    for (int i = start; i < (int)g_chatLines.size(); i++) {
        const std::string &l = g_chatLines[i];
        mvwprintw(w_chat, i - start, 0, "%.*s", cols, l.c_str());
    }
    wrefresh(w_chat);
}

static void init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    w_status = newwin(1, cols, 0, 0);
    w_chat   = newwin(rows - 2, cols, 1, 0);
    w_input  = newwin(1, cols, rows - 1, 0);

    scrollok(w_chat, TRUE);
    idlok(w_chat, TRUE);
    keypad(w_input, TRUE);

    wrefresh(w_chat);
    wrefresh(w_input);
}

static void cleanup_ncurses() {
    if (w_chat)   delwin(w_chat);
    if (w_input)  delwin(w_input);
    if (w_status) delwin(w_status);
    endwin();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Receive thread
// ─────────────────────────────────────────────────────────────────────────────

static void recv_thread() {
    while (g_running.load()) {
        uint8_t type;
        std::string payload;
        if (!recv_frame(g_sock, type, payload)) {
            if (g_running.load())
                push_line("[!] Conexión con el servidor perdida.");
            g_running = false;
            return;
        }

        switch (type) {

        case TYPE_SERVER_RESPONSE: {
            chat::ServerResponse resp;
            if (!resp.ParseFromString(payload)) break;
            std::string pfx = resp.is_successful() ? "[OK] " : "[ERR] ";
            push_line(pfx + resp.message());
            redraw_status();
            break;
        }

        case TYPE_BROADCAST: {
            chat::BroadcastDelivery brd;
            if (!brd.ParseFromString(payload)) break;
            push_line("[" + timestamp() + "] <" + brd.username_origin() + "> " + brd.message());
            break;
        }

        case TYPE_FOR_DM: {
            chat::ForDm dm;
            if (!dm.ParseFromString(payload)) break;
            push_line("[" + timestamp() + "] [DM de " + dm.username_des() + "] " + dm.message());
            break;
        }

        case TYPE_ALL_USERS: {
            chat::AllUsers au;
            if (!au.ParseFromString(payload)) break;
            push_line("── Usuarios conectados ──────────────────");
            for (int i = 0; i < au.usernames_size(); i++) {
                std::string line = "  • " + au.usernames(i);
                if (i < au.status_size())
                    line += "  [" + statusStr(au.status(i)) + "]";
                push_line(line);
            }
            push_line("─────────────────────────────────────────");
            break;
        }

        case TYPE_USER_INFO_RESP: {
            chat::GetUserInfoResponse info;
            if (!info.ParseFromString(payload)) break;
            push_line("── Info usuario ─────────────────────────");
            push_line("  Usuario : " + info.username());
            push_line("  IP      : " + info.ip_address());
            push_line("  Status  : " + statusStr(info.status()));
            push_line("─────────────────────────────────────────");
            break;
        }

        default:
            push_line("[?] Tipo de mensaje desconocido: " + std::to_string(type));
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inactivity watchdog
// ─────────────────────────────────────────────────────────────────────────────

static void inactivity_thread() {
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!g_running.load()) break;

        auto now = std::chrono::steady_clock::now();
        double secs;
        {
            std::lock_guard<std::mutex> lk(g_actMtx);
            secs = std::chrono::duration<double>(now - g_lastActivity).count();
        }

        chat::StatusEnum cur;
        {
            std::lock_guard<std::mutex> lk(g_statusMtx);
            cur = g_myStatus;
        }

        if (secs >= INACTIVITY_SECONDS && cur != chat::INVISIBLE) {
            // request INVISIBLE (INACTIVO) from server
            chat::ChangeStatus cs;
            cs.set_status(chat::INVISIBLE);
            cs.set_username(g_username);
            cs.set_ip(g_myip);
            send_proto(g_sock, TYPE_CHANGE_STATUS, cs);
            {
                std::lock_guard<std::mutex> lk(g_statusMtx);
                g_myStatus = chat::INVISIBLE;
            }
            push_line("[*] Status cambiado automáticamente a INACTIVO por inactividad.");
            redraw_status();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Command handling
// ─────────────────────────────────────────────────────────────────────────────

static void reset_activity() {
    std::lock_guard<std::mutex> lk(g_actMtx);
    g_lastActivity = std::chrono::steady_clock::now();
    // if INACTIVO restore to ACTIVO
    {
        std::lock_guard<std::mutex> ls(g_statusMtx);
        if (g_myStatus == chat::INVISIBLE) {
            g_myStatus = chat::ACTIVE;
            chat::ChangeStatus cs;
            cs.set_status(chat::ACTIVE);
            cs.set_username(g_username);
            cs.set_ip(g_myip);
            send_proto(g_sock, TYPE_CHANGE_STATUS, cs);
        }
    }
    redraw_status();
}

static void show_help() {
    push_line("─── Comandos disponibles ────────────────────────────────────────");
    push_line("  <mensaje>                   → Broadcast al chat general");
    push_line("  /dm <usuario> <mensaje>     → Mensaje directo privado");
    push_line("  /status <activo|ocupado|inactivo>  → Cambiar status");
    push_line("  /usuarios                   → Listar usuarios conectados");
    push_line("  /info <usuario>             → Info de un usuario");
    push_line("  /ayuda                      → Mostrar esta ayuda");
    push_line("  /salir                      → Salir del chat");
    push_line("─────────────────────────────────────────────────────────────────");
}

static void handle_command(const std::string &raw) {
    if (raw.empty()) return;
    reset_activity();

    // /salir
    if (raw == "/salir") {
        chat::Quit q;
        q.set_quit(true);
        q.set_ip(g_myip);
        send_proto(g_sock, TYPE_QUIT, q);
        g_running = false;
        return;
    }

    // /ayuda
    if (raw == "/ayuda") { show_help(); return; }

    // /usuarios
    if (raw == "/usuarios") {
        chat::ListUsers lu;
        lu.set_username(g_username);
        lu.set_ip(g_myip);
        send_proto(g_sock, TYPE_LIST_USERS, lu);
        return;
    }

    // /info <usuario>
    if (raw.rfind("/info ", 0) == 0) {
        std::string target = raw.substr(6);
        if (target.empty()) { push_line("[!] Uso: /info <usuario>"); return; }
        chat::GetUserInfo gu;
        gu.set_username_des(target);
        gu.set_username(g_username);
        gu.set_ip(g_myip);
        send_proto(g_sock, TYPE_GET_USER_INFO, gu);
        return;
    }

    // /status <activo|ocupado|inactivo>
    if (raw.rfind("/status ", 0) == 0) {
        std::string arg = raw.substr(8);
        chat::StatusEnum ns;
        if (arg == "activo")   ns = chat::ACTIVE;
        else if (arg == "ocupado")  ns = chat::DO_NOT_DISTURB;
        else if (arg == "inactivo") ns = chat::INVISIBLE;
        else { push_line("[!] Status válidos: activo, ocupado, inactivo"); return; }
        chat::ChangeStatus cs;
        cs.set_status(ns);
        cs.set_username(g_username);
        cs.set_ip(g_myip);
        send_proto(g_sock, TYPE_CHANGE_STATUS, cs);
        {
            std::lock_guard<std::mutex> lk(g_statusMtx);
            g_myStatus = ns;
        }
        redraw_status();
        return;
    }

    // /dm <usuario> <mensaje>
    if (raw.rfind("/dm ", 0) == 0) {
        std::string rest = raw.substr(4);
        auto sp = rest.find(' ');
        if (sp == std::string::npos) { push_line("[!] Uso: /dm <usuario> <mensaje>"); return; }
        std::string dest = rest.substr(0, sp);
        std::string msg  = rest.substr(sp + 1);
        if (msg.empty()) { push_line("[!] Mensaje vacío."); return; }
        chat::MessageDM dm;
        dm.set_message(msg);
        dm.set_status(g_myStatus);
        dm.set_username_des(dest);
        dm.set_ip(g_myip);
        send_proto(g_sock, TYPE_MSG_DM, dm);
        push_line("[" + timestamp() + "] [DM → " + dest + "] " + msg);
        return;
    }

    // anything else → broadcast
    chat::MessageGeneral mg;
    mg.set_message(raw);
    mg.set_status(g_myStatus);
    mg.set_username_origin(g_username);
    mg.set_ip(g_myip);
    send_proto(g_sock, TYPE_MSG_GENERAL, mg);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Input loop (runs in main thread)
// ─────────────────────────────────────────────────────────────────────────────

static void input_loop() {
    std::string buf;
    const int PROMPT_LEN = 2; // "> "

    auto redraw_input = [&]() {
        std::lock_guard<std::mutex> lk(g_ncMtx);
        int cols = getmaxx(w_input);
        werase(w_input);
        std::string display = "> " + buf;
        mvwprintw(w_input, 0, 0, "%.*s", cols, display.c_str());
        // place cursor
        int cx = std::min((int)display.size(), cols - 1);
        wmove(w_input, 0, cx);
        wrefresh(w_input);
    };

    redraw_input();

    while (g_running.load()) {
        int ch;
        {
            std::lock_guard<std::mutex> lk(g_ncMtx);
            wtimeout(w_input, 200);
            ch = wgetch(w_input);
        }

        if (ch == ERR) continue; // timeout, check g_running

        if (ch == '\n' || ch == KEY_ENTER) {
            if (!buf.empty()) {
                handle_command(buf);
                buf.clear();
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (!buf.empty()) buf.pop_back();
        } else if (ch >= 32 && ch < 256) {
            buf += static_cast<char>(ch);
        }
        redraw_input();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <usuario> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    g_username        = argv[1];
    std::string srv_ip = argv[2];
    int         port   = std::stoi(argv[3]);

    // ── connect ───────────────────────────────────────────────────────────────
    g_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); return 1; }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(port);
    if (inet_pton(AF_INET, srv_ip.c_str(), &srv.sin_addr) <= 0) {
        fprintf(stderr, "IP inválida: %s\n", srv_ip.c_str());
        return 1;
    }

    if (::connect(g_sock, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }

    // obtain our local IP
    sockaddr_in local{};
    socklen_t llen = sizeof(local);
    getsockname(g_sock, reinterpret_cast<sockaddr*>(&local), &llen);
    char ipbuf[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, &local.sin_addr, ipbuf, sizeof(ipbuf));
    g_myip = ipbuf;

    // ── register ─────────────────────────────────────────────────────────────
    {
        chat::Register reg;
        reg.set_username(g_username);
        reg.set_ip(g_myip);
        if (!send_proto(g_sock, TYPE_REGISTER, reg)) {
            fprintf(stderr, "Error enviando registro\n");
            close(g_sock);
            return 1;
        }
    }

    // ── init ncurses ─────────────────────────────────────────────────────────
    init_ncurses();
    redraw_status();
    push_line("Conectado como " + g_username + " desde " + g_myip);
    push_line("Escribe /ayuda para ver los comandos disponibles.");

    // ── start inactivity timer ────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(g_actMtx);
        g_lastActivity = std::chrono::steady_clock::now();
    }

    // ── launch threads ────────────────────────────────────────────────────────
    std::thread tRecv(recv_thread);
    std::thread tInact(inactivity_thread);

    // ── main thread: input loop ───────────────────────────────────────────────
    input_loop();

    // ── cleanup ───────────────────────────────────────────────────────────────
    g_running = false;
    ::shutdown(g_sock, SHUT_RDWR);
    ::close(g_sock);

    tRecv.join();
    tInact.join();

    cleanup_ncurses();
    printf("Desconectado. ¡Hasta luego!\n");
    return 0;
}