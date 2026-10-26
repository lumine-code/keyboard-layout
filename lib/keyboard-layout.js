"use strict";

const { Disposable, Emitter } = require("@lumine-code/event-kit");
const { setInterval, clearInterval } = require("node:timers");
const { KeyboardLayoutManager } = require("../build/Release/keyboard-layout-manager.node");

const emitter = new Emitter();
const keymapsByLayoutName = new Map();
let disposed = false;
let observationTimer = null;
let observationCount = 0;
let observedLayout;

const manager = new KeyboardLayoutManager(didChangeLayout);

function didChangeLayout() {
  if (disposed) return;
  keymapsByLayoutName.clear();
  const current = getCurrentKeyboardLayout();
  emitter.emit("did-change-current-keyboard-layout", current);
}

function ensureActive() {
  if (disposed) throw new Error("Keyboard layout observer has been disposed");
}

function getCurrentKeymap() {
  const currentLayout = getCurrentKeyboardLayout();
  let currentKeymap = keymapsByLayoutName.get(currentLayout);
  if (!currentKeymap) {
    currentKeymap = manager.getCurrentKeymap();
    keymapsByLayoutName.set(currentLayout, currentKeymap);
  }
  return currentKeymap;
}

function getCurrentKeyboardLayout() {
  ensureActive();
  return manager.getCurrentKeyboardLayout();
}

function getCurrentKeyboardLanguage() {
  ensureActive();
  return manager.getCurrentKeyboardLanguage();
}

function getInstalledKeyboardLanguages() {
  ensureActive();
  const languages = new Set();

  for (const language of manager.getInstalledKeyboardLanguages() || []) {
    languages.add(language);
  }

  return Array.from(languages);
}

function onDidChangeCurrentKeyboardLayout(callback) {
  ensureActive();
  const subscription = emitter.on("did-change-current-keyboard-layout", callback);
  observationCount++;
  // Win32 has no native listener in this addon. Poll only while someone is
  // observing, and leave the event loop free to exit.
  if (process.platform === "win32" && observationTimer === null) {
    observedLayout = getCurrentKeyboardLayout();
    observationTimer = setInterval(() => {
      const current = getCurrentKeyboardLayout();
      if (current !== observedLayout) {
        observedLayout = current;
        didChangeLayout();
      }
    }, 100);
    observationTimer.unref();
  }
  return new Disposable(() => {
    subscription.dispose();
    observationCount--;
    if (observationCount === 0 && observationTimer !== null) {
      clearInterval(observationTimer);
      observationTimer = null;
    }
  });
}

function observeCurrentKeyboardLayout(callback) {
  callback(getCurrentKeyboardLayout());
  return onDidChangeCurrentKeyboardLayout(callback);
}

function dispose() {
  if (disposed) return;
  disposed = true;
  if (observationTimer !== null) {
    clearInterval(observationTimer);
    observationTimer = null;
  }
  manager.dispose();
  emitter.dispose();
  keymapsByLayoutName.clear();
}

module.exports = {
  getCurrentKeymap,
  getCurrentKeyboardLayout,
  getCurrentKeyboardLanguage,
  getInstalledKeyboardLanguages,
  onDidChangeCurrentKeyboardLayout,
  observeCurrentKeyboardLayout,
  dispose,
};
