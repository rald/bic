irc_model.cpp: In destructor 'IRCModel::~IRCModel()':
irc_model.cpp:35:28: error: invalid conversion from 'void*' to 'Fl_Timeout_Handler' {aka 'void (*)(void*)'} [-fpermissive]
   35 |         Fl::remove_timeout(connectTimeoutHandle_);
      |                            ^~~~~~~~~~~~~~~~~~~~~
      |                            |
      |                            void*
In file included from irc_model.cpp:12:
/usr/include/FL/Fl.H:447:30: note: initializing argument 1 of 'static void Fl::remove_timeout(Fl_Timeout_Handler, void*)'
  447 |   static void remove_timeout(Fl_Timeout_Handler, void* = 0);
      |                              ^~~~~~~~~~~~~~~~~~
irc_model.cpp: In lambda function:
irc_model.cpp:193:38: error: invalid conversion from 'void*' to 'Fl_Timeout_Handler' {aka 'void (*)(void*)'} [-fpermissive]
  193 |             Fl::remove_timeout(self->connectTimeoutHandle_);
      |                                ~~~~~~^~~~~~~~~~~~~~~~~~~~~
      |                                      |
      |                                      void*
/usr/include/FL/Fl.H:447:30: note: initializing argument 1 of 'static void Fl::remove_timeout(Fl_Timeout_Handler, void*)'
  447 |   static void remove_timeout(Fl_Timeout_Handler, void* = 0);
      |                              ^~~~~~~~~~~~~~~~~~
irc_model.cpp: In member function 'void IRCModel::startNonBlockingConnect(int)':
irc_model.cpp:220:47: error: void value not ignored as it ought to be
  220 |     connectTimeoutHandle_ = Fl::repeat_timeout(5.0, [](void* data) {
      |                             ~~~~~~~~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~
  221 |         IRCModel* self = static_cast<IRCModel*>(data);
      |         ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  222 |         if (self->connectAttempt_) {
      |         ~~~~~~~~~~~~~~~~~~~~~~~~~~~~           
  223 |             self->disconnect();
      |             ~~~~~~~~~~~~~~~~~~~                
  224 |             if (self->controller_) self->controller_->onConnectionFailed("Connection timeout");
      |             ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  225 |         }
      |         ~                                      
  226 |         self->connectTimeoutHandle_ = nullptr;
      |         ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ 
  227 |     }, this);
      |     ~~~~~~~~                                   
irc_model.cpp: In member function 'void IRCModel::disconnect(const std::string&)':
irc_model.cpp:232:28: error: invalid conversion from 'void*' to 'Fl_Timeout_Handler' {aka 'void (*)(void*)'} [-fpermissive]
  232 |         Fl::remove_timeout(connectTimeoutHandle_);
      |                            ^~~~~~~~~~~~~~~~~~~~~
      |                            |
      |                            void*
/usr/include/FL/Fl.H:447:30: note: initializing argument 1 of 'static void Fl::remove_timeout(Fl_Timeout_Handler, void*)'
  447 |   static void remove_timeout(Fl_Timeout_Handler, void* = 0);
      |                              ^~~~~~~~~~~~~~~~~~
make: *** [Makefile:18: irc_model.o] Error 1
