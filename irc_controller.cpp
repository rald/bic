#include "irc_controller.h"
#include "irc_model.h"
#include "irc_view.h"
#include <FL/Fl.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <strings.h>   // for strncasecmp (POSIX)
#include <ctime>
#include <unordered_map>

IRCController::IRCController()
    : model_(new IRCModel())
    , view_(new IRCView())
    , historyPos_(-1)
    , completionIndex_(0)
    , showNamesOutput_(false)
{
    model_->setController(this);

    view_->setSendCallback(&IRCController::sendCallbackStatic, this);

    view_->setTabCompletionCallback([this](const std::string& text, int cursor, std::string& newText, int& newPos) {
        return performCompletion(text, cursor, newText, newPos);
    });

    view_->setHistoryCallbacks(
        [this]() -> std::vector<std::string>& { return history_; },
        [this](int pos) { historyPos_ = pos; },
        [this]() -> int { return historyPos_; },
        [this](const std::string& s) { savedInput_ = s; },
        [this]() -> std::string { return savedInput_; }
    );

#if FLTK_MAJOR_VERSION > 1 || (FLTK_MAJOR_VERSION == 1 && FLTK_MINOR_VERSION >= 3)
    view_->setLogScrollCallback([this](bool up) {
        Fl_Scrollbar* sb = view_->logDisplay()->scrollbar();
        if (sb) {
            if (up)
                sb->value(sb->value() - sb->linesize());
            else
                sb->value(sb->value() + sb->linesize());
            view_->logDisplay()->redraw();
        }
    });
#else
    view_->setLogScrollCallback([](bool) {});
#endif

    view_->setEscQuitCallback([this]() {
        int choice = fl_choice("Quit BIC IRC Client?", "No", "Yes", NULL);
        if (choice == 1) {
            model_->disconnect("Leaving");
            view_->appendMessage("*** Goodbye!");
            Fl::check();
            exit(0);
        }
    });
}

IRCController::~IRCController() {
    delete model_;
    delete view_;
}

void IRCController::run() {
    view_->appendMessage("*** BIC IRC Client started. Type /help for commands.");
    view_->appendMessage("*** Tab completes nicknames in the current channel.");
    view_->show();
    Fl::run();
}

void IRCController::sendCallbackStatic(Fl_Widget*, void* data) {
    IRCController* self = static_cast<IRCController*>(data);
    self->onSendCommand(self->view_->getInputText());
}

void IRCController::onSendCommand(const std::string& input) {
    if (input.empty()) return;

    if (history_.empty() || history_.back() != input) {
        history_.push_back(input);
        if (history_.size() > 100) history_.erase(history_.begin());
    }
    historyPos_ = -1;
    savedInput_.clear();

    if (input[0] == '/') {
        executeCommand(input);
    } else {
        if (!model_->isConnected() || model_->isConnecting()) {
            view_->appendMessage("*** Not connected. Use /connect first.");
        } else {
            std::string target = model_->getDefaultTarget();
            if (target.empty()) target = model_->getCurrentChannel();
            if (target.empty()) {
                view_->appendMessage("*** No target set. Use /join, /target, or /msg.");
            } else {
                model_->sendPrivmsg(target, input);
                if (target[0] == '#') {
                    view_->appendMessage("[" + target + "] <" + model_->getNickname() + "> " + input);
                } else {
                    view_->appendMessage("[PM] " + model_->getNickname() + " -> " + target + ": " + input);
                }
            }
        }
    }
    view_->setInputText("");
    view_->setInputFocus();
}

void IRCController::executeCommand(const std::string& cmdLine) {
    std::string cmd, arg1, arg2, arg3;
    size_t pos = 1;
    while (pos < cmdLine.size() && cmdLine[pos] != ' ') cmd += cmdLine[pos++];
    while (pos < cmdLine.size() && cmdLine[pos] == ' ') pos++;
    std::string rest = cmdLine.substr(pos);

    auto split = [](const std::string& s, std::string& a, std::string& b, std::string& c) {
        size_t p1 = s.find(' ');
        if (p1 == std::string::npos) { a = s; return; }
        a = s.substr(0, p1);
        size_t p2 = s.find(' ', p1+1);
        if (p2 == std::string::npos) { b = s.substr(p1+1); return; }
        b = s.substr(p1+1, p2-p1-1);
        c = s.substr(p2+1);
    };
    split(rest, arg1, arg2, arg3);
    if (cmd == "connect") {
        if (arg1.empty() || arg2.empty() || arg3.empty()) {
            view_->appendMessage("Usage: /connect <server> <port> <nick>");
            return;
        }
        int port;
        try {
            port = std::stoi(arg2);
        } catch (...) {
            view_->appendMessage("Invalid port number");
            return;
        }
        if (port <= 0 || port > 65535) {
            view_->appendMessage("Invalid port number");
            return;
        }
        model_->connectToServer(arg1, port, arg3);
    }
    else if (cmd == "join") {
        if (arg1.empty()) {
            view_->appendMessage("Usage: /join <channel> [key]");
            return;
        }
        model_->joinChannel(arg1, arg2);
    }
    else if (cmd == "part") {
        std::string chan = arg1.empty() ? model_->getCurrentChannel() : arg1;
        if (chan.empty()) { view_->appendMessage("No channel specified and not in any channel."); return; }
        model_->partChannel(chan, arg2);
    }
    else if (cmd == "target") {
        if (arg1.empty()) {
            std::string cur = model_->getDefaultTarget();
            if (cur.empty()) view_->appendMessage("*** No default target set.");
            else view_->appendMessage("*** Default target: " + cur);
        } else {
            model_->setDefaultTarget(arg1);
            if (arg1[0] == '#') {
                model_->setCurrentChannel(arg1);
                if (model_->isConnected())
                    model_->requestNames(arg1);
                view_->appendMessage("*** Default target set to " + arg1 +
                                     (model_->isConnected() ? " (fetching nicknames)" : ""));
            } else {
                model_->setCurrentChannel("");
                view_->appendMessage("*** Default target set to " + arg1 +
                                     " (nick completion disabled for PM)");
            }
        }
    }
    else if (cmd == "me") {
        if (!model_->isConnected()) {
            view_->appendMessage("*** Not connected.");
            return;
        }
        if (rest.empty()) {
            view_->appendMessage("Usage: /me <target> <action>");
            return;
        }

        std::string target, action;
        size_t firstSpace = rest.find(' ');
        if (firstSpace == std::string::npos) {
            view_->appendMessage("Usage: /me <target> <action>");
            return;
        }

        target = rest.substr(0, firstSpace);
        action = rest.substr(firstSpace + 1);

        // Trim leading/trailing spaces from action
        size_t start = action.find_first_not_of(" \t");
        if (start != std::string::npos)
            action = action.substr(start);
        else
            action.clear();

        if (target.empty() || action.empty()) {
            view_->appendMessage("Usage: /me <target> <action>");
            return;
        }

        model_->sendAction(target, action);

        // Display the action locally – always show target
        if (target[0] == '#')
            view_->appendMessage("* " + model_->getNickname() + " " + action + " (in " + target + ")");
        else
            view_->appendMessage("* " + model_->getNickname() + " " + action + " (to " + target + ")");
    }
	else if (cmd == "names") {
		if (arg1.empty()) { view_->appendMessage("Usage: /names <channel>"); return; }
		showNamesOutput_ = true;          // <-- new
		model_->requestNames(arg1);
	}
    else if (cmd == "list") {
        if (!model_->isConnected()) { view_->appendMessage("*** Not connected."); return; }
        model_->sendRaw("LIST" + (arg1.empty() ? "" : " " + arg1));
    }
    else if (cmd == "nick") {
        if (arg1.empty()) { view_->appendMessage("Usage: /nick <newnick>"); return; }
        model_->changeNick(arg1);
    }
    else if (cmd == "msg") {
        if (arg1.empty() || arg2.empty()) {
            view_->appendMessage("Usage: /msg <target> <message>");
            return;
        }
        size_t msgStart = rest.find_first_not_of(' ', arg1.size());
        std::string message = (msgStart != std::string::npos) ? rest.substr(msgStart) : "";
        model_->sendPrivmsg(arg1, message);
        if (arg1[0] == '#') {
            view_->appendMessage("[" + arg1 + "] <" + model_->getNickname() + "> " + message);
        } else {
            view_->appendMessage("[PM] " + model_->getNickname() + " -> " + arg1 + ": " + message);
        }
    }
    else if (cmd == "whois") {
        if (arg1.empty()) {
            view_->appendMessage("Usage: /whois <nick>");
            return;
        }
        if (!model_->isConnected()) {
            view_->appendMessage("*** Not connected.");
            return;
        }
        model_->sendWhois(arg1);
    }
    else if (cmd == "ctcp") {
        // /ctcp <target> <command> [arguments]
        if (arg1.empty() || arg2.empty()) {
            view_->appendMessage("Usage: /ctcp <target> <command> [arguments]");
            return;
        }
        if (!model_->isConnected()) {
            view_->appendMessage("*** Not connected.");
            return;
        }
        // The remaining text after <target> and <command> are arguments
        size_t cmdStart = rest.find_first_not_of(' ', arg1.size());
        size_t argsStart = rest.find_first_not_of(' ', cmdStart + arg2.size() + 1);
        std::string args = (argsStart != std::string::npos) ? rest.substr(argsStart) : "";
        model_->sendCtcp(arg1, arg2, args);
        view_->appendMessage("[CTCP] Sent " + arg2 + " to " + arg1);
    }
    else if (cmd == "clear") {
        view_->clearDisplay();
        view_->appendMessage("*** Display cleared");
    }
    else if (cmd == "disconnect" || cmd == "quit") {
        model_->disconnect(arg1);
    }
    else if (cmd == "topic") {
        if (!model_->isConnected()) {
            view_->appendMessage("*** Not connected.");
            return;
        }

        std::string channel, newTopic;
        size_t firstSpace = rest.find(' ');

        // If no arguments, use current channel (if any)
        if (rest.empty()) {
            channel = model_->getCurrentChannel();
            if (channel.empty()) {
                view_->appendMessage("*** No current channel. Use /topic #channel or /target first.");
                return;
            }
            model_->sendTopic(channel, "");   // query only
        }
        else {
            // Check if the first word looks like a channel
            std::string firstWord = rest.substr(0, firstSpace);
            if (firstWord[0] == '#') {
                channel = firstWord;
                if (firstSpace != std::string::npos && firstSpace + 1 < rest.size()) {
                    newTopic = rest.substr(firstSpace + 1);
                }
            } else {
                // No channel given – use current channel
                channel = model_->getCurrentChannel();
                if (channel.empty()) {
                    view_->appendMessage("*** No current channel. Use /topic #channel or /target first.");
                    return;
                }
                newTopic = rest;   // entire rest is the new topic
            }
            model_->sendTopic(channel, newTopic);
        }
    }
    else if (cmd == "mode") {
        if (arg1.empty()) {
            view_->appendMessage("Usage: /mode <target> [modestring [parameters...]]");
            return;
        }
        if (!model_->isConnected()) {
            view_->appendMessage("*** Not connected.");
            return;
        }
        // Capture everything after the target (the mode string and parameters)
        std::string modeArgs;
        size_t firstSpace = rest.find(' ');
        if (firstSpace != std::string::npos) {
            modeArgs = rest.substr(firstSpace + 1);
        }
        model_->sendMode(arg1, modeArgs);
    }
    else if (cmd == "ignore") {
        if (arg1.empty()) {
            // List ignored nicks
            if (ignoredNicks_.empty()) {
                view_->appendMessage("*** No ignored nicks.");
            } else {
                std::string list;
                for (const auto& pair : ignoredNicks_) {
                    if (!list.empty()) list += ", ";
                    list += pair.second;   // original case
                }
                view_->appendMessage("*** Ignored nicks: " + list);
            }
        } else {
            addIgnoredNick(arg1);
            view_->appendMessage("*** Now ignoring " + arg1);
        }
    }
    else if (cmd == "unignore") {
        if (arg1.empty()) {
            view_->appendMessage("Usage: /unignore <nick>");
        } else {
            removeIgnoredNick(arg1);
            view_->appendMessage("*** No longer ignoring " + arg1);
        }
    }
    else if (cmd == "help") {
        view_->appendMessage("--- BIC IRC Client Commands ---");
        view_->appendMessage("/connect <server> <port> <nick>  -- connect to IRC server");
        view_->appendMessage("/join <channel> [key]            -- join a channel (with optional key)");
        view_->appendMessage("/part [channel] [reason]         -- leave channel (default: current)");
        view_->appendMessage("/target [target]                 -- set default target for non‑command lines");
        view_->appendMessage("/names <channel>                 -- list nicks in a channel");
        view_->appendMessage("/list [pattern]                  -- list channels");
        view_->appendMessage("/nick <newnick>                  -- change your nickname");
        view_->appendMessage("/msg <target> <text>             -- send private message");
        view_->appendMessage("/me <target> <action>            -- send CTCP ACTION to a channel or nickname");
        view_->appendMessage("/whois <nick>                    -- get information about a user");
        view_->appendMessage("/mode <target> [modestring [params]]  -- view or change channel/user modes");
        view_->appendMessage("/ctcp <target> <command> [args]  -- send a CTCP request (e.g. VERSION, PING, TIME)");
        view_->appendMessage("/topic [#channel] [new topic]    -- view or set the channel topic");
        view_->appendMessage("/clear                           -- clear chat display");
        view_->appendMessage("/ignore [nick]                   -- ignore a user (or list ignored)");
        view_->appendMessage("/unignore <nick>                 -- remove a user from ignore list");
        view_->appendMessage("/disconnect [message]            -- disconnect from server");
        view_->appendMessage("/quit [message]                  -- alias for disconnect");
        view_->appendMessage("/help                            -- this help");
    }
    else {
        view_->appendMessage("Unknown command: /" + cmd + ". Type /help");
    }
}

bool IRCController::performCompletion(const std::string& text, int cursorPos, std::string& newText, int& newPos) {
    // Find the start of the word to complete
    int start = cursorPos;
    while (start > 0 && text[start-1] != ' ') start--;
    std::string prefix = text.substr(start, cursorPos - start);
    if (prefix.empty()) return false;

    // --- Channel completion (word starts with '#') ---
    if (prefix[0] == '#') {
        const auto& channels = model_->getJoinedChannels();
        if (channels.empty()) return false;

        // Prepare for cycling through matches
        if (prefix != completionPrefix_) {
            completionPrefix_ = prefix;
            completionIndex_ = 0;
        }

        std::vector<std::string> matches;
        for (const auto& chan : channels) {
            if (chan.size() >= prefix.size() &&
                strncasecmp(chan.c_str(), prefix.c_str(), prefix.size()) == 0) {
                matches.push_back(chan);
            }
        }
        if (matches.empty()) return false;
        if (completionIndex_ >= (int)matches.size()) completionIndex_ = 0;
        std::string newWord = matches[completionIndex_++];
        newText = text.substr(0, start) + newWord + text.substr(cursorPos);
        newPos = start + (int)newWord.size();
        return true;
    }

    // --- Nick completion (original behaviour) ---
    if (model_->getCurrentChannel().empty() || model_->getNicks().empty()) return false;

    if (prefix != completionPrefix_) {
        completionPrefix_ = prefix;
        completionIndex_ = 0;
    }
    std::vector<std::string> matches;
    for (const auto& nick : model_->getNicks()) {
        if (nick.size() >= prefix.size() &&
            strncasecmp(nick.c_str(), prefix.c_str(), prefix.size()) == 0) {
            matches.push_back(nick);
        }
    }
    if (matches.empty()) return false;
    if (completionIndex_ >= (int)matches.size()) completionIndex_ = 0;
    std::string newWord = matches[completionIndex_++];
    newText = text.substr(0, start) + newWord + text.substr(cursorPos);
    newPos = start + (int)newWord.size();
    return true;
}

void IRCController::updateNickCompletionList() {
    completionPrefix_.clear();
    completionIndex_ = 0;
}

std::vector<std::string>& IRCController::getHistory() {
    return history_;
}

int IRCController::getHistoryPos() const {
    return historyPos_;
}

void IRCController::setHistoryPos(int pos) {
    historyPos_ = pos;
}

std::string IRCController::getSavedInput() const {
    return savedInput_;
}

void IRCController::setSavedInput(const std::string& s) {
    savedInput_ = s;
}

// Model callbacks
void IRCController::onConnected() {
    view_->appendMessage("*** Connected to server.");
}

void IRCController::onConnectionFailed(const std::string& reason) {
    view_->appendMessage("*** Connection failed: " + reason);
}

void IRCController::onDisconnected() {
    view_->appendMessage("*** Disconnected.");
    updateNickCompletionList();
    showNamesOutput_ = false;
}

void IRCController::onJoin(const std::string& nick, const std::string& channel) {
    view_->appendMessage("*** " + nick + " has joined " + channel);
    if (nick == model_->getNickname()) {
        updateNickCompletionList();
        // Removed automatic default target setting:
        // model_->setDefaultTarget(channel);
        // view_->appendMessage("*** Default target set to " + channel);
    }
}

void IRCController::onPart(const std::string& nick, const std::string& channel, const std::string& reason) {
    if (reason.empty())
        view_->appendMessage("*** " + nick + " has left " + channel);
    else
        view_->appendMessage("*** " + nick + " has left " + channel + " (" + reason + ")");
}

void IRCController::onQuit(const std::string& nick, const std::string& reason) {
    if (reason.empty())
        view_->appendMessage("*** " + nick + " has quit");
    else
        view_->appendMessage("*** " + nick + " has quit (" + reason + ")");
}

void IRCController::onNickChange(const std::string& oldNick, const std::string& newNick) {
    view_->appendMessage("*** " + oldNick + " is now known as " + newNick);
}

void IRCController::onNickList(const std::vector<std::string>& nicks) {
    (void)nicks;
    updateNickCompletionList();
}

void IRCController::onChannelMessage(const std::string& target, const std::string& nick, const std::string& msg) {
    if (isIgnored(nick)) return;
    view_->appendMessage("[" + target + "] <" + nick + "> " + msg);
}

void IRCController::onPmMessage(const std::string& nick, const std::string& msg) {
    if (isIgnored(nick)) return;
    view_->appendMessage("[PM] " + model_->getNickname() + " <- " + nick + ": " + msg);
}

void IRCController::onAction(const std::string& target, const std::string& nick, const std::string& action) {
    if (isIgnored(nick)) return;
    if (target[0] == '#')
        view_->appendMessage("* " + nick + " " + action + " (in " + target + ")");
    else
        view_->appendMessage("* " + nick + " " + action);
}

void IRCController::onRawLine(const std::string& line) {
    (void)line;
}

void IRCController::onError(const std::string& error) {
    view_->appendMessage("*** ERROR: " + error);
}

// Helper: case‑insensitive search in a vector of strings
static bool caseInsensitiveFind(const std::vector<std::string>& vec, const std::string& target) {
    return std::find_if(vec.begin(), vec.end(),
        [&](const std::string& s) {
            return s.size() == target.size() &&
                   std::equal(s.begin(), s.end(), target.begin(),
                       [](char a, char b) { return std::tolower(a) == std::tolower(b); });
        }) != vec.end();
}

void IRCController::onNamesComplete(const std::string& channel, const std::vector<std::string>& nicks) {
    if (!showNamesOutput_) return;   // only print when explicitly requested

    // (original code for displaying the nicklist)
    std::vector<std::string> allNicks = nicks;
    std::string myNick = model_->getNickname();
    if (!myNick.empty() && !caseInsensitiveFind(allNicks, myNick)) {
        allNicks.push_back(myNick);
    }
    std::sort(allNicks.begin(), allNicks.end());

    if (allNicks.empty()) {
        view_->appendMessage("*** No users in " + channel);
    } else {
        std::string nicklist;
        for (size_t i = 0; i < allNicks.size(); ++i) {
            if (i > 0) nicklist += ", ";
            nicklist += allNicks[i];
        }
        view_->appendMessage("*** Users in " + channel + ": " + nicklist);
    }

    showNamesOutput_ = false;          // reset flag after displaying
}
void IRCController::onMotdEnd() {
    view_->appendMessage("*** You can now join channels (e.g., /join #channel)");
}

void IRCController::onServerMessage(const std::string& msg) {
    view_->appendMessage("*** " + msg);
}

void IRCController::onNotice(const std::string& from, const std::string& msg) {
    if (isIgnored(from)) return;
    view_->appendMessage("[NOTICE from " + from + "] " + msg);
}

void IRCController::onCtcpRequest(const std::string& from, const std::string& /*target*/, const std::string& command, const std::string& args) {
    if (isIgnored(from)) return;
    view_->appendMessage("[CTCP] Request from " + from + ": " + command + (args.empty() ? "" : " " + args));
    // Auto-reply to common CTCP commands
    sendCtcpReplyAuto(from, command, args);
}

void IRCController::onCtcpReply(const std::string& from, const std::string& command, const std::string& args) {
    if (isIgnored(from)) return;
    view_->appendMessage("[CTCP reply from " + from + "] " + command + (args.empty() ? "" : ": " + args));
}

void IRCController::sendCtcpReplyAuto(const std::string& target, const std::string& command, const std::string& args) {
    // We only reply to certain CTCP requests, and only if we are connected.
    if (!model_->isConnected()) return;

    std::string cmd = command;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

    if (cmd == "VERSION") {
        model_->sendCtcpReply(target, "VERSION", "BIC IRC Client v1.0 / FLTK");
    }
    else if (cmd == "TIME") {
        time_t now = time(nullptr);
        std::string timestr = ctime(&now);
        timestr.pop_back(); // remove newline
        model_->sendCtcpReply(target, "TIME", timestr);
    }
    else if (cmd == "USERINFO") {
        model_->sendCtcpReply(target, "USERINFO", "BIC IRC Client user");
    }
    else if (cmd == "CLIENTINFO") {
        model_->sendCtcpReply(target, "CLIENTINFO", "VERSION TIME USERINFO CLIENTINFO PING");
    }
    else if (cmd == "PING") {
        // Reply with the same argument (ping value)
        model_->sendCtcpReply(target, "PING", args);
    }
    // For other CTCP commands, we ignore.
}

// ----- ignore list helpers -----
std::string IRCController::toLower(const std::string& s) const {
    std::string lc;
    lc.reserve(s.size());
    std::transform(s.begin(), s.end(), std::back_inserter(lc), ::tolower);
    return lc;
}

bool IRCController::isIgnored(const std::string& nick) const {
    return ignoredNicks_.find(toLower(nick)) != ignoredNicks_.end();
}

void IRCController::addIgnoredNick(const std::string& nick) {
    ignoredNicks_[toLower(nick)] = nick;
}

void IRCController::removeIgnoredNick(const std::string& nick) {
    ignoredNicks_.erase(toLower(nick));
}