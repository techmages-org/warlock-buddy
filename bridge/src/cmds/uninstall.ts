// `warlock-buddy uninstall` — unload the launchd agent and remove the
// plist. Inverse of `install`.

import { unlink } from "node:fs/promises";
import { LABEL, plistPath } from "./install.ts";

export async function runUninstall(): Promise<void> {
  const target = `gui/${process.getuid?.() ?? 501}/${LABEL}`;
  const proc = Bun.spawn(["launchctl", "bootout", target], {
    stdout: "pipe",
    stderr: "pipe",
  });
  const code = await proc.exited;
  if (code === 0) {
    console.log(`✓ launchd unloaded ${LABEL}`);
  } else {
    console.log(`note: launchctl bootout returned ${code} (was likely already unloaded)`);
  }

  const f = Bun.file(plistPath());
  if (await f.exists()) {
    await unlink(plistPath());
    console.log(`✓ removed ${plistPath()}`);
  }

  console.log("");
  console.log("the bridge no longer starts at login.");
  console.log("run `warlock-buddy daemon` to start it manually for this session.");
}
