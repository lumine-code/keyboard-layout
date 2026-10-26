"use strict";

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { spawnSync } = require("node:child_process");
const { Disposable, Emitter } = require("@lumine-code/event-kit");

const modulePath = require.resolve("../lib/keyboard-layout");
const nativePath = path.resolve(__dirname, "../build/Release/keyboard-layout-manager.node");
const wrapperSource = fs.readFileSync(modulePath, "utf8");

function runChild(source, { exposeGc = false } = {}) {
  const environment = { ...process.env };
  delete environment.CI;
  const result = spawnSync(process.execPath, [...(exposeGc ? ["--expose-gc"] : []), "-e", source], {
    env: environment,
    encoding: "utf8",
    timeout: 15000,
  });
  const details = [result.error?.message, result.stdout, result.stderr].filter(Boolean).join("\n");
  expect(result.error).withContext(details).toBeUndefined();
  expect(result.signal).withContext(details).toBeNull();
  expect(result.status).withContext(details).toBe(0);
}

function loadWrapper(platform = "linux") {
  const native = {
    layout: "same-layout",
    generation: 1,
    keymapReads: 0,
    dispose: jasmine.createSpy("native dispose"),
  };
  class KeyboardLayoutManager {
    constructor(callback) {
      native.notify = callback;
    }

    getCurrentKeyboardLayout() {
      return native.layout;
    }

    getCurrentKeyboardLanguage() {
      return "en";
    }

    getInstalledKeyboardLanguages() {
      return ["en", "en", "pl"];
    }

    getCurrentKeymap() {
      native.keymapReads++;
      return { KeyA: { unmodified: String(native.generation) } };
    }

    dispose() {
      native.dispose();
    }
  }

  const timers = new Map();
  const unref = jasmine.createSpy("timer unref");
  const sandbox = {
    module: { exports: {} },
    process: { platform },
    require(id) {
      if (id === "@lumine-code/event-kit") return { Disposable, Emitter };
      if (id === "node:timers") {
        return { setInterval: sandbox.setInterval, clearInterval: sandbox.clearInterval };
      }
      if (id.endsWith("keyboard-layout-manager.node")) return { KeyboardLayoutManager };
      throw new Error(`Unexpected wrapper dependency: ${id}`);
    },
    setInterval(callback) {
      const handle = { unref };
      timers.set(handle, callback);
      return handle;
    },
    clearInterval(handle) {
      timers.delete(handle);
    },
  };
  vm.runInNewContext(wrapperSource, sandbox, { filename: modulePath });
  return { api: sandbox.module.exports, native, timers, unref };
}

describe("Native keyboard layout lifecycle", () => {
  it("collects multiple native managers before the environment exits", () => {
    runChild(
      `
      const { KeyboardLayoutManager } = require(${JSON.stringify(nativePath)});
      for (let index = 0; index < 8; index++) new KeyboardLayoutManager(() => {});
      global.gc();
      setImmediate(() => {
        global.gc();
        setImmediate(() => global.gc());
      });
      `,
      { exposeGc: true },
    );
  });

  it("rejects invalid constructor arguments without breaking finalization", () => {
    runChild(
      `
      const assert = require("node:assert/strict");
      const { KeyboardLayoutManager } = require(${JSON.stringify(nativePath)});
      for (const argument of [undefined, null, false, 1, "callback", {}]) {
        assert.throws(() => new KeyboardLayoutManager(argument), TypeError);
      }
      global.gc();
      setImmediate(() => global.gc());
      `,
      { exposeGc: true },
    );
  });

  it("disposes a native manager repeatedly and rejects subsequent queries", () => {
    runChild(
      `
      const assert = require("node:assert/strict");
      const { KeyboardLayoutManager } = require(${JSON.stringify(nativePath)});
      let manager = new KeyboardLayoutManager(() => {});
      manager.dispose();
      manager.dispose();
      for (const method of [
        "getCurrentKeymap", "getCurrentKeyboardLayout",
        "getCurrentKeyboardLanguage", "getInstalledKeyboardLanguages"
      ]) assert.throws(() => manager[method]());
      manager = null;
      global.gc();
      setImmediate(() => global.gc());
      `,
      { exposeGc: true },
    );
  });

  it("can dispose and reload the public module in one environment", () => {
    runChild(
      `
      const modulePath = ${JSON.stringify(modulePath)};
      for (let index = 0; index < 6; index++) {
        const layout = require(modulePath);
        layout.getCurrentKeymap();
        layout.dispose();
        delete require.cache[modulePath];
        global.gc();
      }
      setImmediate(() => global.gc());
      `,
      { exposeGc: true },
    );
  });

  it("allows multiple workers to read layouts and exit normally", () => {
    runChild(`
      const assert = require("node:assert/strict");
      const { Worker } = require("node:worker_threads");
      const source = ${JSON.stringify(`
        const layout = require(${JSON.stringify(modulePath)});
        layout.observeCurrentKeyboardLayout(() => {});
        layout.getCurrentKeyboardLayout();
        layout.getCurrentKeymap();
      `)};
      Promise.all(Array.from({ length: 4 }, () => new Promise((resolve, reject) => {
        const worker = new Worker(source, { eval: true });
        worker.once("error", reject);
        worker.once("exit", (code) => {
          try { assert.equal(code, 0); resolve(); } catch (error) { reject(error); }
        });
      }))).catch((error) => { console.error(error); process.exitCode = 1; });
    `);
  });

  it("cleans up workers terminated while a layout observer is active", () => {
    runChild(`
      const assert = require("node:assert/strict");
      const { Worker } = require("node:worker_threads");
      const source = ${JSON.stringify(`
        const { parentPort } = require("node:worker_threads");
        const layout = require(${JSON.stringify(modulePath)});
        layout.observeCurrentKeyboardLayout(() => {});
        layout.getCurrentKeymap();
        setInterval(() => {}, 1000);
        parentPort.postMessage("ready");
      `)};
      (async () => {
        for (let index = 0; index < 4; index++) {
          const worker = new Worker(source, { eval: true });
          await new Promise((resolve, reject) => {
            worker.once("message", resolve);
            worker.once("error", reject);
          });
          assert.equal(await worker.terminate(), 1);
        }
      })().catch((error) => { console.error(error); process.exitCode = 1; });
    `);
  });
});

describe("Keyboard layout wrapper lifecycle", () => {
  it("refreshes a cached keymap before notifying observers of a same-name change", () => {
    const { api, native } = loadWrapper();
    const first = api.getCurrentKeymap();
    expect(api.getCurrentKeymap()).toBe(first);
    expect(native.keymapReads).toBe(1);

    const observer = jasmine.createSpy("layout observer").and.callFake(() => {
      expect(api.getCurrentKeymap().KeyA.unmodified).toBe("2");
    });
    const subscription = api.onDidChangeCurrentKeyboardLayout(observer);
    native.generation = 2;
    native.notify();

    expect(observer).toHaveBeenCalledOnceWith("same-layout");
    expect(api.getCurrentKeymap()).not.toBe(first);
    expect(native.keymapReads).toBe(2);
    subscription.dispose();
    api.dispose();
  });

  it("disposes native resources once and rejects new work after disposal", () => {
    const { api, native } = loadWrapper();
    const observer = jasmine.createSpy("layout observer");
    const subscription = api.onDidChangeCurrentKeyboardLayout(observer);
    api.getCurrentKeymap();
    api.dispose();
    api.dispose();
    subscription.dispose();
    native.notify();

    expect(native.dispose).toHaveBeenCalledTimes(1);
    for (const method of [
      "getCurrentKeymap",
      "getCurrentKeyboardLayout",
      "getCurrentKeyboardLanguage",
      "getInstalledKeyboardLanguages",
    ]) {
      expect(() => api[method]())
        .withContext(method)
        .toThrow();
    }
    expect(() => api.onDidChangeCurrentKeyboardLayout(observer)).toThrow();
    expect(() => api.observeCurrentKeyboardLayout(observer)).toThrow();
    expect(observer).not.toHaveBeenCalled();
  });

  it("polls Windows layouts only while subscribed and unreferences the timer", () => {
    const { api, native, timers, unref } = loadWrapper("win32");
    expect(timers.size).toBe(0);
    const observer = jasmine.createSpy("layout observer");
    const first = api.onDidChangeCurrentKeyboardLayout(observer);
    const second = api.onDidChangeCurrentKeyboardLayout(() => {});
    expect(timers.size).toBe(1);
    expect(unref).toHaveBeenCalledTimes(1);

    const tick = [...timers.values()][0];
    tick();
    expect(observer).not.toHaveBeenCalled();
    native.layout = "another-layout";
    tick();
    tick();
    expect(observer).toHaveBeenCalledOnceWith("another-layout");

    first.dispose();
    first.dispose();
    expect(timers.size).toBe(1);
    second.dispose();
    expect(timers.size).toBe(0);
    api.onDidChangeCurrentKeyboardLayout(() => {});
    expect(timers.size).toBe(1);
    api.dispose();
    expect(timers.size).toBe(0);
  });
});
