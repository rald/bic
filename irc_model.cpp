#include "irc_model.h"
#include "irc_controller.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <FL/Fl.H>

#define CLOSE_SOCKET(s) close(s)
#define IS_VALID_SOCKET(s) ((s) >= 0)
#define SOCKET_ERROR_VAL (-1)
#define MAX_LINE_LEN 512

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

IRCModel::IRCModel()
    : sockfd_(SOCKET_ERROR_VAL)
    , connectAttempt_(false)
    , controller_(nullptr)
{
}

IRCModel::~IRCModel() {
    Fl::remove_timeout(&IRCModel::connectTimeoutCallback, this);
    if (IS_VALID_SOCKET(sockfd_)) {
        Fl::remove_fd(sockfd_);
        CLOSE_SOCKET(sockfd_);
    }
}

void IRCModel::setController(IRCController* ctrl) {
    controller_ = ctrl;
}

bool IRCModel::isConnected() const {
    return IS_VALID_SOCKET(sockfd_) && !connectAttempt_;
}

bool IRCModel::isConnecting() const {
    return connectAttempt_;
}

void IRCModel::sendRaw(const std::string& msg) {
    if (!isConnected()) return;
    std::string out = msg + "\r\n";
    int ret = send(sockfd_, out.c_str(), out.size(), 0);
    if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Output buffer full – will be sent when socket becomes writable.
            // For simplicity, we drop the data this time. A full solution would buffer.
            return;
        }
        if (controller_) controller_->onError("Send error, disconnecting");
        disconnect();
    }
}

static std::string sanitizeMessage(const std::string& msg) {
    std::string safe;
    safe.reserve(msg.size());
    for (char c : msg) {
        if (c != '\r' && c != '\n')
            safe.push_back(c);
    }
    return safe;
}

void IRCModel::sendPrivmsg(const std::string& target, const std::string& msg) {
    sendRaw("PRIVMSG " + target + " :" + sanitizeMessage(msg));
}

void IRCModel::sendAction(const std::string& target, const std::string& action) {
    sendRaw("PRIVMSG " + target + " :\001ACTION " + sanitizeMessage(action) + "\001");
}

void IRCModel::joinChannel(const std::string& channel, const std::string& key) {
    if (key.empty())
        sendRaw("JOIN " + channel);
    else
        sendRaw("JOIN " + channel + " " + key);
    currentChannel_ = channel;
    nicks_.clear();
}

void IRCModel::partChannel(const std::string& channel, const std::string& reason) {
    if (reason.empty())
        sendRaw("PART " + channel);
    else
        sendRaw("PART " + channel + " :" + sanitizeMessage(reason));
    if (channel == currentChannel_) currentChannel_.clear();
}

void IRCModel::changeNick(const std::string& newnick) {
    sendRaw("NICK " + sanitizeMessage(newnick));
    nickname_ = newnick;
}

void IRCModel::requestNames(const std::string& channel) {
    nicks_.clear();
    sendRaw("NAMES " + channel);
}

std::string IRCModel::getCurrentChannel() const {
    return currentChannel_;
}

std::string IRCModel::getNickname() const {
    return nickname_;
}

std::string IRCModel::getDefaultTarget() const {
    return defaultTarget_;
}

void IRCModel::setDefaultTarget(const std::string& target) {
    defaultTarget_ = target;
}

const std::vector<std::string>& IRCModel::getNicks() const {
    return nicks_;
}

void IRCModel::connectToServer(const std::string& server, int port, const std::string& nick) {
    if (isConnected() || connectAttempt_) {
        if (controller_) controller_->onError("Already connected or connecting");
        return;
    }

    // Remove any previous timeout (e.g., from a previous failed attempt)
    Fl::remove_timeout(&IRCModel::connectTimeoutCallback, this);

    nickname_ = nick;
    connectAttempt_ = true;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(server.c_str(), port_str, &hints, &res);
    if (err != 0) {
        if (controller_) controller_->onError("Cannot resolve hostname: " + std::string(gai_strerror(err)));
        connectAttempt_ = false;
        return;
    }

    for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        sockfd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (!IS_VALID_SOCKET(sockfd_)) continue;
        set_nonblocking(sockfd_);
        if (connect(sockfd_, rp->ai_addr, rp->ai_addrlen) == 0) {
            connectAttempt_ = false;
            Fl::add_fd(sockfd_, FL_READ, [](int fd, void* data) {
                (void)fd;
                static_cast<IRCModel*>(data)->onSocketReady();
            }, this);
            sendRaw("NICK " + nickname_);
            sendRaw("USER " + nickname_ + " 0 * :BIC IRC Client");
            if (controller_) controller_->onConnected();
            break;
        }
        if (errno == EINPROGRESS) {
            startNonBlockingConnect(sockfd_);
            break;
        }
        CLOSE_SOCKET(sockfd_);
        sockfd_ = SOCKET_ERROR_VAL;
    }
    freeaddrinfo(res);

    if (!IS_VALID_SOCKET(sockfd_)) {
        connectAttempt_ = false;
        if (controller_) controller_->onConnectionFailed("Cannot create/connect socket");
    }
}

void IRCModel::startNonBlockingConnect(int sockfd) {
    Fl::repeat_timeout(5.0, &IRCModel::connectTimeoutCallback, this);

    Fl::add_fd(sockfd, FL_WRITE, [](int fd, void* data) {
        IRCModel* self = static_cast<IRCModel*>(data);
        if (!self->connectAttempt_) return;

        Fl::remove_timeout(&IRCModel::connectTimeoutCallback, self);

        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            self->connectAttempt_ = false;
            Fl::remove_fd(fd);
            CLOSE_SOCKET(fd);
            self->sockfd_ = SOCKET_ERROR_VAL;
            if (self->controller_) self->controller_->onConnectionFailed(strerror(err));
            return;
        }

        Fl::remove_fd(fd);
        Fl::add_fd(fd, FL_READ, [](int fd2, void* data2) {
            (void)fd2;
            static_cast<IRCModel*>(data2)->onSocketReady();
        }, self);
        self->connectAttempt_ = false;
        self->sendRaw("NICK " + self->nickname_);
        self->sendRaw("USER " + self->nickname_ + " 0 * :BIC IRC Client");
        if (self->controller_) self->controller_->onConnected();
    }, this);
}

void IRCModel::connectTimeoutCallback(void* data) {
    IRCModel* self = static_cast<IRCModel*>(data);
    if (self->connectAttempt_) {
        self->disconnect();
        if (self->controller_) self->controller_->onConnectionFailed("Connection timeout");
    }
}

void IRCModel::disconnect(const std::string& quitmsg) {
    Fl::remove_timeout(&IRCModel::connectTimeoutCallback, this);

    if (IS_VALID_SOCKET(sockfd_)) {
        if (isConnected() && !quitmsg.empty()) sendRaw("QUIT :" + sanitizeMessage(quitmsg));
        Fl::remove_fd(sockfd_);
        CLOSE_SOCKET(sockfd_);
    }
    sockfd_ = SOCKET_ERROR_VAL;
    connectAttempt_ = false;
    currentChannel_.clear();
    defaultTarget_.clear();
    nicks_.clear();
    lineBuffer_.clear();
    if (controller_) controller_->onDisconnected();
}

void IRCModel::onSocketReady() {
    char buf[4096];
    int n = recv(sockfd_, buf, sizeof(buf)-1, 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // no data yet
        disconnect();
        return;
    }
    if (n == 0) {
        disconnect();
        return;
    }
    buf[n] = '\0';
    feedRecvData(buf, n);
}

void IRCModel::feedRecvData(const char* data, int len) {
    lineBuffer_.append(data, len);
    size_t pos;
    while ((pos = lineBuffer_.find("\r\n")) != std::string::npos) {
        std::string line = lineBuffer_.substr(0, pos);
        lineBuffer_.erase(0, pos + 2);
        if (line.size() > MAX_LINE_LEN) line = line.substr(0, MAX_LINE_LEN);
        processLine(line);
    }
    if ((pos = lineBuffer_.find('\n')) != std::string::npos) {
        std::string line = lineBuffer_.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lineBuffer_.erase(0, pos + 1);
        if (line.size() > MAX_LINE_LEN) line = line.substr(0, MAX_LINE_LEN);
        processLine(line);
    }
}

void IRCModel::processLine(const std::string& line) {
    if (line.compare(0, 4, "PING") == 0) {
        size_t colon = line.find(':');
        if (colon != std::string::npos)
            sendRaw("PONG :" + line.substr(colon+1));
        else
            sendRaw("PONG");
        if (controller_) controller_->onRawLine(line);
        return;
    }

    if (line.empty() || line[0] != ':') {
        if (controller_) controller_->onRawLine(line);
        return;
    }

    size_t space1 = line.find(' ', 1);
    if (space1 == std::string::npos) return;
    std::string prefix = line.substr(1, space1-1);
    size_t cmdStart = space1+1;
    size_t space2 = line.find(' ', cmdStart);
    std::string cmd = line.substr(cmdStart, space2 - cmdStart);
    std::string params;
    if (space2 != std::string::npos) params = line.substr(space2+1);

    if (cmd == "353") {
        size_t colon = params.find(':');
        if (colon != std::string::npos) {
            std::string nickList = params.substr(colon+1);
            std::vector<std::string> newNicks;
            size_t pos = 0;
            while ((pos = nickList.find(' ')) != std::string::npos) {
                std::string n = nickList.substr(0, pos);
                if (!n.empty()) {
                    if (n[0] == '@' || n[0] == '+' || n[0] == '%' || n[0] == '~' || n[0] == '&')
                        n = n.substr(1);
                    newNicks.push_back(n);
                }
                nickList.erase(0, pos+1);
            }
            if (!nickList.empty()) {
                if (nickList[0] == '@' || nickList[0] == '+' || nickList[0] == '%' || nickList[0] == '~' || nickList[0] == '&')
                    nickList = nickList.substr(1);
                newNicks.push_back(nickList);
            }
            for (const auto& n : newNicks) {
                if (std::find(nicks_.begin(), nicks_.end(), n) == nicks_.end())
                    nicks_.push_back(n);
            }
            if (controller_) controller_->onNickList(nicks_);
        }
        return;
    }

    if (cmd == "376" || cmd == "422") {
        if (controller_) controller_->onMotdEnd();
        return;
    }
    
    if (cmd == "366") {
        // Numeric 366: end of NAMES list. Extract channel name robustly.
        // Format: <client> 366 <nick> <channel> :End of /NAMES list
        std::string channel;
        size_t first = params.find(' ');
        if (first != std::string::npos) {
            size_t second = params.find(' ', first + 1);
            if (second != std::string::npos) {
                size_t third = params.find(' ', second + 1);
                if (third != std::string::npos) {
                    channel = params.substr(second + 1, third - second - 1);
                    if (!channel.empty() && channel[0] == ':')
                        channel = channel.substr(1);
                }
            }
        }
        if (!channel.empty() && controller_) {
            controller_->onNamesComplete(channel, nicks_);
        }
        return;
    }

    if (cmd.size() == 3 && std::isdigit(cmd[0]) && std::isdigit(cmd[1]) && std::isdigit(cmd[2])) {
        size_t colon = params.find(':');
        std::string msg = (colon != std::string::npos) ? params.substr(colon + 1) : params;
        if (!msg.empty() && controller_) {
            int num = std::stoi(cmd);
            switch (num) {
                case 001: controller_->onServerMessage("Welcome to the IRC server: " + msg); break;
                case 002: case 003: case 004: controller_->onServerMessage(msg); break;
                case 251: case 252: case 253: case 254: case 255: case 265: case 266:
                    controller_->onServerMessage(msg); break;
                case 372: controller_->onServerMessage(msg); break;
                case 375: controller_->onServerMessage("MOTD: " + msg); break;
                default: break;
            }
        }
        return;
    }

    if (cmd == "PRIVMSG") {
        size_t targetEnd = params.find(' ');
        if (targetEnd == std::string::npos) return;
        std::string target = params.substr(0, targetEnd);
        size_t colon = params.find(':', targetEnd);
        if (colon == std::string::npos) return;
        std::string msg = params.substr(colon+1);
        std::string nick;
        extractNickFromPrefix(prefix, nick);
        if (msg.size() >= 8 && msg.substr(0, 7) == "\001ACTION" && msg.back() == '\001') {
            std::string action = msg.substr(8, msg.size()-9);
            if (controller_) controller_->onAction(target, nick, action);
        } else {
            if (target[0] == '#') {
                if (controller_) controller_->onChannelMessage(target, nick, msg);
            } else {
                if (controller_) controller_->onPmMessage(nick, msg);
            }
        }
        return;
    }

    if (cmd == "JOIN") {
        size_t colon = params.find(':');
        std::string channel = (colon != std::string::npos) ? params.substr(colon+1) : params;
        std::string nick;
        extractNickFromPrefix(prefix, nick);
        if (controller_) controller_->onJoin(nick, channel);
        if (nick == nickname_ && !channel.empty()) {
            currentChannel_ = channel;
            nicks_.clear();
        } else if (channel == currentChannel_) {
            if (std::find(nicks_.begin(), nicks_.end(), nick) == nicks_.end())
                nicks_.push_back(nick);
            if (controller_) controller_->onNickList(nicks_);
        }
        return;
    }

    if (cmd == "PART") {
        size_t space = params.find(' ');
        std::string channel = params.substr(0, space);
        std::string reason;
        size_t colon = params.find(':');
        if (colon != std::string::npos) reason = params.substr(colon+1);
        std::string nick;
        extractNickFromPrefix(prefix, nick);
        if (controller_) controller_->onPart(nick, channel, reason);
        if (channel == currentChannel_ && nick != nickname_) {
            auto it = std::find(nicks_.begin(), nicks_.end(), nick);
            if (it != nicks_.end()) nicks_.erase(it);
            if (controller_) controller_->onNickList(nicks_);
        }
        if (nick == nickname_ && channel == currentChannel_) currentChannel_.clear();
        return;
    }

    if (cmd == "QUIT") {
        std::string reason;
        size_t colon = params.find(':');
        if (colon != std::string::npos) reason = params.substr(colon+1);
        std::string nick;
        extractNickFromPrefix(prefix, nick);
        if (controller_) controller_->onQuit(nick, reason);
        auto it = std::find(nicks_.begin(), nicks_.end(), nick);
        if (it != nicks_.end()) nicks_.erase(it);
        if (controller_) controller_->onNickList(nicks_);
        return;
    }

    if (cmd == "NICK") {
        std::string newnick;
        size_t colon = params.find(':');
        if (colon != std::string::npos) newnick = params.substr(colon+1);
        else newnick = params;
        std::string oldnick;
        extractNickFromPrefix(prefix, oldnick);
        if (controller_) controller_->onNickChange(oldnick, newnick);
        if (oldnick == nickname_) nickname_ = newnick;
        auto it = std::find(nicks_.begin(), nicks_.end(), oldnick);
        if (it != nicks_.end()) {
            nicks_.erase(it);
            if (std::find(nicks_.begin(), nicks_.end(), newnick) == nicks_.end())
                nicks_.push_back(newnick);
            if (controller_) controller_->onNickList(nicks_);
        }
        return;
    }

    if (controller_) controller_->onRawLine(line);
}

void IRCModel::extractNickFromPrefix(const std::string& prefix, std::string& nick) const {
    size_t bang = prefix.find('!');
    if (bang != std::string::npos)
        nick = prefix.substr(0, bang);
    else
        nick = prefix;
}