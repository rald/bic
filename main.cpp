// BIC - BIC IRC Client (non‑blocking connect, full debug logs)
// Compile: g++ -std=c++11 -o bic bic.cpp -lfltk -lpthread -lX11
// Windows:  g++ -std=c++11 -o bic.exe bic.cpp -lfltk -lws2_32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <string>
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/fl_ask.H>

// --------------------------------------------------------------
// Platform-specific socket headers and helpers
// --------------------------------------------------------------
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <io.h>
    typedef int socklen_t;
    #define CLOSE_SOCKET(s) closesocket(s)
    #define GET_ERROR() WSAGetLastError()
    #define IS_VALID_SOCKET(s) ((s) != INVALID_SOCKET)
    #define SOCKET_ERROR_VAL INVALID_SOCKET
    static const char* socket_strerror(int err) {
        static char buf[256];
        snprintf(buf, sizeof(buf), "Winsock error %d", err);
        return buf;
    }
    static void set_nonblocking(int fd) {
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #define CLOSE_SOCKET(s) close(s)
    #define GET_ERROR() errno
    #define IS_VALID_SOCKET(s) ((s) >= 0)
    #define SOCKET_ERROR_VAL (-1)
    static const char* socket_strerror(int err) {
        return strerror(err);
    }
    static void set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif

// --------------------------------------------------------------
// IRC client state
// --------------------------------------------------------------
struct IRCClient {
    int sockfd;
    Fl_Text_Buffer *logbuf;
    Fl_Text_Display *logdisp;
    Fl_Input *msg_input;
    char current_channel[64];
    char nickname[32];
    char default_target[64];
    std::vector<std::string> history;
    int history_pos;
    std::string saved_input;
    bool connect_attempt;
};

static IRCClient irc;

// --------------------------------------------------------------
// Custom input widget with history and PgUp/PgDn
// --------------------------------------------------------------
class HistoryInput : public Fl_Input {
private:
    void recall_older() {
        if (irc.history.empty()) return;
        if (irc.history_pos == -1) {
            irc.saved_input = value();
            irc.history_pos = (int)irc.history.size() - 1;
        } else if (irc.history_pos > 0) {
            irc.history_pos--;
        } else {
            return;
        }
        value(irc.history[irc.history_pos].c_str());
        position(strlen(value()));
        redraw();
    }

    void recall_newer() {
        if (irc.history.empty()) return;
        if (irc.history_pos == -1) return;
        if (irc.history_pos == (int)irc.history.size() - 1) {
            irc.history_pos = -1;
            value(irc.saved_input.c_str());
        } else {
            irc.history_pos++;
            value(irc.history[irc.history_pos].c_str());
        }
        position(strlen(value()));
        redraw();
    }

public:
    HistoryInput(int x, int y, int w, int h, const char *l = 0)
        : Fl_Input(x, y, w, h, l) {}

    int handle(int event) override {
        if (event == FL_KEYBOARD) {
            int key = Fl::event_key();
            if (key == FL_Up) {
                recall_older();
                return 1;
            }
            if (key == FL_Down) {
                recall_newer();
                return 1;
            }
#if FLTK_MAJOR_VERSION > 1 || (FLTK_MAJOR_VERSION == 1 && FLTK_MINOR_VERSION >= 3)
            if (key == FL_Page_Up || key == FL_Page_Down) {
                Fl_Scrollbar *sb = irc.logdisp->scrollbar();
                if (sb) {
                    if (key == FL_Page_Up)
                        sb->value(sb->value() - sb->linesize());
                    else
                        sb->value(sb->value() + sb->linesize());
                    irc.logdisp->redraw();
                }
                return 1;
            }
#endif
        }
        return Fl_Input::handle(event);
    }
};

// --------------------------------------------------------------
// Timestamp (12‑hour format)
// --------------------------------------------------------------
const char* get_timestamp() {
    static char time_str[20];
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%I:%M:%S %p", timeinfo);
    if (time_str[0] == '0') {
        for (int i = 0; time_str[i]; i++) time_str[i] = time_str[i+1];
    }
    return time_str;
}

// --------------------------------------------------------------
// Auto‑scroll detection (FLTK 1.3+)
// --------------------------------------------------------------
bool at_bottom() {
#if FLTK_MAJOR_VERSION > 1 || (FLTK_MAJOR_VERSION == 1 && FLTK_MINOR_VERSION >= 3)
    Fl_Scrollbar *sb = irc.logdisp->scrollbar();
    if (!sb) return true;
    return (sb->value() + sb->linesize() >= sb->maximum() - 0.5);
#else
    return true;
#endif
}

// --------------------------------------------------------------
// Append log with timestamp and smart scroll
// --------------------------------------------------------------
void append_log(const char *fmt, ...) {
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    bool was_at_bottom = at_bottom();
    char final[1100];
    snprintf(final, sizeof(final), "<%s> %s", get_timestamp(), buffer);
    irc.logbuf->append(final);
    irc.logbuf->append("\n");
    if (was_at_bottom) {
        irc.logdisp->insert_position(irc.logbuf->length());
        irc.logdisp->show_insert_position();
    }
}

// --------------------------------------------------------------
// Send raw IRC message (only after fully connected)
// --------------------------------------------------------------
void send_raw(const char *fmt, ...) {
    if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) return;
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    strcat(buffer, "\r\n");
#ifdef _WIN32
    send(irc.sockfd, buffer, (int)strlen(buffer), 0);
#else
    send(irc.sockfd, buffer, strlen(buffer), 0);
#endif
}

// --------------------------------------------------------------
// IRC commands
// --------------------------------------------------------------
void join_channel(const char *channel) {
    if (!channel || !channel[0]) return;
    send_raw("JOIN %s", channel);
    append_log("*** JOIN sent for %s", channel);
    strncpy(irc.current_channel, channel, sizeof(irc.current_channel)-1);
    irc.current_channel[sizeof(irc.current_channel)-1] = '\0';
}

void part_channel(const char *channel, const char *reason) {
    if (!channel || !channel[0]) {
        append_log("Usage: .part <channel> [reason]");
        return;
    }
    if (reason && reason[0])
        send_raw("PART %s :%s", channel, reason);
    else
        send_raw("PART %s", channel);
    append_log("*** Left %s", channel);
    if (strcmp(channel, irc.current_channel) == 0)
        irc.current_channel[0] = '\0';
}

void disconnect(const char *quitmsg) {
    if (IS_VALID_SOCKET(irc.sockfd) && !irc.connect_attempt) {
        if (quitmsg && quitmsg[0])
            send_raw("QUIT :%s", quitmsg);
        else
            send_raw("QUIT :Leaving");
        CLOSE_SOCKET(irc.sockfd);
        Fl::remove_fd(irc.sockfd);
    }
    irc.sockfd = SOCKET_ERROR_VAL;
    irc.connect_attempt = false;
    append_log("*** Disconnected");
    irc.current_channel[0] = '\0';
    irc.default_target[0] = '\0';
    irc.history.clear();
    irc.history_pos = -1;
    irc.saved_input.clear();
}

// --------------------------------------------------------------
// Display incoming messages
// --------------------------------------------------------------
void display_privmsg(const char *prefix, const char *target, const char *message) {
    char nick[64];
    const char *bang = strchr(prefix, '!');
    if (bang) {
        int len = (int)(bang - prefix);
        if (len >= (int)sizeof(nick)) len = sizeof(nick)-1;
        strncpy(nick, prefix, len);
        nick[len] = '\0';
    } else {
        strcpy(nick, prefix);
    }
    // CTCP ACTION (/me)
    if (message[0] == '\x01') {
        char action_msg[512];
        strncpy(action_msg, message+1, sizeof(action_msg)-1);
        action_msg[sizeof(action_msg)-1] = '\0';
        char *end = strchr(action_msg, '\x01');
        if (end) *end = '\0';
        if (strncmp(action_msg, "ACTION ", 7) == 0) {
            append_log("<%s> * <%s> %s", target, nick, action_msg+7);
            return;
        }
    }
    // Normal message
    if (target[0] == '#')
        append_log("<%s> <%s> %s", target, nick, message);
    else
        append_log("<%s> <%s> %s", target, nick, message);
}

// --------------------------------------------------------------
// Socket callback – normal data reception
// --------------------------------------------------------------
void socket_callback(int fd, void*) {
    char buf[4096];
    int n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        append_log("*** Disconnected from server");
        disconnect(NULL);
        return;
    }
    buf[n] = '\0';
    char *line = strtok(buf, "\r\n");
    while (line) {
        if (strncmp(line, "PING", 4) == 0) {
            char *pong = strchr(line, ':');
            if (pong) send_raw("PONG :%s", pong+1);
            else send_raw("PONG");
            append_log("%s", line);
        }
        else if (line[0] == ':') {
            char *prefix_end = strchr(line+1, ' ');
            if (prefix_end) {
                char *cmd_start = prefix_end + 1;
                if (strncmp(cmd_start, "PRIVMSG ", 8) == 0) {
                    char prefix[128];
                    int prefix_len = (int)(prefix_end - line);
                    if (prefix_len >= (int)sizeof(prefix)) prefix_len = sizeof(prefix)-1;
                    strncpy(prefix, line+1, prefix_len);
                    prefix[prefix_len] = '\0';
                    char *target_start = cmd_start + 8;
                    char *target_end = strchr(target_start, ' ');
                    if (target_end) {
                        char target[64];
                        int target_len = (int)(target_end - target_start);
                        if (target_len >= (int)sizeof(target)) target_len = sizeof(target)-1;
                        strncpy(target, target_start, target_len);
                        target[target_len] = '\0';
                        char *colon = strchr(target_end+1, ':');
                        if (colon) {
                            const char *message = colon+1;
                            display_privmsg(prefix, target, message);
                        } else {
                            append_log("%s", line);
                        }
                    } else {
                        append_log("%s", line);
                    }
                } else {
                    append_log("%s", line);
                }
            } else {
                append_log("%s", line);
            }
        } else {
            append_log("%s", line);
        }
        line = strtok(NULL, "\r\n");
    }
}

// --------------------------------------------------------------
// Non‑blocking connect callbacks with debug logs
// --------------------------------------------------------------
static void connected_callback(int fd, void*) {
    if (!irc.connect_attempt) return;
    append_log("*** Connection callback triggered");
    int err = 0;
    socklen_t len = sizeof(err);
#ifdef _WIN32
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0) {
        append_log("*** getsockopt error: cannot get connection status");
        disconnect(NULL);
        return;
    }
#else
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        append_log("*** getsockopt error: %s", strerror(errno));
        disconnect(NULL);
        return;
    }
#endif
    if (err != 0) {
        append_log("*** Connection failed: %s", socket_strerror(err));
        disconnect(NULL);
        return;
    }
    // Connection successful
    Fl::remove_fd(fd);
    Fl::add_fd(fd, FL_READ, socket_callback);
    irc.sockfd = fd;
    irc.connect_attempt = false;
    send_raw("NICK %s", irc.nickname);
    send_raw("USER %s 0 * :BIC IRC Client", irc.nickname);
    append_log("*** Connected to server (registered)");
}

static void connect_timeout(void*) {
    if (irc.connect_attempt && IS_VALID_SOCKET(irc.sockfd)) {
        append_log("*** Connection timeout (5 seconds) – giving up");
        disconnect(NULL);
    }
}

// --------------------------------------------------------------
// Connect to IRC server (non‑blocking with full debug)
// --------------------------------------------------------------
void connect_to_server(const char *server, int port, const char *nick) {
    if (IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
        append_log("*** Already connected or connecting. Use .quit first.");
        return;
    }

    append_log("*** Resolving hostname: %s", server);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(server, port_str, &hints, &res);
    if (err != 0) {
        append_log("*** getaddrinfo error: %s", gai_strerror(err));
        fl_alert("Cannot resolve hostname: %s\nError: %s", server, gai_strerror(err));
        return;
    }
    append_log("*** Hostname resolved successfully");

    int sock = SOCKET_ERROR_VAL;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (!IS_VALID_SOCKET(sock)) continue;
        append_log("*** Socket created (fd=%d)", sock);
        set_nonblocking(sock);
        append_log("*** Socket set to non‑blocking");
        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            append_log("*** Connected immediately (unusual)");
            break;
        }
#ifdef _WIN32
        int errcode = WSAGetLastError();
        if (errcode == WSAEWOULDBLOCK) {
            append_log("*** Connect in progress (WSAEWOULDBLOCK)");
            break;
        }
#else
        if (errno == EINPROGRESS) {
            append_log("*** Connect in progress (EINPROGRESS)");
            break;
        }
#endif
        append_log("*** Connect failed immediately: %s", socket_strerror(GET_ERROR()));
        CLOSE_SOCKET(sock);
        sock = SOCKET_ERROR_VAL;
    }
    freeaddrinfo(res);

    if (!IS_VALID_SOCKET(sock)) {
        append_log("*** Connection failed: cannot create/connect socket");
        fl_alert("Connection to %s:%d failed", server, port);
        return;
    }

    irc.sockfd = sock;
    irc.connect_attempt = true;
    strncpy(irc.nickname, nick, sizeof(irc.nickname)-1);
    irc.nickname[sizeof(irc.nickname)-1] = '\0';
    append_log("*** Waiting for connection to complete (max 5 seconds)...");

    Fl::add_fd(sock, FL_WRITE, connected_callback);
    Fl::add_timeout(5.0, connect_timeout, nullptr);
}

// --------------------------------------------------------------
// Helper: trim and split command arguments
// --------------------------------------------------------------
void trim(char *str) {
    if (!str) return;
    char *start = str;
    while (*start == ' ') start++;
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\n' || *end == '\r')) end--;
    *(end+1) = '\0';
    if (start != str) memmove(str, start, strlen(start)+1);
}

int split_args(const char *input, char *out1, char *out2, char *out3) {
    char buf[256];
    strncpy(buf, input, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char *args[3] = {out1, out2, out3};
    int count = 0;
    char *token = strtok(buf, " ");
    while (token && count < 3) {
        strncpy(args[count], token, 63);
        args[count][63] = '\0';
        trim(args[count]);
        count++;
        token = strtok(NULL, " ");
    }
    return count;
}

// --------------------------------------------------------------
// Command dispatcher
// --------------------------------------------------------------
void execute_command(const char *cmdline) {
    char cmd[32];
    char arg1[128], arg2[128], arg3[128];
    const char *p = cmdline + 1;
    int i = 0;
    while (*p && *p != ' ') cmd[i++] = *p++;
    cmd[i] = '\0';
    while (*p == ' ') p++;
    int argc = split_args(p, arg1, arg2, arg3);

    if (strcmp(cmd, "connect") == 0) {
        if (argc < 3) { append_log("Usage: .connect <server> <port> <nick>"); return; }
        int port = atoi(arg2);
        if (port <= 0 || port > 65535) { append_log("Invalid port number: %s", arg2); return; }
        connect_to_server(arg1, port, arg3);
    }
    else if (strcmp(cmd, "join") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected. Use .connect first.");
            return;
        }
        if (argc < 1) { append_log("Usage: .join <channel>"); return; }
        join_channel(arg1);
    }
    else if (strcmp(cmd, "part") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected.");
            return;
        }
        char channel[128], reason[512] = {0};
        const char *ptr = cmdline + 5;
        while (*ptr == ' ') ptr++;
        int idx = 0;
        while (*ptr && *ptr != ' ' && idx < (int)sizeof(channel)-1) channel[idx++] = *ptr++;
        channel[idx] = '\0';
        while (*ptr == ' ') ptr++;
        if (*ptr) strncpy(reason, ptr, sizeof(reason)-1);
        if (!channel[0]) {
            if (irc.current_channel[0]) strcpy(channel, irc.current_channel);
            else { append_log("Usage: .part <channel> [reason]"); return; }
        }
        part_channel(channel, reason);
    }
    else if (strcmp(cmd, "target") == 0) {
        if (argc < 1) {
            if (irc.default_target[0]) append_log("*** Current default target: %s", irc.default_target);
            else append_log("*** No default target set (uses current channel).");
            return;
        }
        strncpy(irc.default_target, arg1, sizeof(irc.default_target)-1);
        irc.default_target[sizeof(irc.default_target)-1] = '\0';
        append_log("*** Default target set to %s", irc.default_target);
    }
    else if (strcmp(cmd, "me") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected. Use .connect first.");
            return;
        }
        if (argc < 1) {
            append_log("Usage: .me <action>");
            return;
        }
        char target[64];
        if (irc.default_target[0]) {
            strcpy(target, irc.default_target);
        } else if (irc.current_channel[0]) {
            strcpy(target, irc.current_channel);
        } else {
            append_log("*** No target set. Use .join, .target, or .msg.");
            return;
        }
        const char *ptr = cmdline + 4;
        while (*ptr == ' ') ptr++;
        if (!*ptr) {
            append_log("Usage: .me <action>");
            return;
        }
        char action[512];
        strncpy(action, ptr, sizeof(action)-1);
        action[sizeof(action)-1] = '\0';
        send_raw("PRIVMSG %s :\001ACTION %s\001", target, action);
        append_log("<%s> * <%s> %s", target, irc.nickname, action);
    }
    else if (strcmp(cmd, "names") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected.");
            return;
        }
        if (argc < 1) {
            append_log("Usage: .names <channel>");
            return;
        }
        send_raw("NAMES %s", arg1);
    }
    else if (strcmp(cmd, "list") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected.");
            return;
        }
        if (argc >= 1) send_raw("LIST %s", arg1); else send_raw("LIST");
    }
    else if (strcmp(cmd, "nick") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected.");
            return;
        }
        if (argc < 1) { append_log("Usage: .nick <newnick>"); return; }
        send_raw("NICK %s", arg1);
        strncpy(irc.nickname, arg1, sizeof(irc.nickname)-1);
        irc.nickname[sizeof(irc.nickname)-1] = '\0';
        append_log("*** Nickname changed to %s", arg1);
    }
    else if (strcmp(cmd, "msg") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected.");
            return;
        }
        if (argc < 2) { append_log("Usage: .msg <target> <message>"); return; }
        const char *ptr = p;
        while (*ptr && *ptr != ' ') ptr++;
        while (*ptr == ' ') ptr++;
        char message[512];
        strncpy(message, ptr, sizeof(message)-1);
        message[sizeof(message)-1] = '\0';
        send_raw("PRIVMSG %s :%s", arg1, message);
        append_log("<%s> <%s> %s", arg1, irc.nickname, message);
    }
    else if (strcmp(cmd, "quit") == 0) {
        disconnect(argc >= 1 ? arg1 : NULL);
    }
    else if (strcmp(cmd, "help") == 0) {
        append_log("--- BIC IRC Client Commands ---");
        append_log(".connect <server> <port> <nick>   -- connect to IRC server");
        append_log(".join <channel>                  -- join a channel");
        append_log(".part [channel] [reason]         -- leave a channel (default: current)");
        append_log(".target [target]                 -- set default target for non‑command lines");
        append_log(".names <channel>                 -- list nicks in a channel");
        append_log(".list [pattern]                  -- list channels matching pattern (or all)");
        append_log(".nick <newnick>                  -- change your nickname");
        append_log(".msg <target> <text>             -- send private message to target");
        append_log(".me <action>                     -- send CTCP ACTION (/me) to current target");
        append_log(".quit [message]                  -- disconnect from server (optional quit message)");
        append_log(".help                            -- show this help");
        append_log("---");
        append_log("Any line not starting with '.' is sent to default target or current channel.");
        append_log("Press Up/Down arrows to cycle through message history.");
        append_log("Press Page Up/Down to scroll the chat (FLTK 1.3+).");
    }
    else {
        append_log("Unknown command: .%s. Type .help", cmd);
    }
}

// --------------------------------------------------------------
// Callback for Send button / Enter
// --------------------------------------------------------------
void send_cb(Fl_Widget*, void*) {
    const char *text = irc.msg_input->value();
    if (strlen(text) == 0) return;

    irc.history.push_back(std::string(text));
    if (irc.history.size() > 100) irc.history.erase(irc.history.begin());
    irc.history_pos = -1;
    irc.saved_input.clear();

    if (text[0] == '.') {
        execute_command(text);
    } else {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected. Use .connect first.");
        } else {
            char target[64];
            if (irc.default_target[0]) {
                strcpy(target, irc.default_target);
            } else if (irc.current_channel[0]) {
                strcpy(target, irc.current_channel);
            } else {
                append_log("*** No target set. Use .join, .target, or .msg.");
                irc.msg_input->value("");
                irc.msg_input->take_focus();
                return;
            }
            send_raw("PRIVMSG %s :%s", target, text);
            append_log("<%s> <%s> %s", target, irc.nickname, text);
        }
    }
    irc.msg_input->value("");
    irc.msg_input->take_focus();
}

// --------------------------------------------------------------
// Theme: light green on black, monospace, default button
// --------------------------------------------------------------
void apply_theme(Fl_Window *win, Fl_Text_Display *disp, Fl_Input *inp, Fl_Button *btn) {
    win->color(FL_BLACK);
    disp->color(FL_BLACK);
    disp->textcolor(FL_GREEN);
    disp->cursor_color(FL_GREEN);
    disp->selection_color(FL_DARK_GREEN);
    disp->textfont(FL_COURIER);
    inp->color(FL_BLACK);
    inp->textcolor(FL_GREEN);
    inp->cursor_color(FL_GREEN);
    inp->textfont(FL_COURIER);
    // Button: default system colours
}

// --------------------------------------------------------------
// Main
// --------------------------------------------------------------
int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fl_alert("Failed to initialise Winsock");
        return 1;
    }
#endif

    Fl_Window *win = new Fl_Window(600, 450, "BIC - BIC IRC Client");
    win->begin();

    Fl_Text_Buffer *logbuf = new Fl_Text_Buffer();
    Fl_Text_Display *logdisp = new Fl_Text_Display(0, 0, 600, 420);
    logdisp->buffer(logbuf);
    logdisp->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);

    HistoryInput *msg_in = new HistoryInput(0, 420, 520, 30, "");
    msg_in->callback(send_cb);
    Fl_Button *send_btn = new Fl_Button(520, 420, 80, 30, "Send");
    send_btn->callback(send_cb);

    win->resizable(logdisp);
    win->end();

    apply_theme(win, logdisp, msg_in, send_btn);
    win->show();
    msg_in->take_focus();

    irc.sockfd = SOCKET_ERROR_VAL;
    irc.logbuf = logbuf;
    irc.logdisp = logdisp;
    irc.msg_input = msg_in;
    irc.current_channel[0] = '\0';
    irc.nickname[0] = '\0';
    irc.default_target[0] = '\0';
    irc.history_pos = -1;
    irc.saved_input.clear();
    irc.connect_attempt = false;

    append_log("*** BIC IRC Client started (non‑blocking). Type .help for commands.");

    Fl::run();

    if (IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) disconnect(NULL);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}