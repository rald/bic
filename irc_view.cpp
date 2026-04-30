#include "irc_view.h"
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <ctime>

static std::string get_timestamp() {
    char time_str[10];
    time_t rawtime;
    struct tm* timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
    return std::string(time_str);
}

// ---------- HistoryInput ----------
HistoryInput::HistoryInput(int x, int y, int w, int h, const char* l)
    : Fl_Input(x, y, w, h, l)
{}

void HistoryInput::setCompletionCallback(std::function<bool(const std::string&, int, std::string&, int&)> cb) {
    completionCallback_ = cb;
}

void HistoryInput::setHistoryCallback(std::function<std::vector<std::string>&()> getHistory,
                                      std::function<void(int)> setHistoryPos,
                                      std::function<int()> getHistoryPos,
                                      std::function<void(const std::string&)> setSavedInput,
                                      std::function<std::string()> getSavedInput) {
    getHistory_ = getHistory;
    setHistoryPos_ = setHistoryPos;
    getHistoryPos_ = getHistoryPos;
    setSavedInput_ = setSavedInput;
    getSavedInput_ = getSavedInput;
}

void HistoryInput::setLogScrollCallback(std::function<void(bool)> scrollPage) {
    scrollPage_ = scrollPage;
}

int HistoryInput::handle(int event) {
    if (event == FL_KEYBOARD) {
        int key = Fl::event_key();
        if (key == FL_Up || key == FL_Down) {
            if (getHistory_ && setHistoryPos_ && getHistoryPos_ && setSavedInput_ && getSavedInput_) {
                auto& hist = getHistory_();
                if (hist.empty()) return 1;
                int pos = getHistoryPos_();
                if (key == FL_Up) {
                    if (pos == -1) {
                        setSavedInput_(value());
                        setHistoryPos_((int)hist.size() - 1);
                    } else if (pos > 0) {
                        setHistoryPos_(pos - 1);
                    } else {
                        return 1;
                    }
                    value(hist[getHistoryPos_()].c_str());
                } else {
                    if (pos == -1) return 1;
                    if (pos == (int)hist.size() - 1) {
                        setHistoryPos_(-1);
                        value(getSavedInput_().c_str());
                    } else {
                        setHistoryPos_(pos + 1);
                        value(hist[getHistoryPos_()].c_str());
                    }
                }
                position(strlen(value()));
                redraw();
                return 1;
            }
        }
#if FLTK_MAJOR_VERSION > 1 || (FLTK_MAJOR_VERSION == 1 && FLTK_MINOR_VERSION >= 3)
        if ((key == FL_Page_Up || key == FL_Page_Down) && scrollPage_) {
            scrollPage_(key == FL_Page_Up);
            return 1;
        }
#endif
        if (key == FL_Tab && completionCallback_) {
            std::string newText;
            int newPos;
            if (completionCallback_(value(), position(), newText, newPos)) {
                value(newText.c_str());
                position(newPos);
                return 1;
            }
            return 1;
        }
        if (key == FL_Enter || key == FL_KP_Enter) {
            if (callback()) {
                do_callback();
            }
            return 1;
        }
    }
    return Fl_Input::handle(event);
}

// ---------- IRCView ----------
IRCView::IRCView() {
    window_ = new Fl_Window(600, 450, "BIC - BIC IRC Client");
    logbuf_ = new Fl_Text_Buffer();
    logdisp_ = new Fl_Text_Display(0, 0, 600, 420);
    logdisp_->buffer(logbuf_);
    logdisp_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);

    input_ = new HistoryInput(0, 420, 520, 30, "");
    sendBtn_ = new Fl_Button(520, 420, 80, 30, "Send");

    window_->resizable(logdisp_);
    window_->end();

    // Theme
    window_->color(FL_BLACK);
    logdisp_->color(FL_BLACK);
    logdisp_->textcolor(FL_GREEN);
    logdisp_->cursor_color(FL_GREEN);
    logdisp_->selection_color(FL_DARK_GREEN);
    logdisp_->textfont(FL_COURIER);
    input_->color(FL_BLACK);
    input_->textcolor(FL_GREEN);
    input_->cursor_color(FL_GREEN);
    input_->textfont(FL_COURIER);
    // Send button uses default system color
}

IRCView::~IRCView() {
    delete window_;
}

void IRCView::show() {
    window_->show();
    // Force focus to the input field
    Fl::focus(input_);
    input_->take_focus();
}

void IRCView::appendMessage(const std::string& msg) {
    bool atBottom = isAtBottom();
    std::string line = "[" + get_timestamp() + "] " + msg + "\n";
    logbuf_->append(line.c_str());
    if (atBottom) scrollToBottom();
}

void IRCView::clearDisplay() {
    logbuf_->text("");
}

void IRCView::setInputFocus() {
    Fl::focus(input_);
    input_->take_focus();
}

std::string IRCView::getInputText() const {
    return input_->value() ? input_->value() : "";
}

void IRCView::setInputText(const std::string& txt) {
    input_->value(txt.c_str());
    input_->position((int)txt.size());
}

void IRCView::setSendCallback(void (*cb)(Fl_Widget*, void*), void* data) {
    input_->callback(cb, data);
    sendBtn_->callback(cb, data);
}

void IRCView::setTabCompletionCallback(std::function<bool(const std::string&, int, std::string&, int&)> cb) {
    input_->setCompletionCallback(cb);
}

void IRCView::setHistoryCallbacks(std::function<std::vector<std::string>&()> getHistory,
                                  std::function<void(int)> setHistoryPos,
                                  std::function<int()> getHistoryPos,
                                  std::function<void(const std::string&)> setSavedInput,
                                  std::function<std::string()> getSavedInput) {
    input_->setHistoryCallback(getHistory, setHistoryPos, getHistoryPos, setSavedInput, getSavedInput);
}

void IRCView::setLogScrollCallback(std::function<void(bool)> scrollPage) {
    input_->setLogScrollCallback(scrollPage);
}

bool IRCView::isAtBottom() const {
#if FLTK_MAJOR_VERSION > 1 || (FLTK_MAJOR_VERSION == 1 && FLTK_MINOR_VERSION >= 3)
    Fl_Scrollbar* sb = logdisp_->scrollbar();
    if (!sb) return true;
    return (sb->value() + sb->linesize() >= sb->maximum() - 0.5);
#else
    return true;
#endif
}

void IRCView::scrollToBottom() {
    logdisp_->insert_position(logbuf_->length());
    logdisp_->show_insert_position();
}