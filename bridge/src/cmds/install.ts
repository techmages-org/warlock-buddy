// `warlock-buddy install` — register a launchd LaunchAgent so the bridge
// daemon starts at login and restarts if it crashes or the BLE link
// drops. Pairs with `daemon` exiting non-zero on disconnect so launchd's
// KeepAlive policy respawns it.
//
// Plist lives at ~/Library/LaunchAgents/com.jbrashear.warlock-buddy.plist
// Logs at ~/Library/Logs/warlock-buddy/{out,err}.log

import { homedir } from "node:os";
import { join, dirname } from "node:path";
import { mkdir } from "node:fs/promises";

export const LABEL = "com.jbrashear.warlock-buddy";

export function plistPath(): string {
  return join(homedir(), "Library", "LaunchAgents", `${LABEL}.plist`);
}

function logDir(): string {
  return join(homedir(), "Library", "Logs", "warlock-buddy");
}

function plistBody(bunPath: string, cliPath: string, workDir: string, targetName?: string): string {
  const args = [bunPath, "run", cliPath, "daemon"];
  if (targetName) args.push("--device", targetName);
  const argXml = args
    .map((a) => `    <string>${escapeXml(a)}</string>`)
    .join("\n");
  return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>${LABEL}</string>
  <key>ProgramArguments</key>
  <array>
${argXml}
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>ThrottleInterval</key>
  <integer>10</integer>
  <key>WorkingDirectory</key>
  <string>${escapeXml(workDir)}</string>
  <key>StandardOutPath</key>
  <string>${escapeXml(join(logDir(), "out.log"))}</string>
  <key>StandardErrorPath</key>
  <string>${escapeXml(join(logDir(), "err.log"))}</string>
</dict>
</plist>
`;
}

function escapeXml(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

// Pick the bun binary we're currently running under. Falls back to the
// PATH lookup if the explicit env var isn't set (always set under Bun).
function bunBinary(): string {
  return process.execPath;
}

export async function runInstall(args: string[]): Promise<void> {
  let targetName: string | undefined;
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--device" && args[i + 1]) {
      targetName = args[i + 1];
      i++;
    }
  }

  const cliPath = new URL("../cli.ts", import.meta.url).pathname;
  const workDir = new URL("..", import.meta.url).pathname;
  const bunPath = bunBinary();
  const body = plistBody(bunPath, cliPath, workDir, targetName);

  await mkdir(logDir(), { recursive: true });
  await mkdir(dirname(plistPath()), { recursive: true });
  await Bun.write(plistPath(), body);
  console.log(`✓ wrote ${plistPath()}`);

  // Best-effort bootout: if it's already loaded, unload first so the new
  // plist takes effect on this `load`. Errors here are fine — usually
  // means the service wasn't loaded yet.
  await launchctl(["bootout", `gui/${process.getuid?.() ?? 501}/${LABEL}`], true);
  const code = await launchctl(["bootstrap", `gui/${process.getuid?.() ?? 501}`, plistPath()]);
  if (code !== 0) {
    console.error(`✗ launchctl bootstrap failed (code ${code})`);
    console.error(`  inspect with: launchctl print gui/$(id -u)/${LABEL}`);
    process.exit(code);
  }

  console.log(`✓ launchd loaded ${LABEL}`);
  console.log("");
  console.log("the bridge now starts at login and restarts on crash/disconnect.");
  console.log(`logs:  ~/Library/Logs/warlock-buddy/{out,err}.log`);
  console.log(`stop:  warlock-buddy uninstall`);
  console.log(`tail:  tail -f ~/Library/Logs/warlock-buddy/out.log`);
  console.log("");
  console.log("⚠  one-time macOS step:");
  console.log("   launchd-spawned processes need their own Bluetooth grant");
  console.log("   (separate from the one Terminal already has). Opening System");
  console.log("   Settings → Privacy & Security → Bluetooth now — toggle `bun`");
  console.log("   ON, then everything works on every boot from here forward.");

  // Open the BT pane directly so the user doesn't hunt for it.
  Bun.spawn(["open", "x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth"], {
    stdout: "ignore",
    stderr: "ignore",
  });

  console.log("");
  console.log("after granting:  launchctl kickstart -k gui/$(id -u)/" + LABEL);
  console.log("(or just unplug + replug the buddy; launchd auto-restarts and reconnects)");
}

async function launchctl(args: string[], swallow = false): Promise<number> {
  const proc = Bun.spawn(["launchctl", ...args], {
    stdout: swallow ? "pipe" : "inherit",
    stderr: swallow ? "pipe" : "inherit",
  });
  return await proc.exited;
}
