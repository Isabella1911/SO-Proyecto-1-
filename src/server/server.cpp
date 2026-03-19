// server.cpp — Chat server for CC3064 Proyecto 1
// Compile after generating protos. See Makefile.

#include <iostream>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <csignal>
#include <atomic>

// Networking
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

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
// Constants
// ─────────────────────────────────────────────
static const int INACTIVITY_TIMEOUT_SECS = 60;  // Adjust for evaluation
static const int INACTIVITY_CHECK_INTERVAL = 10; // How often to check (secs)

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
    TYPE_SERVER_RESPONSE       = 10,
    TYPE_ALL_USERS             = 11,
    TYPE_FOR_DM                = 12,
    TYPE_BROADCAST_DELIVERY    = 13,
    TYPE_GET_USER_INFO_RESPONSE = 14,
};

// ─────────────────────────────────────────────
// Connected client info
// ─────────────────────────────────────────────
struct ClientInfo {
    std::string username;
    std::string ip;
    int         socket_fd;
    chat::StatusEnum status;
    std::chrono::steady_clock::time_point last_activity;
};

// ─────────────────────────────────────────────
// Shared state (protected by mutex)
// ─────────────────────────────────────────────
static std::map<std::string, ClientInfo> g_clients;   // key = username
static std::mutex g_mutex;
static std::atomic<bool> g_running{true};

// ─────────────────────────────────────────────
// Helper: read exactly n bytes from fd
// Returns false on disconnect / error
// ─────────────────────────────────────────────
static bool read_exact(int fd, void* buf, size_t n) {
    size_t total = 0;
    char* ptr = static_cast<char*>(buf);
    while (total < n) {
        ssize_t r = read(fd, ptr + total, n - total);
        if (r <= 0) return false;  // disconnected or error
        total += r;
    }
    return true;
}

// ─────────────────────────────────────────────
// Helper: send a framed message (5-byte header + payload)
// ─────────────────────────────────────────────
static bool send_message(int fd, uint8_t type, const std::string& payload) {
    uint8_t header[5];
    header[0] = type;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(header + 1, &len, 4);

    // Send header
    if (write(fd, header, 5) != 5) return false;
    // Send payload
    size_t total = 0;
    while (total < payload.size()) {
        ssize_t w = write(fd, payload.data() + total, payload.size() - total);
        if (w <= 0) return false;
        total += w;
    }
    return true;
}

// ─────────────────────────────────────────────
// Helper: build and send a ServerResponse
// ─────────────────────────────────────────────
static bool send_server_response(int fd, int32_t code, const std::string& msg, bool success) {
    chat::ServerResponse resp;
    resp.set_status_code(code);
    resp.set_message(msg);
    resp.set_is_successful(success);
    std::string payload;
    resp.SerializeToString(&payload);
    return send_message(fd, TYPE_SERVER_RESPONSE, payload);
}

// ─────────────────────────────────────────────
// Handle: TYPE_REGISTER (1)
// ─────────────────────────────────────────────
static void handle_register(int fd, const std::string& payload, const std::string& peer_ip) {
    chat::Register reg;
    if (!reg.ParseFromString(payload)) {
        send_server_response(fd, 400, "Malformed register message", false);
        return;
    }

    std::string username = reg.username();
    std::string ip = reg.ip().empty() ? peer_ip : reg.ip();

    if (username.empty()) {
        send_server_response(fd, 400, "Username cannot be empty", false);
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    // Check if username already exists
    if (g_clients.find(username) != g_clients.end()) {
        send_server_response(fd, 409, "Username '" + username + "' is already taken", false);
        return;
    }

    // Check if IP already connected
    for (auto& [name, info] : g_clients) {
        if (info.ip == ip) {
            send_server_response(fd, 409, "IP " + ip + " is already connected as '" + name + "'", false);
            return;
        }
    }

    // Register the client
    ClientInfo ci;
    ci.username = username;
    ci.ip = ip;
    ci.socket_fd = fd;
    ci.status = chat::ACTIVE;
    ci.last_activity = std::chrono::steady_clock::now();
    g_clients[username] = ci;

    std::cout << "[+] Registered user: " << username << " (" << ip << ") fd=" << fd << std::endl;
    send_server_response(fd, 200, "Registration successful", true);
}

// ─────────────────────────────────────────────
// Handle: TYPE_MSG_GENERAL (2) — Broadcast
// ─────────────────────────────────────────────
static void handle_message_general(int fd, const std::string& payload) {
    chat::MessageGeneral msg;
    if (!msg.ParseFromString(payload)) return;

    std::string origin = msg.username_origin();

    // Build broadcast delivery
    chat::BroadcastDelivery bd;
    bd.set_message(msg.message());
    bd.set_username_origin(origin);
    std::string out;
    bd.SerializeToString(&out);

    std::lock_guard<std::mutex> lock(g_mutex);

    // Update last_activity for the sender
    if (g_clients.count(origin)) {
        g_clients[origin].last_activity = std::chrono::steady_clock::now();
    }

    // Send to ALL connected clients (including sender, so they see the echo)
    for (auto& [name, info] : g_clients) {
        send_message(info.socket_fd, TYPE_BROADCAST_DELIVERY, out);
    }

    std::cout << "[broadcast] " << origin << ": " << msg.message() << std::endl;
}

// ─────────────────────────────────────────────
// Handle: TYPE_MSG_DM (3) — Direct message
// ─────────────────────────────────────────────
static void handle_message_dm(int fd, const std::string& payload) {
    chat::MessageDM msg;
    if (!msg.ParseFromString(payload)) return;

    std::string dest = msg.username_des();

    std::lock_guard<std::mutex> lock(g_mutex);

    // Find sender by fd and update activity
    std::string sender;
    for (auto& [name, info] : g_clients) {
        if (info.socket_fd == fd) {
            sender = name;
            info.last_activity = std::chrono::steady_clock::now();
            break;
        }
    }

    // Find destination
    auto it = g_clients.find(dest);
    if (it == g_clients.end()) {
        send_server_response(fd, 404, "User '" + dest + "' not found", false);
        return;
    }

    // Build ForDm
    chat::ForDm dm;
    dm.set_username_des(sender);  // who sent it
    dm.set_message(msg.message());
    std::string out;
    dm.SerializeToString(&out);

    send_message(it->second.socket_fd, TYPE_FOR_DM, out);
    std::cout << "[dm] " << sender << " -> " << dest << ": " << msg.message() << std::endl;
}

// ─────────────────────────────────────────────
// Handle: TYPE_CHANGE_STATUS (4)
// ─────────────────────────────────────────────
static void handle_change_status(int fd, const std::string& payload) {
    chat::ChangeStatus cs;
    if (!cs.ParseFromString(payload)) return;

    std::string username = cs.username();
    chat::StatusEnum new_status = cs.status();

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_clients.find(username);
    if (it == g_clients.end()) {
        send_server_response(fd, 404, "User not found", false);
        return;
    }

    it->second.status = new_status;
    it->second.last_activity = std::chrono::steady_clock::now();

    const char* status_names[] = {"ACTIVE", "DO_NOT_DISTURB", "INVISIBLE"};
    std::string sname = (new_status >= 0 && new_status <= 2) ? status_names[new_status] : "UNKNOWN";

    std::cout << "[status] " << username << " -> " << sname << std::endl;
    send_server_response(fd, 200, "Status changed to " + sname, true);
}

// ─────────────────────────────────────────────
// Handle: TYPE_LIST_USERS (5)
// ─────────────────────────────────────────────
static void handle_list_users(int fd, const std::string& payload) {
    chat::ListUsers lu;
    if (!lu.ParseFromString(payload)) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    // Update activity for requester
    std::string requester = lu.username();
    if (g_clients.count(requester)) {
        g_clients[requester].last_activity = std::chrono::steady_clock::now();
    }

    chat::AllUsers au;
    for (auto& [name, info] : g_clients) {
        au.add_usernames(name);
        au.add_status(info.status);
    }

    std::string out;
    au.SerializeToString(&out);
    send_message(fd, TYPE_ALL_USERS, out);
    std::cout << "[list_users] Sent to " << requester << " (" << g_clients.size() << " users)" << std::endl;
}

// ─────────────────────────────────────────────
// Handle: TYPE_GET_USER_INFO (6)
// ─────────────────────────────────────────────
static void handle_get_user_info(int fd, const std::string& payload) {
    chat::GetUserInfo gui;
    if (!gui.ParseFromString(payload)) return;

    std::string target = gui.username_des();
    std::string requester = gui.username();

    std::lock_guard<std::mutex> lock(g_mutex);

    // Update activity for requester
    if (g_clients.count(requester)) {
        g_clients[requester].last_activity = std::chrono::steady_clock::now();
    }

    auto it = g_clients.find(target);
    if (it == g_clients.end()) {
        send_server_response(fd, 404, "User '" + target + "' not found or not connected", false);
        return;
    }

    chat::GetUserInfoResponse resp;
    resp.set_ip_address(it->second.ip);
    resp.set_username(it->second.username);
    resp.set_status(it->second.status);

    std::string out;
    resp.SerializeToString(&out);
    send_message(fd, TYPE_GET_USER_INFO_RESPONSE, out);
    std::cout << "[info] " << requester << " queried info for " << target << std::endl;
}

// ─────────────────────────────────────────────
// Handle: TYPE_QUIT (7)
// ─────────────────────────────────────────────
static void handle_quit(int fd, const std::string& payload) {
    chat::Quit q;
    if (!q.ParseFromString(payload)) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    // Find user by fd and remove
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (it->second.socket_fd == fd) {
            std::cout << "[-] User quit: " << it->first << std::endl;
            g_clients.erase(it);
            break;
        }
    }
}

// ─────────────────────────────────────────────
// Remove a client by fd (disconnect detection)
// ─────────────────────────────────────────────
static void remove_client_by_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
        if (it->second.socket_fd == fd) {
            std::cout << "[-] Disconnected: " << it->first << std::endl;
            g_clients.erase(it);
            return;
        }
    }
}

// ─────────────────────────────────────────────
// Client thread: read loop
// ─────────────────────────────────────────────
static void client_thread(int fd, std::string peer_ip) {
    std::cout << "[thread] New connection from " << peer_ip << " fd=" << fd << std::endl;

    while (g_running) {
        // 1. Read 5-byte header
        uint8_t header[5];
        if (!read_exact(fd, header, 5)) {
            // Client disconnected
            break;
        }

        uint8_t type = header[0];
        uint32_t net_len;
        std::memcpy(&net_len, header + 1, 4);
        uint32_t length = ntohl(net_len);

        // Safety check
        if (length > 10 * 1024 * 1024) {  // 10 MB max
            std::cerr << "[!] Payload too large from fd=" << fd << ": " << length << std::endl;
            break;
        }

        // 2. Read payload
        std::string payload(length, '\0');
        if (length > 0 && !read_exact(fd, &payload[0], length)) {
            break;
        }

        // 3. Dispatch based on type
        switch (type) {
            case TYPE_REGISTER:
                handle_register(fd, payload, peer_ip);
                break;
            case TYPE_MSG_GENERAL:
                handle_message_general(fd, payload);
                break;
            case TYPE_MSG_DM:
                handle_message_dm(fd, payload);
                break;
            case TYPE_CHANGE_STATUS:
                handle_change_status(fd, payload);
                break;
            case TYPE_LIST_USERS:
                handle_list_users(fd, payload);
                break;
            case TYPE_GET_USER_INFO:
                handle_get_user_info(fd, payload);
                break;
            case TYPE_QUIT:
                handle_quit(fd, payload);
                close(fd);
                std::cout << "[thread] Closed fd=" << fd << std::endl;
                return;  // exit thread
            default:
                std::cerr << "[!] Unknown message type " << (int)type << " from fd=" << fd << std::endl;
                send_server_response(fd, 400, "Unknown message type", false);
                break;
        }
    }

    // If we fell out of the loop, client disconnected unexpectedly
    remove_client_by_fd(fd);
    close(fd);
    std::cout << "[thread] Closed fd=" << fd << std::endl;
}

// ─────────────────────────────────────────────
// Inactivity checker thread
// ─────────────────────────────────────────────
static void inactivity_thread() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(INACTIVITY_CHECK_INTERVAL));

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto& [name, info] : g_clients) {
            if (info.status == chat::INVISIBLE) continue;  // already inactive

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - info.last_activity).count();
            if (elapsed >= INACTIVITY_TIMEOUT_SECS) {
                info.status = chat::INVISIBLE;
                std::cout << "[inactivity] " << name << " -> INVISIBLE (inactive " << elapsed << "s)" << std::endl;

                // Optionally notify the client
                send_server_response(info.socket_fd, 200,
                    "Your status has been changed to INVISIBLE due to inactivity", true);
            }
        }
    }
}

// ─────────────────────────────────────────────
// Signal handler for clean shutdown
// ─────────────────────────────────────────────
static void signal_handler(int) {
    g_running = false;
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }

    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Error: Invalid port number." << std::endl;
        return 1;
    }

    // Verify protobuf library version
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Set up signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe

    // 1. Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Bind
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // 3. Listen
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  Chat Server running on port " << port << std::endl;
    std::cout << "  Inactivity timeout: " << INACTIVITY_TIMEOUT_SECS << "s" << std::endl;
    std::cout << "========================================" << std::endl;

    // 4. Start inactivity checker thread
    std::thread inactivity(inactivity_thread);
    inactivity.detach();

    // 5. Accept loop
    while (g_running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!g_running) break;  // shutting down
            perror("accept");
            continue;
        }

        // Get peer IP as string
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string peer_ip(ip_str);

        // Spawn a thread for this client
        std::thread t(client_thread, client_fd, peer_ip);
        t.detach();
    }

    // Cleanup
    close(server_fd);
    google::protobuf::ShutdownProtobufLibrary();
    std::cout << "\nServer shut down." << std::endl;
    return 0;
}
