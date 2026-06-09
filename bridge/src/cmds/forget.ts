// `warlock-buddy forget <name>` — remove device from config.
//
// Note: this does NOT remove the macOS BLE bond. To fully unpair, also
// run: System Settings → Bluetooth → right-click device → Forget.
// Or factory-reset the stick: Hold A → settings → reset → factory reset.

import { forgetDevice } from "../config.ts";

export async function runForget(args: string[]): Promise<void> {
  const target = args[0];
  if (!target) {
    console.error("usage: warlock-buddy forget <name-or-id>");
    process.exit(1);
  }
  const removed = await forgetDevice(target);
  if (removed) {
    console.log(`✓ removed ${target} from config`);
    console.log("note: macOS Bluetooth bond not touched — Forget it in System Settings");
    console.log("      or factory-reset the stick if you want a fresh pairing.");
  } else {
    console.error(`no device matching "${target}" in config`);
    process.exit(1);
  }
}
