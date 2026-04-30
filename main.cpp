// BIC - BIC IRC Client with nick completion (Tab)
// Fixed: Enter works after completion, Tab never sends command, line buffering, safe disconnect
// Compile: g++ -std=c++11 -o bic main.cpp -lfltk -lpthread -lX11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/fl_ask.H>

// --------------------------------------------------------------
// Linux socket headers and helpers
// --------------------------------------------------------------
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
    std::string line_buffer;
    // nick completion
    std::vector<std::string> nicks;      // nicknames in current channel
    std::string completion_prefix;       // last completed prefix
    int completion_index;                // cycling index
};

static IRCClient irc;

static void connect_timeout(void*);

// --------------------------------------------------------------
// Custom input widget with history, PgUp/PgDn, Tab completion, and Enter fix
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
            // ----- Tab completion (never sends) -----
            if (key == FL_Tab) {
                if (irc.current_channel[0] != '\0' && !irc.nicks.empty()) {
                    std::string text = value();
                    int pos = position();
                    int start = pos;
                    while (start > 0 && text[start-1] != ' ') start--;
                    std::string prefix = text.substr(start, pos - start);
                    if (!prefix.empty()) {
                        if (prefix != irc.completion_prefix) {
                            irc.completion_prefix = prefix;
                            irc.completion_index = 0;
                        }
                        std::vector<std::string> matches;
                        for (const auto& nick : irc.nicks) {
                            if (nick.size() >= prefix.size() &&
                                strncasecmp(nick.c_str(), prefix.c_str(), prefix.size()) == 0) {
                                matches.push_back(nick);
                            }
                        }
                        if (!matches.empty()) {
                            if (irc.completion_index >= (int)matches.size())
                                irc.completion_index = 0;
                            std::string new_word = matches[irc.completion_index];
                            irc.completion_index++;
                            std::string new_text = text.substr(0, start) + new_word + text.substr(pos);
                            value(new_text.c_str());
                            position(start + new_word.size());
                            return 1;
                        }
                    }
                }
                // Always eat Tab (prevents accidental sends)
                return 1;
            }
            // ----- Enter key: always trigger send callback -----
            if (key == FL_Enter || key == FL_KP_Enter) {
                if (callback()) {
                    do_callback();
                }
                return 1;
            }
        }
        return Fl_Input::handle(event);
    }
};

// --------------------------------------------------------------
// Timestamp (24‑hour format, HH:MM:SS)
// --------------------------------------------------------------
const char* get_timestamp() {
    static char time_str[10];
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
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
    snprintf(final, sizeof(final), "[%s] %s", get_timestamp(), buffer);
    irc.logbuf->append(final);
    irc.logbuf->append("\n");
    if (was_at_bottom) {
        irc.logdisp->insert_position(irc.logbuf->length());
        irc.logdisp->show_insert_position();
    }
}

// --------------------------------------------------------------
// Send raw IRC message
// --------------------------------------------------------------
void send_raw(const char *fmt, ...) {
    if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) return;
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    strcat(buffer, "\r\n");
    send(irc.sockfd, buffer, strlen(buffer), 0);
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
    // Clear nick list; will be repopulated by NAMES reply
    irc.nicks.clear();
    irc.completion_prefix = "";
    irc.completion_index = 0;
}

void part_channel(const char *channel, const char *reason) {
    if (!channel || !channel[0]) {
        append_log("Usage: /part <channel> [reason]");
        return;
    }
    if (reason && reason[0])
        send_raw("PART %s :%s", channel, reason);
    else
        send_raw("PART %s", channel);
    append_log("*** Left %s", channel);
    if (strcmp(channel, irc.current_channel) == 0) {
        irc.current_channel[0] = '\0';
        irc.nicks.clear();
        irc.completion_prefix = "";
    }
}

void disconnect(const char *quitmsg) {
    if (IS_VALID_SOCKET(irc.sockfd)) {
        if (!irc.connect_attempt && quitmsg && quitmsg[0])
            send_raw("QUIT :%s", quitmsg);
        CLOSE_SOCKET(irc.sockfd);
        Fl::remove_fd(irc.sockfd);
    }
    irc.sockfd = SOCKET_ERROR_VAL;
    irc.connect_attempt = false;
    Fl::remove_timeout(connect_timeout);
    append_log("*** Disconnected");
    irc.current_channel[0] = '\0';
    irc.default_target[0] = '\0';
    irc.nicks.clear();
    irc.completion_prefix = "";
    irc.completion_index = 0;
    irc.history.clear();
    irc.history_pos = -1;
    irc.saved_input.clear();
    irc.line_buffer.clear();
}

// --------------------------------------------------------------
// Helper: extract nick from prefix ":nick!user@host"
// --------------------------------------------------------------
static void extract_nick(const char *prefix, char *nick, size_t nick_size) {
    const char *bang = strchr(prefix, '!');
    if (bang) {
        size_t len = bang - prefix;
        if (len >= nick_size) len = nick_size - 1;
        strncpy(nick, prefix, len);
        nick[len] = '\0';
    } else {
        strncpy(nick, prefix, nick_size - 1);
        nick[nick_size - 1] = '\0';
    }
}

// --------------------------------------------------------------
// Process a complete IRC line (including JOIN, PART, QUIT, NICK, NAMES)
// --------------------------------------------------------------
void process_line(const char *line) {
    // PING
    if (strncmp(line, "PING", 4) == 0) {
        char *pong = strchr(line, ':');
        if (pong) send_raw("PONG :%s", pong+1);
        else send_raw("PONG");
        append_log("%s", line);
        return;
    }

    // All other messages start with ':'
    if (line[0] != ':') {
        append_log("%s", line);
        return;
    }

    // Find prefix end (space after :nick!user@host)
    const char *prefix_end = strchr(line+1, ' ');
    if (!prefix_end) {
        append_log("%s", line);
        return;
    }

    char prefix[128];
    int prefix_len = (int)(prefix_end - line);
    if (prefix_len >= (int)sizeof(prefix)) prefix_len = sizeof(prefix)-1;
    strncpy(prefix, line+1, prefix_len);
    prefix[prefix_len] = '\0';

    const char *cmd_start = prefix_end + 1;
    // Extract command (up to next space)
    char cmd[32];
    int cmd_len = 0;
    while (cmd_start[cmd_len] && cmd_start[cmd_len] != ' ' && cmd_len < 31) cmd_len++;
    strncpy(cmd, cmd_start, cmd_len);
    cmd[cmd_len] = '\0';

    // ----- RPL_NAMREPLY (353) -----
    if (strcmp(cmd, "353") == 0) {
        // :server 353 mynick = #channel :nick1 nick2 nick3 ...
        const char *names_start = strchr(cmd_start + cmd_len, ':');
        if (names_start) {
            names_start++; // skip ':'
            std::string names_str(names_start);
            size_t pos = 0;
            while ((pos = names_str.find(' ')) != std::string::npos) {
                std::string nick = names_str.substr(0, pos);
                if (!nick.empty()) {
                    // Remove mode prefixes (@, +, %, ~, &)
                    if (nick[0] == '@' || nick[0] == '+' || nick[0] == '%' || nick[0] == '~' || nick[0] == '&')
                        nick = nick.substr(1);
                    if (std::find(irc.nicks.begin(), irc.nicks.end(), nick) == irc.nicks.end())
                        irc.nicks.push_back(nick);
                }
                names_str.erase(0, pos + 1);
            }
            if (!names_str.empty()) {
                if (names_str[0] == '@' || names_str[0] == '+' || names_str[0] == '%' || names_str[0] == '~' || names_str[0] == '&')
                    names_str = names_str.substr(1);
                if (std::find(irc.nicks.begin(), irc.nicks.end(), names_str) == irc.nicks.end())
                    irc.nicks.push_back(names_str);
            }
        }
        return; // don't print raw numeric
    }

    // ----- RPL_ENDOFNAMES (366) – nothing to do -----
    if (strcmp(cmd, "366") == 0) {
        return; // ignore
    }

    // ----- JOIN -----
    if (strcmp(cmd, "JOIN") == 0) {
        char nick[64];
        extract_nick(prefix, nick, sizeof(nick));
        // The channel is the last parameter (starts with ':')
        const char *chan_start = cmd_start + cmd_len;
        while (*chan_start == ' ') chan_start++;
        if (*chan_start == ':') chan_start++;
        append_log("*** %s has joined %s", nick, chan_start);
        // Add to nick list if it's our current channel
        if (strcmp(chan_start, irc.current_channel) == 0) {
            std::string new_nick(nick);
            if (std::find(irc.nicks.begin(), irc.nicks.end(), new_nick) == irc.nicks.end())
                irc.nicks.push_back(new_nick);
        }
        return;
    }

    // ----- PART -----
    if (strcmp(cmd, "PART") == 0) {
        char nick[64];
        extract_nick(prefix, nick, sizeof(nick));
        // channel is first argument
        const char *chan_start = cmd_start + cmd_len;
        while (*chan_start == ' ') chan_start++;
        char channel[64];
        int chan_len = 0;
        while (chan_start[chan_len] && chan_start[chan_len] != ' ' && chan_len < 63) chan_len++;
        strncpy(channel, chan_start, chan_len);
        channel[chan_len] = '\0';
        // reason (if any) after colon
        const char *reason_start = strchr(chan_start + chan_len, ':');
        if (reason_start && reason_start[1]) {
            append_log("*** %s has left %s (%s)", nick, channel, reason_start+1);
        } else {
            append_log("*** %s has left %s", nick, channel);
        }
        // Remove from nick list if it's our current channel
        if (strcmp(channel, irc.current_channel) == 0) {
            std::string leaving_nick(nick);
            auto it = std::find(irc.nicks.begin(), irc.nicks.end(), leaving_nick);
            if (it != irc.nicks.end()) irc.nicks.erase(it);
        }
        return;
    }

    // ----- QUIT -----
    if (strcmp(cmd, "QUIT") == 0) {
        char nick[64];
        extract_nick(prefix, nick, sizeof(nick));
        const char *reason_start = strchr(cmd_start + cmd_len, ':');
        if (reason_start && reason_start[1]) {
            append_log("*** %s has quit (%s)", nick, reason_start+1);
        } else {
            append_log("*** %s has quit", nick);
        }
        // Remove from nick list (if present)
        std::string quitting_nick(nick);
        auto it = std::find(irc.nicks.begin(), irc.nicks.end(), quitting_nick);
        if (it != irc.nicks.end()) irc.nicks.erase(it);
        return;
    }

    // ----- NICK -----
    if (strcmp(cmd, "NICK") == 0) {
        char old_nick[64];
        extract_nick(prefix, old_nick, sizeof(old_nick));
        const char *new_nick_start = strchr(cmd_start + cmd_len, ':');
        if (!new_nick_start) new_nick_start = cmd_start + cmd_len;
        while (*new_nick_start == ' ') new_nick_start++;
        if (*new_nick_start == ':') new_nick_start++;
        append_log("*** %s is now known as %s", old_nick, new_nick_start);
        // Update nick list
        std::string old(old_nick);
        std::string newn(new_nick_start);
        auto it = std::find(irc.nicks.begin(), irc.nicks.end(), old);
        if (it != irc.nicks.end()) {
            irc.nicks.erase(it);
            if (std::find(irc.nicks.begin(), irc.nicks.end(), newn) == irc.nicks.end())
                irc.nicks.push_back(newn);
        }
        return;
    }

    // ----- PRIVMSG -----
    if (strcmp(cmd, "PRIVMSG") == 0) {
        char nick[64];
        extract_nick(prefix, nick, sizeof(nick));
        // Find target (channel or nick)
        const char *target_start = cmd_start + cmd_len;
        while (*target_start == ' ') target_start++;
        const char *target_end = strchr(target_start, ' ');
        if (!target_end) {
            append_log("%s", line);
            return;
        }
        char target[64];
        int target_len = (int)(target_end - target_start);
        if (target_len >= (int)sizeof(target)) target_len = sizeof(target)-1;
        strncpy(target, target_start, target_len);
        target[target_len] = '\0';
        // Message after colon
        const char *colon = strchr(target_end+1, ':');
        if (!colon) {
            append_log("%s", line);
            return;
        }
        const char *message = colon+1;
        // CTCP ACTION
        if (message[0] == '\x01') {
            char action_msg[512];
            strncpy(action_msg, message+1, sizeof(action_msg)-1);
            action_msg[sizeof(action_msg)-1] = '\0';
            char *end = strchr(action_msg, '\x01');
            if (end) *end = '\0';
            if (strncmp(action_msg, "ACTION ", 7) == 0) {
                append_log("* %s %s", nick, action_msg+7);
                return;
            }
        }
        if (target[0] == '#')
            append_log("[%s] <%s> %s", target, nick, message);
        else
            append_log("[PM] <%s> %s", nick, message);
        return;
    }

    // ----- Everything else (raw) -----
    append_log("%s", line);
}

// --------------------------------------------------------------
// Socket callback – line buffering
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
    irc.line_buffer += buf;

    size_t pos;
    while ((pos = irc.line_buffer.find("\r\n")) != std::string::npos) {
        std::string line = irc.line_buffer.substr(0, pos);
        irc.line_buffer.erase(0, pos + 2);
        process_line(line.c_str());
    }
}

// --------------------------------------------------------------
// Non‑blocking connect callbacks
// --------------------------------------------------------------
static void connected_callback(int fd, void*) {
    if (!irc.connect_attempt) return;
    Fl::remove_timeout(connect_timeout);
    append_log("*** Connection callback triggered");
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        append_log("*** getsockopt error: %s", strerror(errno));
        disconnect(NULL);
        return;
    }
    if (err != 0) {
        append_log("*** Connection failed: %s", socket_strerror(err));
        disconnect(NULL);
        return;
    }
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
// Connect to IRC server (non‑blocking)
// --------------------------------------------------------------
void connect_to_server(const char *server, int port, const char *nick) {
    if (IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
        append_log("*** Already connected or connecting. Use /quit first.");
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
        if (errno == EINPROGRESS) {
            append_log("*** Connect in progress (EINPROGRESS)");
            break;
        }
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
        if (argc < 3) { append_log("Usage: /connect <server> <port> <nick>"); return; }
        int port = atoi(arg2);
        if (port <= 0 || port > 65535) { append_log("Invalid port number: %s", arg2); return; }
        connect_to_server(arg1, port, arg3);
    }
    else if (strcmp(cmd, "join") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected. Use /connect first.");
            return;
        }
        if (argc < 1) { append_log("Usage: /join <channel>"); return; }
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
            else { append_log("Usage: /part <channel> [reason]"); return; }
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
            append_log("*** Not connected. Use /connect first.");
            return;
        }
        if (argc < 1) {
            append_log("Usage: /me <action>");
            return;
        }
        char target[64];
        if (irc.default_target[0]) {
            strcpy(target, irc.default_target);
        } else if (irc.current_channel[0]) {
            strcpy(target, irc.current_channel);
        } else {
            append_log("*** No target set. Use /join, /target, or /msg.");
            return;
        }
        const char *ptr = cmdline + 4;
        while (*ptr == ' ') ptr++;
        if (!*ptr) {
            append_log("Usage: /me <action>");
            return;
        }
        char action[512];
        strncpy(action, ptr, sizeof(action)-1);
        action[sizeof(action)-1] = '\0';
        send_raw("PRIVMSG %s :\001ACTION %s\001", target, action);
        append_log("* %s %s", irc.nickname, action);
    }
    else if (strcmp(cmd, "names") == 0) {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected.");
            return;
        }
        if (argc < 1) {
            append_log("Usage: /names <channel>");
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
        if (argc < 1) { append_log("Usage: /nick <newnick>"); return; }
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
        if (argc < 2) { append_log("Usage: /msg <target> <message>"); return; }
        const char *ptr = p;
        while (*ptr && *ptr != ' ') ptr++;
        while (*ptr == ' ') ptr++;
        char message[512];
        strncpy(message, ptr, sizeof(message)-1);
        message[sizeof(message)-1] = '\0';
        send_raw("PRIVMSG %s :%s", arg1, message);
        if (arg1[0] == '#')
            append_log("[%s] <%s> %s", arg1, irc.nickname, message);
        else
            append_log("[PM] <%s> %s", irc.nickname, message);
    }
    else if (strcmp(cmd, "clear") == 0) {
        irc.logbuf->text("");
        append_log("*** Display cleared");
    }
    else if (strcmp(cmd, "disconnect") == 0) {
        disconnect(argc >= 1 ? arg1 : NULL);
    }
    else if (strcmp(cmd, "quit") == 0) {
        disconnect(argc >= 1 ? arg1 : NULL);
    }
    else if (strcmp(cmd, "help") == 0) {
        append_log("--- BIC IRC Client Commands ---");
        append_log("/connect <server> <port> <nick>   -- connect to IRC server");
        append_log("/join <channel>                  -- join a channel");
        append_log("/part [channel] [reason]         -- leave a channel (default: current)");
        append_log("/target [target]                 -- set default target for non‑command lines");
        append_log("/names <channel>                 -- list nicks in a channel");
        append_log("/list [pattern]                  -- list channels (not always supported)");
        append_log("/nick <newnick>                  -- change your nickname");
        append_log("/msg <target> <text>             -- send private message to target");
        append_log("/me <action>                     -- send CTCP ACTION (/me) to current target");
        append_log("/clear                           -- clear the chat display");
        append_log("/disconnect [message]            -- disconnect from server");
        append_log("/quit [message]                  -- disconnect from server (alias)");
        append_log("/help                            -- show this help");
        append_log("---");
        append_log("Any line not starting with '/' is sent to default target or current channel.");
        append_log("Press Up/Down arrows to cycle through message history.");
        append_log("Tab completes nicknames in the current channel.");
    }
    else {
        append_log("Unknown command: /%s. Type /help", cmd);
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

    if (text[0] == '/') {
        execute_command(text);
    } else {
        if (!IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) {
            append_log("*** Not connected. Use /connect first.");
        } else {
            char target[64];
            if (irc.default_target[0]) {
                strcpy(target, irc.default_target);
            } else if (irc.current_channel[0]) {
                strcpy(target, irc.current_channel);
            } else {
                append_log("*** No target set. Use /join, /target, or /msg.");
                irc.msg_input->value("");
                irc.msg_input->take_focus();
                return;
            }
            send_raw("PRIVMSG %s :%s", target, text);
            if (target[0] == '#')
                append_log("[%s] <%s> %s", target, irc.nickname, text);
            else
                append_log("[PM] <%s> %s", irc.nickname, text);
        }
    }
    irc.msg_input->value("");
    irc.msg_input->take_focus();
}

// --------------------------------------------------------------
// Theme: light green on black, monospace
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
}

// --------------------------------------------------------------
// Main
// --------------------------------------------------------------
int main() {
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
    irc.line_buffer.clear();
    irc.completion_index = 0;
    irc.completion_prefix = "";

    append_log("*** BIC IRC Client started. Type /help for commands.");
    append_log("*** Tab completes nicknames in the current channel.");

    Fl::run();

    if (IS_VALID_SOCKET(irc.sockfd) || irc.connect_attempt) disconnect(NULL);

    return 0;
}