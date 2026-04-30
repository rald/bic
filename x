irc_model.cpp: In lambda function:
irc_model.cpp:129:49: warning: unused parameter 'fd' [-Wunused-parameter]
  129 |             Fl::add_fd(sockfd_, FL_READ, [](int fd, void* data) {
      |                                             ~~~~^~
irc_model.cpp: In lambda function:
irc_model.cpp:167:40: warning: unused parameter 'fd2' [-Wunused-parameter]
  167 |         Fl::add_fd(fd, FL_READ, [](int fd2, void* data2) {
      |                                    ~~~~^~~
irc_model.cpp: In member function 'void IRCModel::processLine(const std::string&)':
irc_model.cpp:292:16: warning: suggest explicit braces to avoid ambiguous 'else' [-Wdangling-else]
  292 |             if (target[0] == '#')
      |                ^
