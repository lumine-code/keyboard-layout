#include <X11/XKBlib.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  Display *display = XOpenDisplay(NULL);
  if (!display) return 2;
  int success = XkbLockGroup(display, XkbUseCoreKbd, atoi(argv[1]));
  XSync(display, False);
  XCloseDisplay(display);
  return success ? 0 : 3;
}
