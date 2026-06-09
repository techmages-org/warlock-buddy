#!/usr/bin/env bun
// warlock-buddy CLI — dispatch to subcommand.

import { runDaemon } from "./cmds/daemon.ts";
import { runList } from "./cmds/list.ts";
import { runPair } from "./cmds/pair.ts";
import { runForget } from "./cmds/forget.ts";
import { runFlash } from "./cmds/flash.ts";
import { runInstall } from "./cmds/install.ts";
import { runUninstall } from "./cmds/uninstall.ts";
import { runPrint } from "./cmds/print.ts";
import {
  getEngagements,
  activateEngagement,
  endEngagement,
  killswitch,
  apiUrl,
} from "./warlock.ts";

const USAGE = `warlock-buddy — hardware companion for the Warlock deck

Usage:
  warlock-buddy daemon [--device N]   Run the bridge (poll Warlock → BLE).
  warlock-buddy install [--device N]  Register a launchd agent (start at login,
                                      restart on crash/disconnect).
  warlock-buddy uninstall             Unload the launchd agent + remove plist.
  warlock-buddy pair                  Scan, pick a device, bond, save.
  warlock-buddy list                  Show known + live devices.
  warlock-buddy forget <name|id>      Remove device from config.
  warlock-buddy flash [port]          Build + flash firmware over USB.

  -- deck (no hardware needed; honors WARLOCK_API_URL/_USER/_PASS) --
  warlock-buddy print                 Poll Warlock once, print the heartbeat.
  warlock-buddy engagements           List engagements (id / name / status).
  warlock-buddy arm <id>              ARM (activate) a staged engagement.
  warlock-buddy end <id>              END the active engagement.
  warlock-buddy killswitch            Abort all offensive ops + restore ifaces.

  warlock-buddy --help                This help.

Config: ~/.config/warlock-buddy/config.json
Logs (when installed): ~/Library/Logs/warlock-buddy/
Firmware: ../firmware/ and ../firmware-cores3/ (PlatformIO projects)
Deck API: ${apiUrl()}
`;

async function runEngagements(): Promise<void> {
  const list = await getEngagements();
  if (!list.length) {
    console.log("(no engagements — create one on the deck first)");
    return;
  }
  for (const e of list) {
    console.log(`${e.status.padEnd(7)}  ${e.id}  ${e.name}`);
  }
}

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  switch (cmd) {
    case "daemon":      return await runDaemon(rest);
    case "install":     return await runInstall(rest);
    case "uninstall":   return await runUninstall();
    case "list":        return await runList();
    case "pair":        return await runPair();
    case "forget":      return await runForget(rest);
    case "flash":       return await runFlash(rest);
    case "print":       return await runPrint();
    case "engagements": return await runEngagements();
    case "arm": {
      if (!rest[0]) { console.error("usage: warlock-buddy arm <engagement-id>"); process.exit(1); }
      console.log(await activateEngagement(rest[0]) ? `✓ ARMED ${rest[0]}` : `✗ activate failed`);
      return;
    }
    case "end": {
      if (!rest[0]) { console.error("usage: warlock-buddy end <engagement-id>"); process.exit(1); }
      console.log(await endEngagement(rest[0]) ? `✓ ended ${rest[0]}` : `✗ end failed`);
      return;
    }
    case "killswitch":
      console.log(await killswitch() ? "✓ KILLSWITCH fired" : "✗ killswitch failed");
      return;
    case undefined:
    case "--help":
    case "-h":
    case "help":
      process.stdout.write(USAGE);
      return;
    default:
      console.error(`unknown subcommand: ${cmd}`);
      process.stdout.write("\n" + USAGE);
      process.exit(1);
  }
}

main()
  .then(() => {
    // The daemon subcommand runs forever (SIGINT-only exit). Other subcommands
    // finish and should exit cleanly — noble keeps the BLE adapter open which
    // holds the event loop, so we have to force exit.
    if (process.argv[2] !== "daemon") process.exit(0);
  })
  .catch((err) => {
    console.error("✗", err instanceof Error ? err.message : err);
    process.exit(1);
  });
