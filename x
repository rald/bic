irc_controller.cpp: In member function 'void IRCController::onNamesComplete(const std::string&, const std::vector<std::__cxx11::basic_string<char> >&)':
irc_controller.cpp:329:33: error: 'find' is not a member of 'std'; did you mean 'bind'?
  329 |     if (!myNick.empty() && std::find(allNicks.begin(), allNicks.end(), myNick) == allNicks.end()) {
      |                                 ^~~~
      |                                 bind
irc_controller.cpp:334:10: error: 'sort' is not a member of 'std'; did you mean 'qsort'?
  334 |     std::sort(allNicks.begin(), allNicks.end());
      |          ^~~~
      |          qsort
make: *** [Makefile:24: irc_controller.o] Error 1
