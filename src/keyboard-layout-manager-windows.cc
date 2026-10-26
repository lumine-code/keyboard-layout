#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00

#undef WINVER
#define WINVER 0x0A00

#include "keyboard-layout-manager.h"

#include <string>
#include <cwctype>
#include <vector>
#include <windows.h>

using namespace Napi;

Napi::Value LanguageForLayout(Napi::Env env, HKL layout) {
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  const LANGID language = LOWORD(reinterpret_cast<UINT_PTR>(layout));
  const int length = LCIDToLocaleName(
    MAKELCID(language, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0);
  if (length <= 1) return env.Null();
  return Napi::String::New(env, reinterpret_cast<const char16_t*>(name), length - 1);
}

HKL GetForegroundWindowHKL() {
  DWORD dwThreadId = 0;
  HWND hWnd = GetForegroundWindow();
  if (hWnd != NULL) {
    dwThreadId = GetWindowThreadProcessId(hWnd, NULL);
  }
  return GetKeyboardLayout(dwThreadId);
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  const HKL current = GetKeyboardLayout(0);
  const HKL foreground = GetForegroundWindowHKL();
  if (current != foreground && !ActivateKeyboardLayout(foreground, 0)) return env.Null();
  char layoutName[KL_NAMELENGTH] = {};
  const BOOL success = ::GetKeyboardLayoutNameA(layoutName);
  if (current != foreground) ActivateKeyboardLayout(current, 0);
  if (success) {
    return Napi::String::New(env, layoutName);
  } else {
    return env.Null();
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  return LanguageForLayout(env, GetForegroundWindowHKL());
}

Napi::Value KeyboardLayoutManager::GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  const int capacity = GetKeyboardLayoutList(0, nullptr);
  Napi::Array result = Napi::Array::New(env);
  if (capacity <= 0) return result;
  std::vector<HKL> layouts(capacity);
  const int layoutCount = GetKeyboardLayoutList(capacity, layouts.data());

  uint32_t index = 0;
  for (int i = 0; i < layoutCount; i++) {
    Napi::Value language = LanguageForLayout(env, layouts[i]);
    if (language.IsString()) result.Set(index++, language);
  }
  return result;
}

void KeyboardLayoutManager::PlatformSetup(const Napi::CallbackInfo& info) {
  // no-op
}

void KeyboardLayoutManager::PlatformTeardown() {
  // no-op
};

struct KeycodeMapEntry {
  UINT scanCode;
  const char *dom3Code;
};

#define USB_KEYMAP_DECLARATION static const KeycodeMapEntry keyCodeMap[] =
#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id) {win, code}

#include "keycode_converter_data.inc"

Napi::Value CharacterForNativeCode(
  Napi::Env env,
  HKL keyboardLayout,
  UINT keyCode,
  UINT scanCode,
  BYTE *keyboardState,
  bool shift,
  bool altGraph
) {
  memset(keyboardState, 0, 256);
  if (shift) {
    keyboardState[VK_SHIFT] = 0x80;
  }

  if (altGraph) {
    keyboardState[VK_MENU] = 0x80;
    keyboardState[VK_CONTROL] = 0x80;
  }

  wchar_t characters[8] = {};
  // Windows 10 1607+: inspect the layout without consuming or installing a
  // pending dead key in the caller's keyboard state (bit 2).
  const int count = ToUnicodeEx(keyCode, scanCode, keyboardState, characters, 8, 4, keyboardLayout);

  if (count > 0 && count <= 8 && !std::iswcntrl(characters[0])) {
    return Napi::String::New(env, reinterpret_cast<const char16_t*>(characters), count);
  } else {
    return env.Null();
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  BYTE keyboardState[256];
  HKL keyboardLayout = GetForegroundWindowHKL();

  Napi::Object result = Napi::Object::New(env);

  size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);
  for (size_t i = 0; i < keyCodeMapSize; i++) {
    const char *dom3Code = keyCodeMap[i].dom3Code;
    UINT scanCode = keyCodeMap[i].scanCode;

    if (dom3Code && scanCode > 0x0000) {
      UINT keyCode = MapVirtualKeyEx(scanCode, MAPVK_VSC_TO_VK, keyboardLayout);

      if (keyCode == 0) continue;

      Napi::String dom3CodeKey = Napi::String::New(env, dom3Code);
      Napi::Value unmodified = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, false, false);
      Napi::Value withShift = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, true, false);
      Napi::Value withAltGraph = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, false, true);
      Napi::Value withAltGraphShift = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, true, true);

      if (unmodified.IsString() || withShift.IsString() || withAltGraph.IsString() || withAltGraphShift.IsString()) {
        Napi::Object entry = Napi::Object::New(env);
        (entry).Set("unmodified", unmodified);
        (entry).Set("withShift", withShift);
        (entry).Set("withAltGraph", withAltGraph);
        (entry).Set("withAltGraphShift", withAltGraphShift);

        (result).Set(dom3CodeKey, entry);
      }
    }
  }

  return result;
}
