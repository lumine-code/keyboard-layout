"use strict";

if (process.platform === "linux") {
  const fs = require("node:fs");
  const net = require("node:net");
  const os = require("node:os");
  const path = require("node:path");
  const { spawn } = require("node:child_process");

  describe("Wayland initialization", () => {
    it("falls back to X11 when a compositor accepts the connection but sends no keymap", async () => {
      const runtimeDirectory = fs.mkdtempSync(path.join(os.tmpdir(), "keyboard-layout-stalled-"));
      fs.chmodSync(runtimeDirectory, 0o700);
      const sockets = new Set();
      const server = net.createServer((socket) => {
        sockets.add(socket);
        socket.on("close", () => sockets.delete(socket));
      });

      try {
        await new Promise((resolve, reject) => {
          server.once("error", reject);
          server.listen(path.join(runtimeDirectory, "stalled-compositor"), resolve);
        });
        const childEnv = {
          ...process.env,
          XDG_RUNTIME_DIR: runtimeDirectory,
          WAYLAND_DISPLAY: "stalled-compositor",
        };
        delete childEnv.WAYLAND_SOCKET;

        const modulePath = require.resolve("../lib/keyboard-layout");
        const child = spawn(
          process.execPath,
          [
            "-e",
            `const layout = require(${JSON.stringify(modulePath)}); if (!layout.getCurrentKeymap().KeyG) process.exitCode = 1;`,
          ],
          { env: childEnv, stdio: ["ignore", "ignore", "pipe"] },
        );
        let stderr = "";
        child.stderr.setEncoding("utf8");
        child.stderr.on("data", (chunk) => (stderr += chunk));
        const result = await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => {
            child.kill();
            reject(new Error("Keyboard layout initialization did not finish within eight seconds"));
          }, 8000);
          child.once("error", (error) => {
            clearTimeout(timeout);
            reject(error);
          });
          child.once("exit", (status, signal) => {
            clearTimeout(timeout);
            resolve({ status, signal });
          });
        });
        expect(result.signal).toBeNull();
        expect(result.status).withContext(stderr).toBe(0);
      } finally {
        for (const socket of sockets) socket.destroy();
        await new Promise((resolve) => server.close(resolve));
        fs.rmSync(runtimeDirectory, { recursive: true, force: true });
      }
    }, 12000);
  });
}
