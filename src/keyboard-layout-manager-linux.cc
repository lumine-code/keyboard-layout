#include "keyboard-layout-manager.h"

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XKBrules.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

#ifdef DEBUG
#include <iostream>
#endif

// Wayland-only block begins…
#if HAS_WAYLAND

#include <optional>
#include <poll.h>

// Enumerates the various modifiers on this keyboard and tests which one brings
// us to Level 3. This correlates to what we expect from the AltGr key.
static std::optional<size_t> IndexOfLevel3Modifier(WaylandKeymapContext* ctx) {
  if (!ctx->xkb_keymap) return std::nullopt;
  struct xkb_state *state = xkb_state_new(ctx->xkb_keymap);
  if (!state) return std::nullopt;

  // Iterate through all the modifiers.
  for (xkb_mod_index_t mod = 0;
       mod < xkb_keymap_num_mods(ctx->xkb_keymap) &&
       mod < sizeof(xkb_mod_mask_t) * CHAR_BIT; mod++) {
    // Build a mask consisting of just this modifier key.
    xkb_mod_mask_t mask = xkb_mod_mask_t{1} << mod;
    const char *mod_name = xkb_keymap_mod_get_name(ctx->xkb_keymap, mod);
    xkb_state_update_mask(state, mask, 0, 0, 0, 0, ctx->group);

    bool activates_level3 = false;
    int level3_keys_count = 0;

    // Iterate through all the keycodes and see if any of them correspond to
    // Level 3 when combined with this modifier.
    for (xkb_keycode_t keycode = 8; keycode < 256; keycode++) {
      if (!xkb_keymap_key_get_name(ctx->xkb_keymap, keycode)) {
        continue;
      }

      xkb_layout_index_t active_layout = ctx->group;
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

static void AddSeatListener(KeyboardLayoutManager *that);

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  auto that = (static_cast<KeyboardLayoutManager *>(data));
  auto ctx = that->waylandContext;
  if (strcmp(interface, "wl_seat") == 0 && !ctx->seat) {
    ctx->seat = (struct wl_seat *)wl_registry_bind(registry, name,
                                                   &wl_seat_interface, 1);
    if (ctx->seat) {
      AddSeatListener(that);
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

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
    close(fd);
    return;
  }

  char *keymap_string = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (keymap_string == MAP_FAILED) {
    close(fd);
    return;
  }

  // The protocol includes a trailing NUL. Use the bounded API so a bad
  // compositor cannot make the parser read beyond the mapped buffer.
  size_t length = size;
  if (keymap_string[length - 1] == '\0') --length;
  auto keymap = xkb_keymap_new_from_buffer(ctx->xkb_context, keymap_string,
                                         length, XKB_KEYMAP_FORMAT_TEXT_V1,
                                         XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(keymap_string, size);
  close(fd);

  if (!keymap)
    return;

  auto state = xkb_state_new(keymap);

  if (!state) {
    xkb_keymap_unref(keymap);
    return;
  }

  // A compositor can replace its keymap repeatedly. Keep the last valid
  // generation until the replacement has compiled successfully.
  if (ctx->xkb_state) xkb_state_unref(ctx->xkb_state);
  if (ctx->xkb_keymap) xkb_keymap_unref(ctx->xkb_keymap);
  ctx->xkb_keymap = keymap;
  ctx->xkb_state = state;
  ctx->group = 0;
  ctx->shift_mask = 0;
  ctx->alt_gr_mask = 0;

  // Get shift mask
  xkb_mod_index_t shift_idx =
      xkb_keymap_mod_get_index(ctx->xkb_keymap, XKB_MOD_NAME_SHIFT);
  if (shift_idx != XKB_MOD_INVALID &&
      shift_idx < sizeof(xkb_mod_mask_t) * CHAR_BIT) {
    ctx->shift_mask = xkb_mod_mask_t{1} << shift_idx;
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
    ctx->alt_gr_mask = xkb_mod_mask_t{1} << alt_gr_index;
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
      if (idx != XKB_MOD_INVALID && idx < sizeof(xkb_mod_mask_t) * CHAR_BIT) {
#ifdef DEBUG
        std::cout << "Using AltGr name: " << alt_gr_names[i] << std::endl;
#endif
        ctx->alt_gr_mask = xkb_mod_mask_t{1} << idx;
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
  auto that = static_cast<KeyboardLayoutManager *>(data);
  auto ctx = that->waylandContext;
  if (!ctx->xkb_state) return;

  auto changed = xkb_state_update_mask(ctx->xkb_state, mods_depressed,
                                      mods_latched, mods_locked, 0, 0, group);
  ctx->group = 0;
  for (xkb_layout_index_t index = 0;
       index < xkb_keymap_num_layouts(ctx->xkb_keymap); ++index) {
    if (xkb_state_layout_index_is_active(ctx->xkb_state, index,
                                         XKB_STATE_LAYOUT_EFFECTIVE) > 0) {
      ctx->group = index;
      break;
    }
  }
  if (changed & XKB_STATE_LAYOUT_EFFECTIVE) that->OnNotificationReceived();
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
  // Not used
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter,     keyboard_leave,
    keyboard_key,    keyboard_modifiers, keyboard_repeat_info};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities) {
  auto that = static_cast<KeyboardLayoutManager *>(data);
  auto ctx = that->waylandContext;
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !ctx->keyboard) {
    ctx->keyboard = wl_seat_get_keyboard(seat);
    if (ctx->keyboard)
      wl_keyboard_add_listener(ctx->keyboard, &keyboard_listener, that);
  } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && ctx->keyboard) {
    wl_keyboard_destroy(ctx->keyboard);
    ctx->keyboard = nullptr;
  }
}

static void AddSeatListener(KeyboardLayoutManager *that) {
  static const struct wl_seat_listener listener = {seat_capabilities, nullptr};
  wl_seat_add_listener(that->waylandContext->seat, &listener, that);
}

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
    wl_display_disconnect(ctx->display);
    ctx->display = nullptr;
  }
}

// Given a Wayland context, a keycode, and a modifier mask, return the
// character that would be produced by that keycode.
static std::string GetCharForKeycode(WaylandKeymapContext *ctx, uint32_t keycode,
                                     xkb_mod_mask_t modifiers) {
  if (!ctx || !ctx->xkb_keymap) return {};
  // At first I thought we needed to offset this by 8, but it already seems
  // correct as-is.
  xkb_keycode_t xkb_keycode = keycode;

  // Create a copy of the XKB state so we can apply modifiers.
  struct xkb_state *temp_state = xkb_state_new(ctx->xkb_keymap);
  if (!temp_state) {
    return {};
  }

  xkb_state_update_mask(temp_state, modifiers, 0, 0, 0, 0, ctx->group);

  xkb_keysym_t keysym = xkb_state_key_get_one_sym(temp_state, xkb_keycode);

  char result[64] = {};

  // Try to get a UTF-8 representation.
  int len = xkb_keysym_to_utf8(keysym, result, sizeof(result));

  // Convert keysym to UTF-8.
  if (keysym == XKB_KEY_NoSymbol) {
    result[0] = '\0';
  } else if (len <= 0) {
    // If we couldn't get a UTF-8 character, fall back to the keysym’s name.
    if (xkb_keysym_get_name(keysym, result, sizeof(result)) < 0)
      result[0] = '\0';
  }

  xkb_state_unref(temp_state);
  return result;
}

static Napi::Value WaylandCharacterForCode(Napi::Env env,
                                           WaylandKeymapContext *ctx,
                                           uint32_t keycode,
                                           xkb_mod_mask_t modifiers) {
  std::string result = GetCharForKeycode(ctx, keycode, modifiers);
  if (!result.empty()) {
    return Napi::String::New(env, result);
  } else {
    return env.Null();
  }
}

// Pair every prepared read with read_events or cancel_read, including timeouts
// and disconnected compositors. This is shared by bounded setup and polling.
static bool DispatchWaylandEvents(struct wl_display *display, int timeout,
                                  std::mutex *mutex = nullptr) {
  auto dispatchPending = [display, mutex]() {
    if (mutex) {
      std::lock_guard<std::mutex> lock(*mutex);
      return wl_display_dispatch_pending(display);
    }
    return wl_display_dispatch_pending(display);
  };
  while (wl_display_prepare_read(display) != 0) {
    if (dispatchPending() < 0) return false;
  }

  struct pollfd descriptor = {wl_display_get_fd(display), POLLIN, 0};
  if (wl_display_flush(display) < 0) {
    if (errno != EAGAIN) {
      wl_display_cancel_read(display);
      return false;
    }
    descriptor.events |= POLLOUT;
  }

  int status = poll(&descriptor, 1, timeout);
  if (status < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
    wl_display_cancel_read(display);
    return status < 0 && errno == EINTR;
  }
  if (!(descriptor.revents & POLLIN)) {
    wl_display_cancel_read(display);
    return true;
  }
  if (wl_display_read_events(display) < 0) return false;
  return dispatchPending() >= 0;
}

void KeyboardLayoutManager::SetupWaylandPolling() {
  if (!waylandContext || !waylandContext->display)
    return;

  waylandPolling = true;
  waylandPollThread = std::thread([this]() {
    while (waylandPolling) {
      if (!waylandContext || !waylandContext->display)
        break;
      if (!DispatchWaylandEvents(waylandContext->display, 200, &waylandMutex))
        break;
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

  waylandContext = new WaylandKeymapContext{};

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
  if (!waylandContext->registry ||
      wl_registry_add_listener(waylandContext->registry, &registry_listener,
                               this) < 0) {
    CleanupWaylandContext(waylandContext);
    goto x11;
  }

  // Registry, seat capabilities and keymap events can require several request
  // batches. A compositor that never sends a usable map must not hang Node.
  {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!waylandContext->keymap_received) {
      if (std::chrono::steady_clock::now() >= deadline ||
          !DispatchWaylandEvents(waylandContext->display, 100)) {
        CleanupWaylandContext(waylandContext);
        goto x11;
      }
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
}

void KeyboardLayoutManager::PlatformTeardown() {
#if HAS_WAYLAND
  CleanupWaylandPolling();
  CleanupWaylandContext(waylandContext);
  delete waylandContext;
  waylandContext = nullptr;
#endif
  if (xDisplay) {
    XCloseDisplay(xDisplay);
    xDisplay = nullptr;
  }
};

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(
    const Napi::CallbackInfo &info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  Napi::Value result = env.Null();

  if (isWayland) {
#if HAS_WAYLAND
    std::lock_guard<std::mutex> lock(waylandMutex);
    if (!waylandContext || !waylandContext->xkb_keymap ||
        !waylandContext->xkb_state) {
      return env.Null();
    }

    const char *layout_name =
        xkb_keymap_layout_get_name(waylandContext->xkb_keymap,
                                   waylandContext->group);

#ifdef DEBUG
    std::cout << "Current layout: " << layout_name << std::endl;
#endif
    if (layout_name) result = Napi::String::New(env, layout_name);
#endif // HAS_WAYLAND
  } else {
    // X11
    if (!xDisplay) return env.Null();
    XkbRF_VarDefsRec vdr = {};
    char *tmp = NULL;
    std::string layout;
    if (XkbRF_GetNamesProp(xDisplay, &tmp, &vdr) && vdr.layout) {
      XkbStateRec xkbState = {};
      if (XkbGetState(xDisplay, XkbUseCoreKbd, &xkbState) == Success) {
        layout = vdr.layout;
        if (vdr.variant) layout += "," + std::string(vdr.variant);
        layout += " [" + std::to_string(xkbState.group) + "]";
      }
    }
    free(tmp);
    free(vdr.model);
    free(vdr.layout);
    free(vdr.variant);
    free(vdr.options);
    if (!layout.empty()) result = Napi::String::New(env, layout);
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
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
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


Napi::Value CharacterForNativeCode(Napi::Env env, XKeyEvent *keyEvent,
                                   uint xkbKeycode,
                                   uint state) {
  keyEvent->keycode = xkbKeycode;
  keyEvent->state = state;

  // XLookupString returns ISO Latin-1 bytes, not UTF-8. Passing them to the
  // UTF-8 overload corrupts accented characters such as e-acute.
  char characters[2];
  int count = XLookupString(keyEvent, characters, sizeof(characters), NULL, NULL);
  if (count <= 0 ||
      std::iscntrl(static_cast<unsigned char>(characters[0]))) return env.Null();

  std::u16string unicode;
  for (int i = 0; i < count; ++i)
    unicode.push_back(static_cast<unsigned char>(characters[i]));
  return Napi::String::New(env, unicode);
}


Napi::Value
KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo &info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  Napi::Object result = Napi::Object::New(env);
  Napi::String unmodifiedKey = Napi::String::New(env, "unmodified");
  Napi::String withShiftKey = Napi::String::New(env, "withShift");
  Napi::String withAltGraphKey = Napi::String::New(env, "withAltGraph");
  Napi::String withAltGraphShiftKey =
      Napi::String::New(env, "withAltGraphShift");

  if (isWayland) {

#if HAS_WAYLAND
    std::lock_guard<std::mutex> lock(waylandMutex);
    if (!waylandContext || !waylandContext->xkb_keymap) return result;
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
    if (!xDisplay) return result;
    // Clear cached keymap.
    XMappingEvent eventMap = {MappingNotify,   0, false, xDisplay, 0,
                              MappingKeyboard, 0, 0};
    XRefreshKeyboardMapping(&eventMap);

    XkbStateRec xkbState = {};
    if (XkbGetState(xDisplay, XkbUseCoreKbd, &xkbState) != Success) return result;
    uint keyboardBaseState = XkbBuildCoreState(0, xkbState.group);

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
            env, keyEvent, xkbKeycode, keyboardBaseState);
        Napi::Value withShift =
            CharacterForNativeCode(env, keyEvent, xkbKeycode,
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
