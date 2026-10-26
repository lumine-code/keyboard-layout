#import <Cocoa/Cocoa.h>

#import <Carbon/Carbon.h>
#import "keyboard-layout-manager.h"
#include <mutex>
#include <vector>

namespace {

// Text Input Sources shares process-wide state across Node environments.
// Serialize initialization and reads when several workers load the addon.
std::mutex inputSourceMutex;

class ScopedCFRelease {
public:
  explicit ScopedCFRelease(CFTypeRef value) : value(value) {}
  ~ScopedCFRelease() {
    if (value != nullptr) CFRelease(value);
  }

  ScopedCFRelease(const ScopedCFRelease&) = delete;
  ScopedCFRelease& operator=(const ScopedCFRelease&) = delete;

private:
  CFTypeRef value;
};

CFStringRef FirstInputSourceLanguage(TISInputSourceRef source) {
  if (source == nullptr) return nullptr;

  CFArrayRef languages = static_cast<CFArrayRef>(
    TISGetInputSourceProperty(source, kTISPropertyInputSourceLanguages));
  if (languages == nullptr || CFArrayGetCount(languages) == 0) return nullptr;

  return static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0));
}

Napi::Value StringFromCFString(Napi::Env env, CFStringRef value) {
  if (value == nullptr) return env.Null();

  CFIndex length = CFStringGetLength(value);
  if (length == 0) return Napi::String::New(env, "");

  std::vector<UniChar> characters(static_cast<size_t>(length));
  CFStringGetCharacters(value, CFRangeMake(0, length), characters.data());
  return Napi::String::New(env,
    reinterpret_cast<const char16_t*>(characters.data()), characters.size());
}

} // namespace

static void NotificationHandler(
  CFNotificationCenterRef center,
  void *manager,
  CFStringRef name,
  const void *object,
  CFDictionaryRef userInfo
) {
  (static_cast<KeyboardLayoutManager *>(manager))->OnNotificationReceived();
}

static void RemoveCFObserver(void *arg) {
  auto manager = static_cast<KeyboardLayoutManager*>(arg);
  CFNotificationCenterRemoveObserver(
    CFNotificationCenterGetDistributedCenter(),
    manager,
    kTISNotifySelectedKeyboardInputSourceChanged,
    NULL
  );
}

void KeyboardLayoutManager::PlatformTeardown() {
  RemoveCFObserver(this);
}

void KeyboardLayoutManager::PlatformSetup(const Napi::CallbackInfo& info) {
  CFNotificationCenterAddObserver(
    CFNotificationCenterGetDistributedCenter(),
    this,
    NotificationHandler,
    kTISNotifySelectedKeyboardInputSourceChanged,
    NULL,
    CFNotificationSuspensionBehaviorDeliverImmediately
  );
}

Napi::Value KeyboardLayoutManager::GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  std::lock_guard<std::mutex> lock(inputSourceMutex);

  @autoreleasepool {
    auto result = Napi::Array::New(env);
    uint32_t resultIndex = 0;

    // IMEs (for example Japanese) and keyboard layouts have separate types.
    const CFStringRef inputSourceTypes[] = {
      kTISTypeKeyboardLayout, kTISTypeKeyboardInputMode
    };
    for (CFStringRef inputSourceType : inputSourceTypes) {
      NSDictionary* filter = @{
        (__bridge NSString *) kTISPropertyInputSourceType :
        (__bridge NSString *) inputSourceType
      };
      CFArrayRef sources = TISCreateInputSourceList((__bridge CFDictionaryRef) filter, NO);
      ScopedCFRelease releaseSources(sources);
      if (sources == nullptr) continue;

      for (CFIndex i = 0; i < CFArrayGetCount(sources); i++) {
        TISInputSourceRef source = static_cast<TISInputSourceRef>(
          const_cast<void*>(CFArrayGetValueAtIndex(sources, i)));
        CFStringRef language = FirstInputSourceLanguage(source);
        if (language != nullptr) {
          result.Set(resultIndex++, StringFromCFString(env, language));
        }
      }
    }

    return result;
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  std::lock_guard<std::mutex> lock(inputSourceMutex);
  TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
  ScopedCFRelease releaseSource(source);
  return StringFromCFString(env, FirstInputSourceLanguage(source));
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  std::lock_guard<std::mutex> lock(inputSourceMutex);
  TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
  ScopedCFRelease releaseSource(source);
  if (source == nullptr) return env.Null();

  CFStringRef sourceId = (CFStringRef) TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
  return StringFromCFString(env, sourceId);
}

struct KeycodeMapEntry {
  UInt16 virtualKeyCode;
  const char *dom3Code;
};

#define USB_KEYMAP_DECLARATION static const KeycodeMapEntry keyCodeMap[] =
#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id) {mac, code}

#include "keycode_converter_data.inc"

Napi::Value CharacterForNativeCode(
  Napi::Env env,
  const UCKeyboardLayout* keyboardLayout,
  UInt16 virtualKeyCode,
  EventModifiers modifiers
) {
  // See https://developer.apple.com/documentation/coreservices/1390584-uckeytranslate
  UInt32 modifierKeyState = (modifiers >> 8) & 0xFF;
  UInt32 deadKeyState = 0;
  // UCKeyTranslate supports sequences of up to 255 UTF-16 code units.
  UniChar characters[255] = {};
  UniCharCount charCount = 0;
  OSStatus status = UCKeyTranslate(
      keyboardLayout,
      static_cast<UInt16>(virtualKeyCode),
      kUCKeyActionDown,
      modifierKeyState,
      LMGetKbdLast(),
      kUCKeyTranslateNoDeadKeysMask,
      &deadKeyState,
      sizeof(characters) / sizeof(characters[0]),
      &charCount,
      characters);

  // std::iscntrl only accepts byte values; passing a Unicode character is UB.
  if (status == noErr && charCount != 0 &&
      characters[0] >= 0x20 && characters[0] != 0x7f) {
    return Napi::String::New(env, reinterpret_cast<const char16_t*>(characters), static_cast<size_t>(charCount));
  } else {
    return env.Null();
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(!disposed.load(), "Keyboard layout manager has been disposed", env);
  std::lock_guard<std::mutex> lock(inputSourceMutex);
  TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
  ScopedCFRelease releaseSource(source);
  if (source == nullptr) return env.Null();

  CFDataRef layoutData = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));

  if (layoutData == nullptr || CFDataGetLength(layoutData) == 0) return env.Null();

  const UCKeyboardLayout* keyboardLayout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));
  auto result = Napi::Object::New(env);
  size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);

  for (size_t i = 0; i < keyCodeMapSize; i++) {
    const char *dom3Code = keyCodeMap[i].dom3Code;
    int virtualKeyCode = keyCodeMap[i].virtualKeyCode;
    if (dom3Code && virtualKeyCode < 0xffff) {
      auto dom3CodeKey = Napi::String::New(env, dom3Code);

      Napi::Value unmodified = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, 0);
      Napi::Value withShift = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, (1 << shiftKeyBit));
      Napi::Value withAltGraph = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, (1 << optionKeyBit));
      Napi::Value withAltGraphShift = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, (1 << shiftKeyBit) | (1 << optionKeyBit));

      if (unmodified.IsString() || withShift.IsString() || withAltGraph.IsString() || withAltGraphShift.IsString()) {
        auto entry = Napi::Object::New(env);
        entry.Set("unmodified", unmodified);
        entry.Set("withShift", withShift);
        entry.Set("withAltGraph", withAltGraph);
        entry.Set("withAltGraphShift", withAltGraphShift);
        result.Set(dom3CodeKey, entry);
      }
    }
  }
  return result;
}
