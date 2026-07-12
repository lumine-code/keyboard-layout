#include "keyboard-layout-manager.h"

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XKBrules.h>
#include <cctype>
#include <cwctype>
#include <locale.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef DEBUG
#include <iostream>
#endif

// Whether to check XInputContext when trying to match up a keyboard key with
// a character.
#define USE_XIC 0

// Wayland-only block begins…
#if HAS_WAYLAND

#include <optional>
#include <poll.h>

// Enumerates the various modifiers on this keyboard and tests which one brings
// us to Level 3. This correlates to what we expect from the AltGr key.
static std::optional<size_t> IndexOfLevel3Modifier(WaylandKeymapContext* ctx) {
  if (!ctx->xkb_keymap) return std::nullopt;
  struct xkb_state *state = xkb_state_new(ctx->xkb_keymap);

  // Iterate through all the modifiers.
  for (xkb_mod_index_t mod = 0; mod < xkb_keymap_num_mods(ctx->xkb_keymap); mod++) {
    // Build a mask consisting of just this modifier key.
    xkb_mod_mask_t mask = 1 << mod;
    const char *mod_name = xkb_keymap_mod_get_name(ctx->xkb_keymap, mod);
    xkb_state_update_mask(state, mask, 0, 0, 0, 0, 0);

    bool activates_level3 = false;
    int level3_keys_count = 0;

    // Iterate through all the keycodes and see if any of them correspond to
    // Level 3 when combined with this modifier.
    for (xkb_keycode_t keycode = 8; keycode < 256; keycode++) {
      if (!xkb_keymap_key_get_name(ctx->xkb_keymap, keycode)) {
        continue;
      }

      // By observation, index 0 is always the active layout, at least for
      // GNOME; we hope it's universally true.
      xkb_layout_index_t active_layout = 0;
      xkb_level_index_t level = xkb_state_key_get_level(state, keycode, active_layout);

      // Levels are zero-indexed, so “2” is what we expect here.
      if (level == 2) {
        level3_keys_count++;
        if (level3_keys_count >= 3) {
          activates_level3 = true;
          break;
        }
      }
    }

    if (activates_level3) {
#ifdef DEBUG
      std::cout << "Found Level3 modifier: " << (mod_name ? mod_name : "unnamed") << " " << mod << std::endl;
#endif
      xkb_state_unref(state);
      return mod;
    }
  }

  xkb_state_unref(state);
  return std::nullopt;
}

// REGISTRY LISTENER
// =================

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  auto that = (static_cast<KeyboardLayoutManager *>(data));
  auto ctx = that->waylandContext;
  if (strcmp(interface, "wl_seat") == 0) {
    ctx->seat = (struct wl_seat *)wl_registry_bind(registry, name,
                                                   &wl_seat_interface, 1);
    if (ctx->seat) {
      ctx->keyboard = wl_seat_get_keyboard(ctx->seat);
    }
  }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
  // Not used
}

static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove};

// KEYBOARD LISTENER
// =================

// Keyboard listener callbacks
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
  auto that = (static_cast<KeyboardLayoutManager *>(data));
  auto ctx = that->waylandContext;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }

  char *keymap_string = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

#ifdef DEBUG
  std::cout << "KEYMAP STRING:" << std::endl << keymap_string << std::endl;
#endif

  if (keymap_string == MAP_FAILED) {
    close(fd);
    return;
  }

  ctx->xkb_keymap = xkb_keymap_new_from_string(ctx->xkb_context, keymap_string,
                                               XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(keymap_string, size);
  close(fd);

  if (!ctx->xkb_keymap)
    return;

  ctx->xkb_state = xkb_state_new(ctx->xkb_keymap);

  if (!ctx->xkb_state)
    return;

  // Get shift mask
  xkb_mod_index_t shift_idx =
      xkb_keymap_mod_get_index(ctx->xkb_keymap, XKB_MOD_NAME_SHIFT);
  if (shift_idx != XKB_MOD_INVALID) {
    ctx->shift_mask = 1 << shift_idx;
  }

  // It's surprisingly hard to find out which modifier key corresponds to
  // AltGr. In theory, it could be defined by name — or by `ISO_Level3_Shift` —
  // but in my testing, modifier keys are defined generically (`Mod1`, `Mod2`)
  // and the connection to `AltGr` or `ISO_Level3_Shift` is done within the
  // keymap definition itself
  // (https://github.com/xkbcommon/libxkbcommon/blob/master/doc/keymap-format-text-v1.md).
  //
  // We know that `AltGr` is meant to bring us to keyboard level 3, so we'll
  // try this: loop through the set of modifiers and return the first one that
  // brings us to Level 3 for three or more separate keycodes.
  auto alt_gr_index_result = IndexOfLevel3Modifier(ctx);

  if (alt_gr_index_result.has_value()) {
    size_t alt_gr_index = *alt_gr_index_result;
    ctx->alt_gr_mask = 1 << alt_gr_index;
  } else {
    // If that approach fails, then we can try a more desperate approach in
    // which we pick the most likely names for the `AltGr` key and try each of
    // them in order of likelihood of success. Here we're returning the first
    // such modifier that exists on this keymap, whether or not it actually
    // produces the expected result, so it's more speculative than the first
    // approach.
    const char *alt_gr_names[] = {
      "ISO_Level3_Shift", // Most common for European layouts
      "Mode_switch",      // Often used as an alias
      "AltGr",            // Explicit name on some layouts
      "Mod5",             // Often mapped to AltGr
      "Mod3",             // Sometimes used for AltGr
      "LevelThree",       // Another name used in some layouts
      "Right Alt"         // Sometimes used explicitly
    };

    size_t alt_gr_length = sizeof(alt_gr_names) / sizeof(alt_gr_names[0]);
    for (size_t i = 0; i < alt_gr_length; i++) {
      xkb_mod_index_t idx =
          xkb_keymap_mod_get_index(ctx->xkb_keymap, alt_gr_names[i]);
      if (idx != XKB_MOD_INVALID) {
#ifdef DEBUG
        std::cout << "Using AltGr name: " << alt_gr_names[i] << std::endl;
#endif
        ctx->alt_gr_mask = 1 << idx;
        break;
      }
    }
  }
  if (ctx->keymap_received) {
    // Don't fire the callback unless we've already been through this code path
    // once.
    that->OnNotificationReceived();
  }
  ctx->keymap_received = true;
}

// `wayland-client` requires that we provide a whole bundle of listeners, even
// though we don't really need the rest of these.
static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
  // Not used
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
  // Not used
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state) {
  // Not used
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
  // Not used
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
  // Not used
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter,     keyboard_leave,
    keyboard_key,    keyboard_modifiers, keyboard_repeat_info};

static void CleanupWaylandContext(WaylandKeymapContext *ctx) {
  if (!ctx)
    return;
  if (ctx->xkb_state) {
    xkb_state_unref(ctx->xkb_state);
    ctx->xkb_state = nullptr;
  }
  if (ctx->xkb_keymap) {
    xkb_keymap_unref(ctx->xkb_keymap);
    ctx->xkb_keymap = nullptr;
  }
  if (ctx->xkb_context) {
    xkb_context_unref(ctx->xkb_context);
    ctx->xkb_context = nullptr;
  }
  if (ctx->keyboard) {
    wl_keyboard_destroy(ctx->keyboard);
    ctx->keyboard = nullptr;
  }
  if (ctx->seat) {
    wl_seat_destroy(ctx->seat);
    ctx->seat = nullptr;
  }
  if (ctx->registry) {
    wl_registry_destroy(ctx->registry);
    ctx->registry = nullptr;
  }
  if (ctx->display) {
    wl_display_roundtrip(ctx->display);
    wl_display_disconnect(ctx->display);
    ctx->display = nullptr;
  }
}

// Given a Wayland context, a keycode, and a modifier mask, return the
// character that would be produced by that keycode.
static char *GetCharForKeycode(WaylandKeymapContext *ctx, uint32_t keycode,
                               xkb_mod_mask_t modifiers) {
  // At first I thought we needed to offset this by 8, but it already seems
  // correct as-is.
  xkb_keycode_t xkb_keycode = keycode;

  // Create a copy of the XKB state so we can apply modifiers.
  struct xkb_state *temp_state = xkb_state_new(ctx->xkb_keymap);
  if (!temp_state) {
    return strdup("error");
  }

  xkb_state_update_mask(temp_state, modifiers, 0, 0, 0, 0, 0);

  xkb_keysym_t keysym = xkb_state_key_get_one_sym(temp_state, xkb_keycode);

  // Allocate memory for the result.
  char *result = new char[32];

  // Try to get a UTF-8 representation.
  int len = xkb_keysym_to_utf8(keysym, result, 32);

  if (!result) {
    xkb_state_unref(temp_state);
    delete[] result;
    return NULL;
  }

  // Convert keysym to UTF-8.
  if (keysym == XKB_KEY_NoSymbol) {
    strcpy(result, "Dead");
  } else if (len <= 0) {
    // If we couldn't get a UTF-8 character, fall back to the keysym’s name.
    xkb_keysym_get_name(keysym, result, 32);
  }

  xkb_state_unref(temp_state);
  return result;
}

static Napi::Value WaylandCharacterForCode(Napi::Env env,
                                           WaylandKeymapContext *ctx,
                                           uint32_t keycode,
                                           xkb_mod_mask_t modifiers) {
  char *result = GetCharForKeycode(ctx, keycode, modifiers);
  if (result) {
    auto wrappedResult = Napi::String::New(env, result);
    delete[] result;
    return wrappedResult;
  } else {
    return env.Null();
  }
}

void KeyboardLayoutManager::SetupWaylandPolling() {
  if (!waylandContext || !waylandContext->display)
    return;

  int fd = wl_display_get_fd(waylandContext->display);

  waylandPolling = true;
  waylandPollThread = std::thread([this, fd]() {
    struct pollfd descriptor = {fd, POLLIN, 0};
    while (waylandPolling) {
      descriptor.revents = 0;
      int status = poll(&descriptor, 1, 200);
      if (!waylandPolling)
        break;
      if (status <= 0 || !(descriptor.revents & POLLIN))
        continue;

      std::lock_guard<std::mutex> lock(waylandMutex);
      if (!waylandContext || !waylandContext->display)
        break;
      while (wl_display_prepare_read(waylandContext->display) != 0) {
        wl_display_dispatch_pending(waylandContext->display);
      }
      if (wl_display_read_events(waylandContext->display) < 0)
        continue;
      wl_display_dispatch_pending(waylandContext->display);
    }
  });
}

void KeyboardLayoutManager::CleanupWaylandPolling() {
  waylandPolling = false;
  if (waylandPollThread.joinable())
    waylandPollThread.join();
}

#endif // HAS_WAYLAND

void KeyboardLayoutManager::PlatformSetup(const Napi::CallbackInfo &info) {
  auto env = info.Env();

#if HAS_WAYLAND
  // When we're compiled with Wayland support, assume we're on Wayland to start
  // out, and revert to the X11 approach only if we fail to obtain a keymap via
  // Wayland APIs.
  isWayland = true;

  waylandContext = new WaylandKeymapContext();
  memset(waylandContext, 0, sizeof(WaylandKeymapContext));

  waylandContext->display = wl_display_connect(NULL);
  if (!waylandContext->display) {
    CleanupWaylandContext(waylandContext);
    goto x11;
  }

  waylandContext->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!waylandContext->xkb_context) {
    CleanupWaylandContext(waylandContext);
    goto x11;
  }

  // If we can get as far as creating a Wayland display and context, it is
  // assumed that we are on Wayland and not X11. But since any failure further
  // along the Wayland path of this function is fatal, there's no reason not to
  // at least _try_ to fallback to X11.

  // Set up registry listeners so that we can discover a seat and a keyboard.
  waylandContext->registry = wl_display_get_registry(waylandContext->display);
  wl_registry_add_listener(waylandContext->registry, &registry_listener,
                           this);

  // Process registry events.
  wl_display_roundtrip(waylandContext->display);

  // If a seat was found, add a keyboard listener. This should trigger the
  // sending of a `keymap` event that will allow us to get the data we want.
  if (waylandContext->keyboard) {
    wl_keyboard_add_listener(waylandContext->keyboard, &keyboard_listener,
                             this);
  } else {
    CleanupWaylandContext(waylandContext);
    goto x11;
  }

  // Wait for the keymap to be received.
  //
  // TODO: Timeout?
  while (!waylandContext->keymap_received) {
    if (wl_display_dispatch(waylandContext->display) < 0) {
      CleanupWaylandContext(waylandContext);
      goto x11;
    }
  }

  // Once we've gotten this far, we have everything we need to inspect keyboard
  // behavior. Now we'll set up polling on the event loop so we can find out
  // when the keyboard layout changes.
  SetupWaylandPolling();
  return;

#endif // HAS_WAYLAND

x11:
  isWayland = false;
  xDisplay = XOpenDisplay("");
  CHECK_VOID(xDisplay, "Could not connect to X display", env);

  xInputMethod = XOpenIM(xDisplay, 0, 0, 0);
  if (!xInputMethod)
    return;

  XIMStyles *styles = 0;
  if (XGetIMValues(xInputMethod, XNQueryInputStyle, &styles, NULL) || !styles) {
    return;
  }

  XIMStyle bestMatchStyle = 0;
  for (int i = 0; i < styles->count_styles; i++) {
    XIMStyle thisStyle = styles->supported_styles[i];
    if (thisStyle == (XIMPreeditNothing | XIMStatusNothing)) {
      bestMatchStyle = thisStyle;
      break;
    }
  }
  XFree(styles);
  if (!bestMatchStyle)
    return;

  Window window;
  int revert_to;
  XGetInputFocus(xDisplay, &window, &revert_to);
  if (window != BadRequest) {
    xInputContext =
        XCreateIC(xInputMethod, XNInputStyle, bestMatchStyle, XNClientWindow,
                  window, XNFocusWindow, window, NULL);
  }
}

void KeyboardLayoutManager::PlatformTeardown() {
#if HAS_WAYLAND
  CleanupWaylandPolling();
  CleanupWaylandContext(waylandContext);
  delete waylandContext;
  waylandContext = nullptr;
#endif
  callback.Reset();
};

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(
    const Napi::CallbackInfo &info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  Napi::Value result;

  if (isWayland) {
#if HAS_WAYLAND
    std::lock_guard<std::mutex> lock(waylandMutex);
    if (!waylandContext || !waylandContext->xkb_keymap ||
        !waylandContext->xkb_state) {
      return env.Null();
    }

    // Based on lots of experimentation with Gnome/Wayland, the layout at index
    // 0 will always be the active layout. This may or may not be true for
    // other Wayland server implementations, but we'll go with it for now —
    // because if it isn't true, we'd be hard-pressed to discover that
    // information any other way.
    const char *layout_name =
        xkb_keymap_layout_get_name(waylandContext->xkb_keymap, 0);

#ifdef DEBUG
    std::cout << "Current layout: " << layout_name << std::endl;
#endif
    result = Napi::String::New(env, layout_name);
#endif // HAS_WAYLAND
  } else {
    // X11
    XkbRF_VarDefsRec vdr;
    char *tmp = NULL;
    if (XkbRF_GetNamesProp(xDisplay, &tmp, &vdr) && vdr.layout) {
      XkbStateRec xkbState;
      XkbGetState(xDisplay, XkbUseCoreKbd, &xkbState);
      if (vdr.variant) {
        result = Napi::String::New(
            env, std::string(vdr.layout) + "," + std::string(vdr.variant) +
                     " [" + std::to_string(xkbState.group) + "]");
      } else {
        result =
            Napi::String::New(env, std::string(vdr.layout) + " [" +
                                       std::to_string(xkbState.group) + "]");
      }
    } else {
      result = env.Null();
    }
  }
  return result;
}


Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLanguage(
    const Napi::CallbackInfo &info) {
  // No distinction between “language” and “layout” on Linux.
  return GetCurrentKeyboardLayout(info);
}

Napi::Value KeyboardLayoutManager::GetInstalledKeyboardLanguages(
    const Napi::CallbackInfo &info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  return env.Undefined();
}

struct KeycodeMapEntry {
  uint xkbKeycode;
  const char *dom3Code;
};

#define USB_KEYMAP_DECLARATION static const KeycodeMapEntry keyCodeMap[] =
#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id)                        \
  { xkb, code }

#include "keycode_converter_data.inc"


Napi::Value CharacterForNativeCode(Napi::Env env, XIC xInputContext,
                                   XKeyEvent *keyEvent, uint xkbKeycode,
                                   uint state) {
  keyEvent->keycode = xkbKeycode;
  keyEvent->state = state;

  // We're keeping this code path around so that we can return to using it in
  // certain scenarios if need be. But for now it's producing the wrong results
  // in many cases.
  if (USE_XIC) {
    wchar_t characters[2];
    char utf8[MB_CUR_MAX * 2 + 1];
    int count =
        XwcLookupString(xInputContext, keyEvent, characters, 2, NULL, NULL);
    size_t len = wcstombs(utf8, characters, sizeof(utf8));
    if (len == (size_t)-1) {
      return env.Null();
    }

    if (count > 0 && !std::iswcntrl(characters[0])) {
      return Napi::String::New(env, std::string(utf8, len));
    } else {
      return env.Null();
    }
  } else {
    // Graceful fallback for systems where no window is open or no input
    // context can be found.
    char characters[2];
    int count = XLookupString(keyEvent, characters, 2, NULL, NULL);
    if (count > 0 && !std::iscntrl(characters[0])) {
      return Napi::String::New(env, std::string(characters, count));
    } else {
      return env.Null();
    }
  }
}


Napi::Value
KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo &info) {
  auto env = info.Env();
  Napi::Object result = Napi::Object::New(env);
  Napi::String unmodifiedKey = Napi::String::New(env, "unmodified");
  Napi::String withShiftKey = Napi::String::New(env, "withShift");
  Napi::String withAltGraphKey = Napi::String::New(env, "withAltGraph");
  Napi::String withAltGraphShiftKey =
      Napi::String::New(env, "withAltGraphShift");

  if (isWayland) {

#if HAS_WAYLAND
    std::lock_guard<std::mutex> lock(waylandMutex);
    size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);
    for (size_t i = 0; i < keyCodeMapSize; i++) {
      const char *dom3Code = keyCodeMap[i].dom3Code;
      uint xkbKeycode = keyCodeMap[i].xkbKeycode;
      if (dom3Code && xkbKeycode > 0x0000) {
        Napi::String dom3CodeKey = Napi::String::New(env, dom3Code);
        Napi::Value unmodified =
            WaylandCharacterForCode(env, waylandContext, xkbKeycode, 0);
        Napi::Value withShift = WaylandCharacterForCode(
            env, waylandContext, xkbKeycode, waylandContext->shift_mask);
        Napi::Value withAltGraph = WaylandCharacterForCode(
            env, waylandContext, xkbKeycode, waylandContext->alt_gr_mask);
        Napi::Value withAltGraphShift = WaylandCharacterForCode(
            env, waylandContext, xkbKeycode,
            waylandContext->shift_mask | waylandContext->alt_gr_mask);

        if (unmodified.IsString() || withShift.IsString() ||
            withAltGraph.IsString() || withAltGraphShift.IsString()) {
          Napi::Object entry = Napi::Object::New(env);
          (entry).Set(unmodifiedKey, unmodified);
          (entry).Set(withShiftKey, withShift);
          (entry).Set(withAltGraphKey, withAltGraph);
          (entry).Set(withAltGraphShiftKey, withAltGraphShift);
          (result).Set(dom3CodeKey, entry);
        }
      }
    }
#endif // HAS_WAYLAND

  } else {
    // Clear cached keymap.
    XMappingEvent eventMap = {MappingNotify,   0, false, xDisplay, 0,
                              MappingKeyboard, 0, 0};
    XRefreshKeyboardMapping(&eventMap);

    XkbStateRec xkbState;
    XkbGetState(xDisplay, XkbUseCoreKbd, &xkbState);
    uint keyboardBaseState = 0x0000;
    if (xkbState.group == 1) {
      keyboardBaseState = 0x2000;
    } else if (xkbState.group == 2) {
      keyboardBaseState = 0x4000;
    }

    // Set up an event to reuse across CharacterForNativeCode calls.
    XEvent event;
    memset(&event, 0, sizeof(XEvent));
    XKeyEvent *keyEvent = &event.xkey;
    keyEvent->display = xDisplay;
    keyEvent->type = KeyPress;

    size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);
    for (size_t i = 0; i < keyCodeMapSize; i++) {
      const char *dom3Code = keyCodeMap[i].dom3Code;
      uint xkbKeycode = keyCodeMap[i].xkbKeycode;

      if (dom3Code && xkbKeycode > 0x0000) {
        Napi::String dom3CodeKey = Napi::String::New(env, dom3Code);
        Napi::Value unmodified = CharacterForNativeCode(
            env, xInputContext, keyEvent, xkbKeycode, keyboardBaseState);
        Napi::Value withShift =
            CharacterForNativeCode(env, xInputContext, keyEvent, xkbKeycode,
                                   keyboardBaseState | ShiftMask);

        if (unmodified.IsString() || withShift.IsString()) {
          Napi::Object entry = Napi::Object::New(env);
          (entry).Set(unmodifiedKey, unmodified);
          (entry).Set(withShiftKey, withShift);
          (result).Set(dom3CodeKey, entry);
        }
      }
    }
  }

  return result;
}
