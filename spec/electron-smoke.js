"use strict";

// Run with Electron, whose runtime supplies this built-in module.
// eslint-disable-next-line n/no-missing-require
const { app, BrowserWindow } = require("electron");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { once } = require("node:events");

const profile = fs.mkdtempSync(path.join(os.tmpdir(), "keyboard-layout-electron-"));
app.setPath("userData", profile);

app.whenReady().then(async () => {
  const window = new BrowserWindow({
    show: false,
    webPreferences: { nodeIntegration: true, contextIsolation: false, sandbox: false },
  });
  const modulePath = path.resolve(__dirname, "../lib/keyboard-layout.js");
  const nativePath = path.resolve(__dirname, "../build/Release/keyboard-layout-manager.node");
  try {
    await window.loadURL("about:blank");
    for (let index = 0; index < 5; index++) {
      const result = await window.webContents.executeJavaScript(`
        (() => {
          try {
          const { KeyboardLayoutManager } = require(${JSON.stringify(nativePath)});
          const native = new KeyboardLayoutManager(() => {});
          native.getCurrentKeymap();
          native.dispose();
          const layout = require(${JSON.stringify(modulePath)});
          layout.observeCurrentKeyboardLayout(() => {});
          return { layout: layout.getCurrentKeyboardLayout(), keys: Object.keys(layout.getCurrentKeymap() || {}).length };
          } catch (error) {
            return { error: error.stack };
          }
        })()
      `);
      assert.equal(result.error, undefined);
      assert.equal(typeof result.layout, "string");
      assert.ok(result.keys > 0);
      const loaded = once(window.webContents, "did-finish-load");
      window.webContents.reload();
      await loaded;
    }
    console.log(`Electron ${process.versions.electron}: 5 renderer reloads passed`);
    app.exit(0);
  } catch (error) {
    console.error(error);
    app.exit(1);
  }
});
