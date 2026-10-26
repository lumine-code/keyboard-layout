"use strict";

if (process.platform === "linux") {
  const fs = require("node:fs");
  const os = require("node:os");
  const path = require("node:path");
  const { execFileSync, spawn } = require("node:child_process");

  describe("X11 keyboard layouts", () => {
    let directory;
    let displayServer;
    let displayEnv;
    let groupHelper;

    beforeAll(async () => {
      directory = fs.mkdtempSync(path.join(os.tmpdir(), "keyboard-layout-maps-"));
      fs.chmodSync(directory, 0o700);
      groupHelper = path.join(directory, "set-x11-group");
      execFileSync(
        "cc",
        [path.join(__dirname, "fixtures", "set-x11-group.c"), "-o", groupHelper, "-lX11"],
        { timeout: 10000 },
      );

      displayServer = spawn(
        "Xvfb",
        ["-displayfd", "1", "-screen", "0", "1024x768x24", "-noreset", "-nolisten", "tcp"],
        { stdio: ["ignore", "pipe", "pipe"] },
      );
      let diagnostics = "";
      displayServer.stderr.on("data", (chunk) => (diagnostics += chunk));
      const displayNumber = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Xvfb did not become ready")), 5000);
        let output = "";
        displayServer.stdout.setEncoding("utf8");
        displayServer.stdout.on("data", (chunk) => {
          output += chunk;
          if (output.includes("\n")) {
            clearTimeout(timeout);
            resolve(output.trim());
          }
        });
        displayServer.once("error", (error) => {
          clearTimeout(timeout);
          reject(error);
        });
        displayServer.once("exit", (status) => {
          clearTimeout(timeout);
          reject(new Error(`Xvfb exited with ${status}: ${diagnostics}`));
        });
      });

      displayEnv = {
        ...process.env,
        DISPLAY: `:${displayNumber}`,
        XDG_RUNTIME_DIR: directory,
        WAYLAND_DISPLAY: "missing-compositor",
      };
      delete displayEnv.WAYLAND_SOCKET;
      execFileSync("setxkbmap", ["-layout", "us,de,gb,fr", "-option", ""], {
        env: displayEnv,
        timeout: 5000,
      });
    }, 20000);

    afterAll(async () => {
      try {
        if (displayServer && displayServer.exitCode === null && displayServer.signalCode === null) {
          await new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              displayServer.kill("SIGKILL");
              reject(new Error("Xvfb did not stop"));
            }, 3000);
            displayServer.once("exit", () => {
              clearTimeout(timeout);
              resolve();
            });
            displayServer.kill();
          });
        }
      } finally {
        if (directory) fs.rmSync(directory, { recursive: true, force: true });
      }
    });

    function keymapForGroup(group) {
      execFileSync(groupHelper, [String(group)], { env: displayEnv, timeout: 5000 });
      const modulePath = require.resolve("../lib/keyboard-layout");
      const output = execFileSync(
        process.execPath,
        [
          "-e",
          `const layout = require(${JSON.stringify(modulePath)}); console.log(JSON.stringify({ layout: layout.getCurrentKeyboardLayout(), keymap: layout.getCurrentKeymap() }));`,
        ],
        { env: displayEnv, encoding: "utf8", timeout: 5000 },
      );
      return JSON.parse(output);
    }

    it("returns accented Latin-1 characters without UTF-8 replacement characters", () => {
      const result = keymapForGroup(1);
      expect(result.layout).toContain("[1]");
      expect(result.keymap.Semicolon).toEqual({ unmodified: "ö", withShift: "Ö" });
      expect(result.keymap.Quote).toEqual({ unmodified: "ä", withShift: "Ä" });
    });

    it("translates the fourth configured layout instead of silently using the first", () => {
      const result = keymapForGroup(3);
      expect(result.layout).toContain("[3]");
      expect(result.keymap.KeyQ).toEqual({ unmodified: "a", withShift: "A" });
    });
  });
}
