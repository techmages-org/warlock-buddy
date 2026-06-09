// `warlock-buddy flash [port]` — build + flash firmware over USB.
//
// Auto-detects the serial port if not given. Shells out to PlatformIO,
// expects `pio` to be on PATH (installed via `pipx install platformio` or
// the official installer).

import { readdir } from "node:fs/promises";
import { join } from "node:path";

const FIRMWARE_DIR = new URL("../../../firmware", import.meta.url).pathname;

async function findPort(): Promise<string | null> {
  try {
    const entries = await readdir("/dev");
    const matches = entries.filter((n) =>
      n.startsWith("cu.usbserial-") ||
      n.startsWith("cu.wchusbserial") ||
      n.startsWith("cu.SLAB_USBtoUART") ||
      n.startsWith("cu.usbmodem")
    );
    if (matches.length === 0) return null;
    if (matches.length > 1) {
      console.error(`multiple ports found, specify one:`);
      matches.forEach((m) => console.error(`  /dev/${m}`));
      return null;
    }
    return `/dev/${matches[0]}`;
  } catch {
    return null;
  }
}

export async function runFlash(args: string[]): Promise<void> {
  const port = args[0] ?? (await findPort());
  if (!port) {
    console.error("no serial port found. plug in the stick and try again,");
    console.error("or pass explicitly:  warlock-buddy flash /dev/cu.usbserial-XXXX");
    process.exit(1);
  }
  console.log(`flashing firmware via ${port}…`);
  console.log(`(this builds + uploads — first run downloads PlatformIO platform deps, can take a few minutes)\n`);

  const proc = Bun.spawn(
    ["pio", "run", "-t", "upload", "--upload-port", port],
    {
      cwd: FIRMWARE_DIR,
      stdout: "inherit",
      stderr: "inherit",
    },
  );
  const code = await proc.exited;
  if (code !== 0) {
    console.error(`\n✗ flash failed (pio exit code ${code})`);
    process.exit(code);
  }
  console.log(`\n✓ flashed. wake the stick (press A) then run:`);
  console.log(`    warlock-buddy pair`);
}
