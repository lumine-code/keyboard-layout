#include "keyboard-layout-manager.h"
#ifdef DEBUG
#include <iostream>
#endif

KeyboardLayoutManager::KeyboardLayoutManager(const Napi::CallbackInfo& info):
 Napi::ObjectWrap<KeyboardLayoutManager>(info),
 _env(info.Env()) {
  auto env = info.Env();
  CHECK_VOID(
    info[0].IsFunction(),
    "Expected function as first argument",
    env
  );

  tsfn = Napi::ThreadSafeFunction::New(
    env,
    info[0].As<Napi::Function>(),
    "keyboard-layout-listener",
    0,
    1
  );

  tsfn.Unref(env);

  napi_status status = napi_add_env_cleanup_hook(env, CleanupHook, this);
  if (status != napi_ok) {
    Napi::Error::New(env, "Could not register keyboard layout cleanup").ThrowAsJavaScriptException();
    return;
  }
  cleanupHookRegistered = true;

  PlatformSetup(info);
}

// Runs on a background thread.
void KeyboardLayoutManager::OnNotificationReceived() {
  // We don't need to send any arguments; we just need to signal the main
  // thread.
  if (!disposed.load()) {
    tsfn.NonBlockingCall();
  }
}

void KeyboardLayoutManager::CleanupHook(void* data) {
  auto manager = static_cast<KeyboardLayoutManager*>(data);
  manager->cleanupHookRegistered = false;
  manager->Cleanup();
}

void KeyboardLayoutManager::Dispose(const Napi::CallbackInfo& info) {
  Cleanup();
}

void KeyboardLayoutManager::Cleanup() {
  if (disposed.exchange(true)) return;
  if (cleanupHookRegistered) {
    napi_remove_env_cleanup_hook(_env, CleanupHook, this);
    cleanupHookRegistered = false;
  }
  // Stop every producer before releasing the thread-safe function it uses.
  PlatformTeardown();
  if (tsfn != nullptr) tsfn.Abort();
}

KeyboardLayoutManager::~KeyboardLayoutManager() {
  Cleanup();
}

void KeyboardLayoutManager::Init(Napi::Env env, Napi::Object exports) {
#ifdef DEBUG
  std::cout << "KeyboardLayoutManager::Init" << std::endl;
#endif

  Napi::Function func = DefineClass(env, "KeyboardLayoutManager", {
    InstanceMethod<&KeyboardLayoutManager::GetCurrentKeyboardLayout>("getCurrentKeyboardLayout", napi_default_method),
    InstanceMethod<&KeyboardLayoutManager::GetCurrentKeyboardLanguage>("getCurrentKeyboardLanguage", napi_default_method),
    InstanceMethod<&KeyboardLayoutManager::GetInstalledKeyboardLanguages>("getInstalledKeyboardLanguages", napi_default_method),
    InstanceMethod<&KeyboardLayoutManager::GetCurrentKeymap>("getCurrentKeymap", napi_default_method),
    InstanceMethod<&KeyboardLayoutManager::Dispose>("dispose", napi_default_method)
  });

  exports.Set("KeyboardLayoutManager", func);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  KeyboardLayoutManager::Init(env, exports);
  return exports;
}

NODE_API_MODULE(keyboard_layout_manager, Init)
