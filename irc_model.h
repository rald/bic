#ifndef IRC_MODEL_H
#define IRC_MODEL_H

#include <string>
#include <vector>

class IRCController;

class IRCModel {
public:
    IRCModel();
    ~IRCModel();

    void setController(IRCController* ctrl);

    void connectToServer(const std::string& server, int port, const std::string& nick);
    void disconnect(const std::string& quitmsg = "");
    bool isConnected() const;
    bool isConnecting() const;

    void sendRaw(const std::string& msg);
    void sendPrivmsg(const std::string& target, const std::string& msg);
    void sendAction(const std::string& target, const std::string& action);
    void joinChannel(const std::string& channel, const std::string& key = "");
    void partChannel(const std::string& channel, const std::string& reason = "");
    void changeNick(const std::string& newnick);
    void requestNames(const std::string& channel);

    std::string getCurrentChannel() const;
    std::string getNickname() const;
    std::string getDefaultTarget() const;
    void setDefaultTarget(const std::string& target);
    const std::vector<std::string>& getNicks() const;

    void onSocketReady();

    // inside class IRCModel, public section
    void setCurrentChannel(const std::string& channel);

    void sendWhois(const std::string& nick);

private:
    void processLine(const std::string& line);
    void extractNickFromPrefix(const std::string& prefix, std::string& nick) const;
    void startNonBlockingConnect(int sockfd);
    void feedRecvData(const char* buf, int len);

    static void connectTimeoutCallback(void* data);   // static timeout handler

    int sockfd_;
    bool connectAttempt_;
    std::string nickname_;
    std::string currentChannel_;
    std::string defaultTarget_;
    std::vector<std::string> nicks_;
    std::string lineBuffer_;

    IRCController* controller_;
};

#endif