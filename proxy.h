#include "function.h"

class proxy {
 private:
  const char * port_num;

 public:
  proxy(const char * myport) : port_num(myport) {}
  void run();
};