#ifndef IRC_VIEW_H
#define IRC_VIEW_H

#include <FL/Fl_Input.H>
#include <functional>
#include <string>
#include <vector>

class Fl_Window;
class Fl_Text_Display;
class Fl_Text_Buffer;
class Fl_Button;

class HistoryInput : public Fl_Input {
public:
    HistoryInput(int x, int y, int w, int h, const char* l = nullptr);
    void setCompletionCallback(std::function<bool(const std::string&, int, std::string&, int&)> cb);
    void setHistoryCallback(std::function<std::vector<std::string>&()> getHistory,
                            std::function<void(int)> setHistoryPos,
                            std::function<int()> getHistoryPos,
                            std::function<void(const std::string&)> setSavedInput,
                            std::function<std::string()> getSavedInput);
    void setLogScrollCallback(std::function<void(bool)> scrollPage);
    void setEscQuitCallback(std::function<void()> cb);
    int handle(int event) override;

private:
    std::function<bool(const std::string&, int, std::string&, int&)> completionCallback_;
    std::function<std::vector<std::string>&()> getHistory_;
    std::function<void(int)> setHistoryPos_;
    std::function<int()> getHistoryPos_;
    std::function<void(const std::string&)> setSavedInput_;
    std::function<std::string()> getSavedInput_;
    std::function<void(bool)> scrollPage_;
    std::function<void()> escQuitCallback_;
};

class IRCView {
public:
    IRCView();
    ~IRCView();     // FIX: added destructor

    void show();
    void appendMessage(const std::string& msg);
    void clearDisplay();
    void setInputFocus();
    std::string getInputText() const;
    void setInputText(const std::string& txt);
    void setSendCallback(void (*cb)(Fl_Widget*, void*), void* data);
    void setTabCompletionCallback(std::function<bool(const std::string&, int, std::string&, int&)> cb);
    void setHistoryCallbacks(std::function<std::vector<std::string>&()> getHistory,
                             std::function<void(int)> setHistoryPos,
                             std::function<int()> getHistoryPos,
                             std::function<void(const std::string&)> setSavedInput,
                             std::function<std::string()> getSavedInput);
    void setLogScrollCallback(std::function<void(bool)> scrollPage);
    void setEscQuitCallback(std::function<void()> cb);
    bool isAtBottom() const;
    void scrollToBottom();

    Fl_Text_Display* logDisplay() const { return logdisp_; }

private:
    Fl_Window* window_;
    Fl_Text_Display* logdisp_;
    Fl_Text_Buffer* logbuf_;
    HistoryInput* input_;
    Fl_Button* sendBtn_;
};

#endif