#include "function.h"
#include "proxy.h"

int main() {
  const char * port = "3333";
  proxy * myproxy = new proxy(port);
  myproxy->run();
  return 1;
}