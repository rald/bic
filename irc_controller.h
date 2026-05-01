#ifndef IRC_CONTROLLER_H
#define IRC_CONTROLLER_H

#include <string>
#include <vector>
#include <FL/Fl_Widget.H>

class IRCModel;
class IRCView;

class IRCController {
public:
    IRCController();
    ~IRCController();                           // FIX: destructor
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

private:
    void executeCommand(const std::string& cmdLine);
    void updateNickCompletionList();

    static void sendCallbackStatic(Fl_Widget* w, void* data);

    IRCModel* model_;
    IRCView* view_;

    std::vector<std::string> history_;
    int historyPos_;
    std::string savedInput_;
    std::string completionPrefix_;
    int completionIndex_;
};

#endif