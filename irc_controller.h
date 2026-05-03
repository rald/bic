#ifndef IRC_CONTROLLER_H
#define IRC_CONTROLLER_H

#include <string>
#include <vector>
#include <unordered_map>        // for ignore list
#include <cctype>               // for tolower
#include <FL/Fl_Widget.H>

class IRCModel;
class IRCView;

class IRCController {
public:
    IRCController();
    ~IRCController();
    void run();

    void onSendCommand(const std::string& input);

    std::vector<std::string>& getHistory();
    int getHistoryPos() const;
    void setHistoryPos(int pos);
    std::string getSavedInput() const;
    void setSavedInput(const std::string& s);
    bool performCompletion(const std::string& text, int cursorPos, std::string& newText, int& newPos);

    // Model callbacks
    void onConnected();
    void onConnectionFailed(const std::string& reason);
    void onDisconnected();
    void onJoin(const std::string& nick, const std::string& channel);
    void onPart(const std::string& nick, const std::string& channel, const std::string& reason);
    void onQuit(const std::string& nick, const std::string& reason);
    void onNickChange(const std::string& oldNick, const std::string& newNick);
    void onNickList(const std::vector<std::string>& nicks);
    void onChannelMessage(const std::string& target, const std::string& nick, const std::string& msg);
    void onPmMessage(const std::string& nick, const std::string& msg);
    void onAction(const std::string& target, const std::string& nick, const std::string& action);
    void onRawLine(const std::string& line);
    void onNamesComplete(const std::string& channel, const std::vector<std::string>& nicks);
    void onError(const std::string& error);
    void onMotdEnd();
    void onServerMessage(const std::string& msg);
    void onNotice(const std::string& from, const std::string& msg);
    void onCtcpRequest(const std::string& from, const std::string& target, const std::string& command, const std::string& args);
    void onCtcpReply(const std::string& from, const std::string& command, const std::string& args);

private:
    void executeCommand(const std::string& cmdLine);
    void updateNickCompletionList();
    void sendCtcpReplyAuto(const std::string& target, const std::string& command, const std::string& args);

    static void sendCallbackStatic(Fl_Widget* w, void* data);

    // ---- ignore list support ----
    bool isIgnored(const std::string& nick) const;
    void addIgnoredNick(const std::string& nick);
    void removeIgnoredNick(const std::string& nick);
    std::string toLower(const std::string& s) const;

    IRCModel* model_;
    IRCView* view_;

    std::vector<std::string> history_;
    int historyPos_;
    std::string savedInput_;
    std::string completionPrefix_;
    int completionIndex_;
	bool showNamesOutput_; 

    std::unordered_map<std::string, std::string> ignoredNicks_;  // lower -> original
};

#endif