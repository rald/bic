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

IRCController::IRCController()
    : model_(new IRCModel())
    , view_(new IRCView())
    , historyPos_(-1)
    , completionIndex_(0)
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

// Conditional scrollbar support for FLTK 1.3+
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
    // No-op for FLTK 1.1 (scrollbar() method not available)
    view_->setLogScrollCallback([](bool) {});
#endif

    view_->setEscQuitCallback([this]() {
        int choice = fl_choice("Quit BIC IRC Client?", "No", "Yes", NULL);
        if (choice == 1) {   // "Yes" button (index 1)
            model_->disconnect("Leaving");
            view_->appendMessage("*** Goodbye!");
            Fl::check();      // let the message appear
            exit(0);
        }
});
}

void IRCController::run() {
    view_->appendMessage("*** BIC IRC Client started. Type /help for commands.");
    view_->appendMessage("*** Tab completes nicknames in the current channel.");
    view_->show();
    Fl::run();
}

void IRCController::sendCallbackStatic(Fl_Widget*, void* data) {
    IRCController* self = static_cast<IRCController*>(data);
    // DEBUG: Uncomment to verify callback is triggered
    // self->view_->appendMessage("*** DEBUG: Send callback triggered");
    self->onSendCommand(self->view_->getInputText());
}

void IRCController::onSendCommand(const std::string& input) {
    // view_->appendMessage("*** DEBUG: onSendCommand called with: '" + input + "'");  // DEBUG
    if (input.empty()) return;

    history_.push_back(input);
    if (history_.size() > 100) history_.erase(history_.begin());
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
        view_->appendMessage("*** DEBUG: Parsing connect command");  // DEBUG
        if (arg1.empty() || arg2.empty() || arg3.empty()) {
            view_->appendMessage("Usage: /connect <server> <port> <nick>");
            return;
        }
        int port = atoi(arg2.c_str());
        if (port <= 0 || port > 65535) {
            view_->appendMessage("Invalid port number");
            return;
        }
        view_->appendMessage("*** DEBUG: Connecting to " + arg1 + ":" + arg2 + " as " + arg3);
        model_->connectToServer(arg1, port, arg3);
    }
    else if (cmd == "join") {
        if (arg1.empty()) {
            view_->appendMessage("Usage: /join <channel> [key]");
            return;
        }
        std::string channel = arg1;
        std::string key = arg2;   // optional key, already split
        model_->joinChannel(channel, key);
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
            view_->appendMessage("*** Default target set to " + arg1);
        }
    }
    else if (cmd == "me") {
        if (!model_->isConnected()) { view_->appendMessage("*** Not connected."); return; }
        std::string target = model_->getDefaultTarget();
        if (target.empty()) target = model_->getCurrentChannel();
        if (target.empty()) { view_->appendMessage("*** No target set."); return; }
        if (arg1.empty()) { view_->appendMessage("Usage: /me <action>"); return; }
        std::string action = rest;
        model_->sendAction(target, action);
        view_->appendMessage("* " + model_->getNickname() + " " + action);
    }
    else if (cmd == "names") {
        if (arg1.empty()) { view_->appendMessage("Usage: /names <channel>"); return; }
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
        std::string message = rest.substr(arg1.size()+1);
        model_->sendPrivmsg(arg1, message);
        if (arg1[0] == '#') {
            view_->appendMessage("[" + arg1 + "] <" + model_->getNickname() + "> " + message);
        } else {
            // Outgoing PM: MyNick -> Target: message
            view_->appendMessage("[PM] " + model_->getNickname() + " -> " + arg1 + ": " + message);
        }
    }    
    else if (cmd == "clear") {
        view_->clearDisplay();
        view_->appendMessage("*** Display cleared");
    }
    else if (cmd == "disconnect" || cmd == "quit") {
        model_->disconnect(arg1);
    }
    else if (cmd == "help") {
        view_->appendMessage("--- BIC IRC Client Commands ---");
        view_->appendMessage("/connect <server> <port> <nick>   -- connect to IRC server");
        view_->appendMessage("/join <channel> [key]            -- join a channel (with optional key)");
        view_->appendMessage("/part [channel] [reason]         -- leave channel (default: current)");
        view_->appendMessage("/target [target]                 -- set default target for non‑command lines");
        view_->appendMessage("/names <channel>                 -- list nicks in a channel");
        view_->appendMessage("/list [pattern]                  -- list channels");
        view_->appendMessage("/nick <newnick>                  -- change your nickname");
        view_->appendMessage("/msg <target> <text>             -- send private message");
        view_->appendMessage("/me <action>                     -- send CTCP ACTION");
        view_->appendMessage("/clear                           -- clear chat display");
        view_->appendMessage("/disconnect [message]            -- disconnect from server");
        view_->appendMessage("/quit [message]                  -- alias for disconnect");
        view_->appendMessage("/help                            -- this help");
        view_->appendMessage("---");
        view_->appendMessage("Any line not starting with '/' is sent to default target or current channel.");
        view_->appendMessage("Press Up/Down arrows for history, Tab completes nicknames.");
    }
    else {
        view_->appendMessage("Unknown command: /" + cmd + ". Type /help");
    }
}

bool IRCController::performCompletion(const std::string& text, int cursorPos, std::string& newText, int& newPos) {
    if (model_->getCurrentChannel().empty() || model_->getNicks().empty()) return false;
    int start = cursorPos;
    while (start > 0 && text[start-1] != ' ') start--;
    std::string prefix = text.substr(start, cursorPos - start);
    if (prefix.empty()) return false;

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
    completionPrefix_.clear();
    completionIndex_ = 0;
}

void IRCController::onJoin(const std::string& nick, const std::string& channel) {
    view_->appendMessage("*** " + nick + " has joined " + channel);
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
    view_->appendMessage("[" + target + "] <" + nick + "> " + msg);
}

void IRCController::onPmMessage(const std::string& nick, const std::string& msg) {
    view_->appendMessage("[PM] " + model_->getNickname() + " <- " + nick + ": " + msg);
}

void IRCController::onAction(const std::string& target, const std::string& nick, const std::string& action) {
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

void IRCController::onNamesComplete(const std::string& channel, const std::vector<std::string>& nicks) {
    std::vector<std::string> allNicks = nicks;
    
    // Ensure own nickname is included
    std::string myNick = model_->getNickname();
    if (!myNick.empty() && std::find(allNicks.begin(), allNicks.end(), myNick) == allNicks.end()) {
        allNicks.push_back(myNick);
    }

    // Optional: sort alphabetically
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
}

void IRCController::onMotdEnd() {
    view_->appendMessage("*** You can now join channels (e.g., /join #channel)");
}

void IRCController::onServerMessage(const std::string& msg) {
    view_->appendMessage("*** " + msg);
}
