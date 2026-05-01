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

void IRCModel::sendCtcp(const std::string& target, const std::string& command, const std::string& args) {
    std::string ctcp = "\001" + command;
    if (!args.empty()) ctcp += " " + sanitizeMessage(args);
    ctcp += "\001";
    sendRaw("PRIVMSG " + target + " :" + ctcp);
}

void IRCModel::sendCtcpReply(const std::string& target, const std::string& command, const std::string& args) {
    std::string ctcp = "\001" + command;
    if (!args.empty()) ctcp += " " + sanitizeMessage(args);
    ctcp += "\001";
    sendRaw("NOTICE " + target + " :" + ctcp);
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
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
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

bool IRCModel::parseCtcpMessage(const std::string& msg, std::string& cmd, std::string& args) const {
    if (msg.size() < 2 || msg.front() != '\001' || msg.back() != '\001')
        return false;
    std::string inner = msg.substr(1, msg.size() - 2);
    size_t sp = inner.find(' ');
    if (sp == std::string::npos) {
        cmd = inner;
        args.clear();
    } else {
        cmd = inner.substr(0, sp);
        args = inner.substr(sp + 1);
    }
    return true;
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

    // Handle NOTICE (for CTCP replies and generic server notices)
    if (cmd == "NOTICE") {
        size_t targetEnd = params.find(' ');
        if (targetEnd == std::string::npos) return;
        std::string target = params.substr(0, targetEnd);
        size_t colon = params.find(':', targetEnd);
        if (colon == std::string::npos) return;
        std::string msg = params.substr(colon+1);
        std::string nick;
        extractNickFromPrefix(prefix, nick);

        std::string ctcpCmd, ctcpArgs;
        if (parseCtcpMessage(msg, ctcpCmd, ctcpArgs)) {
            // CTCP reply
            if (controller_) controller_->onCtcpReply(nick, ctcpCmd, ctcpArgs);
        } else {
            // Normal notice
            if (controller_) controller_->onNotice(nick, msg);
        }
        return;
    }

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

                case 311: // RPL_WHOISUSER
                    {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            size_t second = params.find(' ', first+1);
                            if (second != std::string::npos) {
                                size_t third = params.find(' ', second+1);
                                if (third != std::string::npos) {
                                    std::string nick = params.substr(0, first);
                                    std::string user = params.substr(first+1, second-first-1);
                                    std::string host = params.substr(second+1, third-second-1);
                                    size_t colon = params.find(':', third);
                                    std::string real = (colon != std::string::npos) ? params.substr(colon+1) : "";
                                    controller_->onServerMessage("WHOIS " + nick + ": " + user + "@" + host + " (" + real + ")");
                                }
                            }
                        }
                    }
                    break;
                case 312: // RPL_WHOISSERVER
                    {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            size_t second = params.find(' ', first+1);
                            if (second != std::string::npos) {
                                std::string nick = params.substr(0, first);
                                std::string server = params.substr(first+1, second-first-1);
                                size_t colon = params.find(':', second);
                                std::string info = (colon != std::string::npos) ? params.substr(colon+1) : "";
                                controller_->onServerMessage("WHOIS " + nick + ": connected via " + server + " (" + info + ")");
                            }
                        }
                    }
                    break;
                case 317: // RPL_WHOISIDLE
                    {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            size_t second = params.find(' ', first+1);
                            if (second != std::string::npos) {
                                size_t third = params.find(' ', second+1);
                                if (third != std::string::npos) {
                                    std::string nick = params.substr(0, first);
                                    std::string idleSec = params.substr(first+1, second-first-1);
                                    controller_->onServerMessage("WHOIS " + nick + ": idle " + idleSec + " seconds");
                                }
                            }
                        }
                    }
                    break;
                case 318: // RPL_ENDOFWHOIS
                    {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            size_t second = params.find(' ', first+1);
                            if (second != std::string::npos) {
                                std::string nick = params.substr(0, first);
                                controller_->onServerMessage("End of WHOIS for " + nick);
                            }
                        }
                    }
                    break;
                case 319: // RPL_WHOISCHANNELS
                    {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            std::string nick = params.substr(0, first);
                            size_t colon = params.find(':');
                            std::string channels = (colon != std::string::npos) ? params.substr(colon+1) : "";
                            controller_->onServerMessage("WHOIS " + nick + ": is on " + channels);
                        }
                    }
                    break;
                case 401: // ERR_NOSUCHNICK
                    {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            size_t second = params.find(' ', first+1);
                            if (second != std::string::npos) {
                                std::string nick = params.substr(first+1, second-first-1);
                                controller_->onServerMessage("No such nick: " + nick);
                            }
                        }
                    }
                    break;

                case 321: // RPL_LISTSTART
                    if (controller_) controller_->onServerMessage("Channel list:");
                    break;

                case 322: // RPL_LIST
                    if (controller_) {
                        size_t first = params.find(' ');
                        if (first != std::string::npos) {
                            size_t second = params.find(' ', first + 1);
                            if (second != std::string::npos) {
                                size_t third = params.find(' ', second + 1);
                                if (third != std::string::npos) {
                                    std::string channel = params.substr(first + 1, second - first - 1);
                                    std::string users = params.substr(second + 1, third - second - 1);
                                    size_t colon = params.find(':', third);
                                    std::string topic = (colon != std::string::npos) ? params.substr(colon + 1) : "";
                                    controller_->onServerMessage(channel + " (" + users + " users) – " + topic);
                                    break;
                                }
                            }
                        }
                        controller_->onServerMessage("(LIST) " + msg);
                    }
                    break;

                case 323: // RPL_LISTEND
                    if (controller_) controller_->onServerMessage("End of channel list.");
                    break;
    
                case 331:   // RPL_NOTOPIC  (format: "yournick #channel :No topic is set")
                {
                    // Split params by spaces
                    std::vector<std::string> tokens;
                    std::stringstream ss(params);
                    std::string token;
                    while (ss >> token) tokens.push_back(token);
                    
                    if (tokens.size() >= 2) {
                        std::string channel = tokens[1];          // second token is the real channel
                        controller_->onServerMessage("No topic set for " + channel);
                    }
                }
                break;

                case 332:   // RPL_TOPIC   (format: "yournick #channel :the actual topic")
                {
                    size_t firstSpace = params.find(' ');
                    if (firstSpace == std::string::npos) break;
                    
                    size_t secondSpace = params.find(' ', firstSpace + 1);
                    std::string channel;
                    if (secondSpace != std::string::npos) {
                        channel = params.substr(firstSpace + 1, secondSpace - firstSpace - 1);
                    } else {
                        // fallback – unlikely
                        channel = params.substr(firstSpace + 1);
                    }
                    
                    size_t colon = params.find(':');
                    std::string topic = (colon != std::string::npos) ? params.substr(colon + 1) : "";
                    
                    controller_->onServerMessage("Topic of " + channel + ": " + topic);
                }
                break;

                case 333:   // RPL_TOPICWHOTIME (format: "yournick #channel who time")
                {
                    std::vector<std::string> tokens;
                    std::stringstream ss(params);
                    std::string token;
                    while (ss >> token) tokens.push_back(token);
                    
                    if (tokens.size() >= 4) {
                        std::string channel = tokens[1];
                        std::string who = tokens[2];
                        std::string timeStr = tokens[3];
                        controller_->onServerMessage("Topic set by " + who + " at " + timeStr);
                    }
                }
                break;

                default:
                    if (controller_) {
                        std::string fullMsg = "[" + cmd + "] " + msg;
                        controller_->onServerMessage(fullMsg);
                    }
                    break;
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

        // Check for CTCP
        std::string ctcpCmd, ctcpArgs;
        if (parseCtcpMessage(msg, ctcpCmd, ctcpArgs)) {
            if (ctcpCmd == "ACTION") {
                // ACTION is already handled as /me
                if (controller_) controller_->onAction(target, nick, ctcpArgs);
            } else {
                // Other CTCP request: inform controller
                if (controller_) controller_->onCtcpRequest(nick, target, ctcpCmd, ctcpArgs);
            }
        } else {
            // Normal message
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

void IRCModel::setCurrentChannel(const std::string& channel) {
    currentChannel_ = channel;
    nicks_.clear();
}

void IRCModel::sendWhois(const std::string& nick) {
    sendRaw("WHOIS " + sanitizeMessage(nick));
}

void IRCModel::sendTopic(const std::string& channel, const std::string& topic) {
    if (topic.empty())
        sendRaw("TOPIC " + channel);
    else
        sendRaw("TOPIC " + channel + " :" + sanitizeMessage(topic));
}