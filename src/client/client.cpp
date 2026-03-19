// client.cpp — Chat client for CC3064 Proyecto 1
// Compile after generating protos. See Makefile.

#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <csignal>

// Networking
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ifaddrs.h>

// Protobuf — client-side messages (types 1-7)
#include "register.pb.h"
#include "message_general.pb.h"
#include "message_dm.pb.h"
#include "change_status.pb.h"
#include "list_users.pb.h"
#include "get_user_info.pb.h"
#include "quit.pb.h"

// Protobuf — server-side messages (types 10-14)
#include "server_response.pb.h"
#include "all_users.pb.h"
#include "for_dm.pb.h"
#include "broadcast_messages.pb.h"
#include "get_user_info_response.pb.h"

// Protobuf — common
#include "common.pb.h"

// ─────────────────────────────────────────────
// Message type IDs (from protocol_standard.md)
// ─────────────────────────────────────────────
enum MsgType : uint8_t {
    // Client → Server
    TYPE_REGISTER       = 1,
    TYPE_MSG_GENERAL    = 2,
    TYPE_MSG_DM         = 3,
    TYPE_CHANGE_STATUS  = 4,
    TYPE_LIST_USERS     = 5,
    TYPE_GET_USER_INFO  = 6,
    TYPE_QUIT           = 7,
    // Server → Client
    TYPE_SERVER_RESPONSE        = 10,
    TYPE_ALL_USERS              = 11,
    TYPE_FOR_DM                 = 12,
    TYPE_BROADCAST_DELIVERY     = 13,
    TYPE_GET_USER_INFO_RESPONSE = 14,
};

// ─────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────
static int g_sockfd = -1;
static std::string g_username;
static std::string g_local_ip;
static std::atomic<bool> g_running{true};

// ─────────────────────────────────────────────
// Helper: read exactly n bytes
// ─────────────────────────────────────────────
static bool read_exact(int fd, void* buf, size_t n) {
    size_t total = 0;
    char* ptr = static_cast<char*>(buf);
    while (total < n) {
        ssize_t r = read(fd, ptr + total, n - total);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

// ─────────────────────────────────────────────
// Helper: send framed message (5-byte header + payload)
// ─────────────────────────────────────────────
static bool send_message(int fd, uint8_t type, const std::string& payload) {
    uint8_t header[5];
    header[0] = type;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(header + 1, &len, 4);

    if (write(fd, header, 5) != 5) return false;
    size_t total = 0;
    while (total < payload.size()) {
        ssize_t w = write(fd, payload.data() + total, payload.size() - total);
        if (w <= 0) return false;
        total += w;
    }
    return true;
}

// ─────────────────────────────────────────────
// Helper: get local IP of the machine
// ─────────────────────────────────────────────
static std::string get_local_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) return "0.0.0.0";

    std::string result = "0.0.0.0";
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

        std::string addr(ip);
        // Skip loopback
        if (addr != "127.0.0.1") {
            result = addr;
            break;
        }
    }
    freeifaddrs(ifaddr);
    return result;
}

// ─────────────────────────────────────────────
// Helper: StatusEnum ↔ string conversions
// ─────────────────────────────────────────────
static std::string status_to_string(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:         return "ACTIVE";
        case chat::DO_NOT_DISTURB: return "DO_NOT_DISTURB";
        case chat::INVISIBLE:      return "INVISIBLE";
        default:                   return "UNKNOWN";
    }
}

static bool string_to_status(const std::string& s, chat::StatusEnum& out) {
    if (s == "ACTIVE" || s == "0")         { out = chat::ACTIVE;         return true; }
    if (s == "DO_NOT_DISTURB" || s == "1") { out = chat::DO_NOT_DISTURB; return true; }
    if (s == "INVISIBLE" || s == "2")      { out = chat::INVISIBLE;      return true; }
    return false;
}

// ─────────────────────────────────────────────
// Print help
// ─────────────────────────────────────────────
static void print_help() {
    std::cout << "\n"
        << "╔══════════════════════════════════════════════════════╗\n"
        << "║                  CHAT COMMANDS                      ║\n"
        << "╠══════════════════════════════════════════════════════╣\n"
        << "║  \\broadcast <message>     Send to all users         ║\n"
        << "║  \\dm <user> <message>     Send direct message       ║\n"
        << "║  \\status <STATUS>         Change your status        ║\n"
        << "║         ACTIVE | DO_NOT_DISTURB | INVISIBLE         ║\n"
        << "║  \\users                   List connected users      ║\n"
        << "║  \\info <user>             Get user info             ║\n"
        << "║  \\help                    Show this help            ║\n"
        << "║  \\quit                    Exit chat                 ║\n"
        << "╚══════════════════════════════════════════════════════╝\n"
        << std::endl;
}

// ─────────────────────────────────────────────
// 4.4 Receiver thread: reads messages from
//     server and displays them
// ─────────────────────────────────────────────
static void receiver_thread() {
    while (g_running) {
        // Read 5-byte header
        uint8_t header[5];
        if (!read_exact(g_sockfd, header, 5)) {
            if (g_running) {
                std::cout << "\n[!] Disconnected from server." << std::endl;
                g_running = false;
            }
            return;
        }

        uint8_t type = header[0];
        uint32_t net_len;
        std::memcpy(&net_len, header + 1, 4);
        uint32_t length = ntohl(net_len);

        if (length > 10 * 1024 * 1024) {
            std::cerr << "[!] Server sent oversized payload." << std::endl;
            g_running = false;
            return;
        }

        // Read payload
        std::string payload(length, '\0');
        if (length > 0 && !read_exact(g_sockfd, &payload[0], length)) {
            if (g_running) {
                std::cout << "\n[!] Disconnected from server." << std::endl;
                g_running = false;
            }
            return;
        }

        // Dispatch by type
        switch (type) {

        case TYPE_SERVER_RESPONSE: {
            chat::ServerResponse resp;
            if (resp.ParseFromString(payload)) {
                if (resp.is_successful()) {
                    std::cout << "[Server] " << resp.message() << std::endl;
                } else {
                    std::cout << "[Server ERROR " << resp.status_code() << "] "
                              << resp.message() << std::endl;
                }
            }
            break;
        }

        case TYPE_ALL_USERS: {
            chat::AllUsers au;
            if (au.ParseFromString(payload)) {
                std::cout << "\n┌─────────────────────────────────┐" << std::endl;
                std::cout << "│       Connected Users           │" << std::endl;
                std::cout << "├──────────────────┬──────────────┤" << std::endl;
                for (int i = 0; i < au.usernames_size(); i++) {
                    std::string name = au.usernames(i);
                    std::string st = (i < au.status_size()) ? status_to_string(au.status(i)) : "?";
                    // Pad name and status for alignment
                    printf("│ %-16s │ %-12s │\n", name.c_str(), st.c_str());
                }
                std::cout << "└──────────────────┴──────────────┘" << std::endl;
            }
            break;
        }

        case TYPE_FOR_DM: {
            chat::ForDm dm;
            if (dm.ParseFromString(payload)) {
                std::cout << "[DM from " << dm.username_des() << "]: "
                          << dm.message() << std::endl;
            }
            break;
        }

        case TYPE_BROADCAST_DELIVERY: {
            chat::BroadcastDelivery bd;
            if (bd.ParseFromString(payload)) {
                std::cout << "[" << bd.username_origin() << "]: "
                          << bd.message() << std::endl;
            }
            break;
        }

        case TYPE_GET_USER_INFO_RESPONSE: {
            chat::GetUserInfoResponse info;
            if (info.ParseFromString(payload)) {
                std::cout << "\n┌─────────────────────────────────┐" << std::endl;
                std::cout << "│         User Info               │" << std::endl;
                std::cout << "├─────────────────────────────────┤" << std::endl;
                std::cout << "│  User:   " << info.username()   << std::endl;
                std::cout << "│  IP:     " << info.ip_address()  << std::endl;
                std::cout << "│  Status: " << status_to_string(info.status()) << std::endl;
                std::cout << "└─────────────────────────────────┘" << std::endl;
            }
            break;
        }

        default:
            std::cerr << "[!] Unknown message type from server: " << (int)type << std::endl;
            break;
        }

        // Reprint prompt
        std::cout << g_username << "> " << std::flush;
    }
}

// ─────────────────────────────────────────────
// 4.3 Command parser: reads user input and
//     sends the appropriate protobuf message
// ─────────────────────────────────────────────
static void process_input() {
    std::string line;

    while (g_running) {
        std::cout << g_username << "> " << std::flush;

        if (!std::getline(std::cin, line)) {
            // EOF (Ctrl+D)
            break;
        }

        if (line.empty()) continue;

        // ── \broadcast <message> ──
        if (line.rfind("\\broadcast ", 0) == 0) {
            std::string message = line.substr(11);
            if (message.empty()) {
                std::cout << "[!] Usage: \\broadcast <message>" << std::endl;
                continue;
            }

            chat::MessageGeneral mg;
            mg.set_message(message);
            mg.set_status(chat::ACTIVE);
            mg.set_username_origin(g_username);
            mg.set_ip(g_local_ip);

            std::string payload;
            mg.SerializeToString(&payload);
            if (!send_message(g_sockfd, TYPE_MSG_GENERAL, payload)) {
                std::cerr << "[!] Failed to send broadcast." << std::endl;
            }
        }
        // ── \dm <user> <message> ──
        else if (line.rfind("\\dm ", 0) == 0) {
            std::istringstream iss(line.substr(4));
            std::string dest;
            iss >> dest;

            // The rest of the line is the message
            std::string message;
            if (iss.peek() == ' ') iss.get(); // consume space
            std::getline(iss, message);

            if (dest.empty() || message.empty()) {
                std::cout << "[!] Usage: \\dm <user> <message>" << std::endl;
                continue;
            }

            chat::MessageDM dm;
            dm.set_message(message);
            dm.set_status(chat::ACTIVE);
            dm.set_username_des(dest);
            dm.set_ip(g_local_ip);

            std::string payload;
            dm.SerializeToString(&payload);
            if (!send_message(g_sockfd, TYPE_MSG_DM, payload)) {
                std::cerr << "[!] Failed to send DM." << std::endl;
            }
        }
        // ── \status <STATUS> ──
        else if (line.rfind("\\status ", 0) == 0) {
            std::string status_str = line.substr(8);
            chat::StatusEnum new_status;

            if (!string_to_status(status_str, new_status)) {
                std::cout << "[!] Invalid status. Use: ACTIVE, DO_NOT_DISTURB, INVISIBLE" << std::endl;
                continue;
            }

            chat::ChangeStatus cs;
            cs.set_status(new_status);
            cs.set_username(g_username);
            cs.set_ip(g_local_ip);

            std::string payload;
            cs.SerializeToString(&payload);
            if (!send_message(g_sockfd, TYPE_CHANGE_STATUS, payload)) {
                std::cerr << "[!] Failed to send status change." << std::endl;
            }
        }
        // ── \users ──
        else if (line == "\\users") {
            chat::ListUsers lu;
            lu.set_username(g_username);
            lu.set_ip(g_local_ip);

            std::string payload;
            lu.SerializeToString(&payload);
            if (!send_message(g_sockfd, TYPE_LIST_USERS, payload)) {
                std::cerr << "[!] Failed to request user list." << std::endl;
            }
        }
        // ── \info <user> ──
        else if (line.rfind("\\info ", 0) == 0) {
            std::string target = line.substr(6);
            if (target.empty()) {
                std::cout << "[!] Usage: \\info <user>" << std::endl;
                continue;
            }

            chat::GetUserInfo gui;
            gui.set_username_des(target);
            gui.set_username(g_username);
            gui.set_ip(g_local_ip);

            std::string payload;
            gui.SerializeToString(&payload);
            if (!send_message(g_sockfd, TYPE_GET_USER_INFO, payload)) {
                std::cerr << "[!] Failed to request user info." << std::endl;
            }
        }
        // ── \help ──
        else if (line == "\\help") {
            print_help();
        }
        // ── \quit ──
        else if (line == "\\quit") {
            chat::Quit q;
            q.set_quit(true);
            q.set_ip(g_local_ip);

            std::string payload;
            q.SerializeToString(&payload);
            send_message(g_sockfd, TYPE_QUIT, payload);

            std::cout << "Goodbye!" << std::endl;
            g_running = false;
            break;
        }
        // ── Unknown command ──
        else if (line[0] == '\\') {
            std::cout << "[!] Unknown command. Type \\help for available commands." << std::endl;
        }
        // ── Default: treat as broadcast (convenience) ──
        else {
            chat::MessageGeneral mg;
            mg.set_message(line);
            mg.set_status(chat::ACTIVE);
            mg.set_username_origin(g_username);
            mg.set_ip(g_local_ip);

            std::string payload;
            mg.SerializeToString(&payload);
            if (!send_message(g_sockfd, TYPE_MSG_GENERAL, payload)) {
                std::cerr << "[!] Failed to send message." << std::endl;
            }
        }
    }
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <server_ip> <port>" << std::endl;
        return 1;
    }

    g_username = argv[1];
    std::string server_ip = argv[2];
    int port = std::atoi(argv[3]);

    if (port <= 0 || port > 65535) {
        std::cerr << "Error: Invalid port number." << std::endl;
        return 1;
    }

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    std::signal(SIGPIPE, SIG_IGN);

    // Get local IP
    g_local_ip = get_local_ip();

    // ── 4.1 Create socket and connect ──
    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cerr << "Error: Invalid server IP address '" << server_ip << "'" << std::endl;
        close(g_sockfd);
        return 1;
    }

    std::cout << "Connecting to " << server_ip << ":" << port << "..." << std::endl;

    if (connect(g_sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(g_sockfd);
        return 1;
    }

    std::cout << "Connected! Registering as '" << g_username << "' (IP: " << g_local_ip << ")..." << std::endl;

    // ── Send Register (type 1) ──
    {
        chat::Register reg;
        reg.set_username(g_username);
        reg.set_ip(g_local_ip);

        std::string payload;
        reg.SerializeToString(&payload);
        if (!send_message(g_sockfd, TYPE_REGISTER, payload)) {
            std::cerr << "Error: Failed to send registration." << std::endl;
            close(g_sockfd);
            return 1;
        }
    }

    // ── Wait for ServerResponse (type 10) ──
    {
        uint8_t header[5];
        if (!read_exact(g_sockfd, header, 5)) {
            std::cerr << "Error: No response from server." << std::endl;
            close(g_sockfd);
            return 1;
        }

        uint8_t type = header[0];
        uint32_t net_len;
        std::memcpy(&net_len, header + 1, 4);
        uint32_t length = ntohl(net_len);

        std::string payload(length, '\0');
        if (length > 0 && !read_exact(g_sockfd, &payload[0], length)) {
            std::cerr << "Error: Incomplete response from server." << std::endl;
            close(g_sockfd);
            return 1;
        }

        if (type != TYPE_SERVER_RESPONSE) {
            std::cerr << "Error: Unexpected response type " << (int)type << std::endl;
            close(g_sockfd);
            return 1;
        }

        chat::ServerResponse resp;
        if (!resp.ParseFromString(payload)) {
            std::cerr << "Error: Malformed server response." << std::endl;
            close(g_sockfd);
            return 1;
        }

        if (!resp.is_successful()) {
            std::cerr << "Registration failed: " << resp.message() << std::endl;
            close(g_sockfd);
            return 1;
        }

        std::cout << "Registered successfully!" << std::endl;
    }

    // ── Welcome message ──
    std::cout << "\n"
        << "════════════════════════════════════════════\n"
        << "  Welcome, " << g_username << "!\n"
        << "  Type \\help for available commands.\n"
        << "  Type a message directly to broadcast.\n"
        << "════════════════════════════════════════════\n"
        << std::endl;

    // ── 4.2 Start receiver thread ──
    std::thread recv_t(receiver_thread);
    recv_t.detach();

    // ── Main thread: process user input ──
    process_input();

    // Cleanup
    close(g_sockfd);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
