#ifndef SRC_KEYBORD_LAYOUT_OBSERVER_H_
#define SRC_KEYBORD_LAYOUT_OBSERVER_H_

#include "napi.h"

#define CHECK(cond, msg, env)                                  \
if (!(cond)) {                                                 \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return env.Null();                                           \
}

#define CHECK_VOID(cond, msg, env)                             \
if (!(cond)) {                                                 \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return;                                                      \
}

#define THROW(env, msg) {                                      \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
}

#define THROW_AND_RETURN(env, msg) {                           \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return env.Null();                                           \
}


#if defined(__linux__) || defined(__FreeBSD__)
#include <X11/Xlib.h>
#include <string>

#if HAS_WAYLAND
#include <atomic>
#include <mutex>
#include <thread>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct wl_keyboard *keyboard;
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;

  // Whether we've received the first `keymap` event.
  bool keymap_received;
  // The modifier mask for the `Shift` key.
  xkb_mod_mask_t shift_mask;
  // The modifier mask for the `AltGr` key.
  xkb_mod_mask_t alt_gr_mask;
} WaylandKeymapContext;
#endif // HAS_WAYLAND

#endif // __linux__ || __FreeBSD__

class KeyboardLayoutManager : public Napi::ObjectWrap<KeyboardLayoutManager> {
public:
  static void Init(Napi::Env env, Napi::Object exports);
  KeyboardLayoutManager(const Napi::CallbackInfo& info);
  ~KeyboardLayoutManager();

  void OnNotificationReceived();

  Napi::Env _env;
#if defined(__linux__) || defined(__FreeBSD__)
  void ProcessCallbackWrapper();
  bool isWayland;

#if HAS_WAYLAND
  WaylandKeymapContext *waylandContext;
#endif // HAS_WAYLAND

#endif // __linux__ || __FreeBSD__

private:
  Napi::Value GetCurrentKeyboardLayout(const Napi::CallbackInfo& info);
  Napi::Value GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info);
  Napi::Value GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info);
  Napi::Value GetCurrentKeymap(const Napi::CallbackInfo& info);

  void PlatformSetup(const Napi::CallbackInfo& info);
  void PlatformTeardown();

  static void ProcessCallback(Napi::Env env, Napi::Function callback);
  void Cleanup();

#if defined(__linux__) || defined(__FreeBSD__)
  Display *xDisplay;
  XIC xInputContext;
  XIM xInputMethod;

#if HAS_WAYLAND
  std::atomic<bool> waylandPolling{false};
  std::mutex waylandMutex;
  std::thread waylandPollThread;
  void SetupWaylandPolling();
  void CleanupWaylandPolling();
#endif // HAS_WAYLAND
#endif // __linux__ || __FreeBSD__

  bool isFinalizing = false;
  Napi::FunctionReference callback;
  Napi::ThreadSafeFunction tsfn;
};

Napi::Object Init(Napi::Env env, Napi::Object exports);

#endif // SRC_KEYBORD_LAYOUT_OBSERVER_H_
