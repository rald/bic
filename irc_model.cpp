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
    if (IS_VALID_SOCKET(sockfd_)) disconnect();
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
    send(sockfd_, out.c_str(), out.size(), 0);
}

void IRCModel::sendPrivmsg(const std::string& target, const std::string& msg) {
    sendRaw("PRIVMSG " + target + " :" + msg);
}

void IRCModel::sendAction(const std::string& target, const std::string& action) {
    sendRaw("PRIVMSG " + target + " :\001ACTION " + action + "\001");
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
        sendRaw("PART " + channel + " :" + reason);
    if (channel == currentChannel_) currentChannel_.clear();
}

void IRCModel::changeNick(const std::string& newnick) {
    sendRaw("NICK " + newnick);
    nickname_ = newnick;
}

void IRCModel::requestNames(const std::string& channel) {
    // Clear current nick list for this channel before requesting fresh list
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
    if (controller_) controller_->onError("Attempting to connect to " + server + ":" + std::to_string(port) + " as " + nick);  // DEBUG

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
                (void)fd; // unused parameter
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
    Fl::add_fd(sockfd, FL_WRITE, [](int fd, void* data) {
        IRCModel* self = static_cast<IRCModel*>(data);
        if (!self->connectAttempt_) return;
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            self->connectAttempt_ = false;
            CLOSE_SOCKET(fd);
            self->sockfd_ = SOCKET_ERROR_VAL;
            if (self->controller_) self->controller_->onConnectionFailed(strerror(err));
            return;
        }
        Fl::remove_fd(fd);
        Fl::add_fd(fd, FL_READ, [](int fd2, void* data2) {
            (void)fd2; // unused parameter
            static_cast<IRCModel*>(data2)->onSocketReady();
        }, self);
        self->connectAttempt_ = false;
        self->sendRaw("NICK " + self->nickname_);
        self->sendRaw("USER " + self->nickname_ + " 0 * :BIC IRC Client");
        if (self->controller_) self->controller_->onConnected();
    }, this);

    Fl::add_timeout(5.0, [](void* data) {
        IRCModel* self = static_cast<IRCModel*>(data);
        if (self->connectAttempt_) {
            self->disconnect();
            if (self->controller_) self->controller_->onConnectionFailed("Connection timeout");
        }
    }, this);
}

void IRCModel::disconnect(const std::string& quitmsg) {
    if (IS_VALID_SOCKET(sockfd_)) {
        if (isConnected() && !quitmsg.empty()) sendRaw("QUIT :" + quitmsg);
        CLOSE_SOCKET(sockfd_);
        Fl::remove_fd(sockfd_);
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
    if (n <= 0) {
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
        processLine(line);
    }
}

void IRCModel::processLine(const std::string& line) {
    // PING
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

    // RPL_NAMREPLY (353)
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

    // RPL_ENDOFMOTD (376) or ERR_NOMOTD (422)
    if (cmd == "376" || cmd == "422") {
        if (controller_) controller_->onMotdEnd();
        return;
    }
    
    if (cmd == "366") {
        // Extract channel (third token) – fixed
        size_t space1 = params.find(' ');
        if (space1 != std::string::npos) {
            size_t space2 = params.find(' ', space1 + 1);
            if (space2 != std::string::npos) {
                std::string channel = params.substr(space1 + 1, space2 - space1 - 1);
                if (!channel.empty() && channel[0] == ':')
                    channel = channel.substr(1);
                if (controller_) {
                    controller_->onNamesComplete(channel, nicks_);
                }
            }
        }
        return;
    }

    // Numeric replies (server messages)
    if (cmd.size() == 3 && std::isdigit(cmd[0]) && std::isdigit(cmd[1]) && std::isdigit(cmd[2])) {
        // Extract the human-readable message after the colon
        size_t colon = params.find(':');
        std::string msg = (colon != std::string::npos) ? params.substr(colon + 1) : params;
        if (!msg.empty() && controller_) {
            // Special handling for common numerics to produce prettier messages
            int num = std::stoi(cmd);
            switch (num) {
                case 001: // RPL_WELCOME
                    controller_->onServerMessage("Welcome to the IRC server: " + msg);
                    break;
                case 002: // RPL_YOURHOST
                case 003: // RPL_CREATED
                case 004: // RPL_MYINFO
                    controller_->onServerMessage(msg);
                    break;
                case 251: // RPL_LUSERCLIENT
                case 252: // RPL_LUSEROP
                case 253: // RPL_LUSERUNKNOWN
                case 254: // RPL_LUSERCHANNELS
                case 255: // RPL_LUSERME
                case 265: // RPL_LOCALUSERS
                case 266: // RPL_GLOBALUSERS
                    controller_->onServerMessage(msg);
                    break;
                case 372: // RPL_MOTD
                    controller_->onServerMessage(msg);
                    break;
                case 375: // RPL_MOTDSTART
                    controller_->onServerMessage("MOTD: " + msg);
                    break;
                // 376 handled separately (onMotdEnd)
                // 422 handled separately (no MOTD)
                default:
                    // For any other numeric, just show the raw line (optional)
                    // controller_->onRawLine(line);
                    break;
            }
        }
        return; // Don't show numeric again as raw line
    }

    // PRIVMSG
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

    // JOIN
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

    // PART
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

    // QUIT
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

    // NICK
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

    // fallback
    if (controller_) controller_->onRawLine(line);
}

void IRCModel::extractNickFromPrefix(const std::string& prefix, std::string& nick) const {
    size_t bang = prefix.find('!');
    if (bang != std::string::npos)
        nick = prefix.substr(0, bang);
    else
        nick = prefix;
}