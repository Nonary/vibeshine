#include "src/audio_usb_session.h"
#include <cstdio>
#include <exception>
int main() {
  try { audio::test_usb_session_guards(); }
  catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what());return 1; }
  std::puts("PASS production USB session quoting, metadata, owner, profile and notification guards");
}
