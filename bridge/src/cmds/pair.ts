// `warlock-buddy pair` — scan, prompt to pick, connect, save.
//
// macOS handles the passkey UI automatically when we touch an encrypted
// characteristic (NUS RX/TX are encrypted-only on the firmware side).
// User sees a passkey on the stick + a system pairing prompt on the Mac.

import { scanDevices, attachById } from "../ble.ts";
import { upsertDevice } from "../config.ts";

async function prompt(question: string): Promise<string> {
  process.stdout.write(question);
  const decoder = new TextDecoder();
  for await (const chunk of Bun.stdin.stream()) {
    return decoder.decode(chunk).trim();
  }
  return "";
}

export async function runPair(): Promise<void> {
  console.log("scanning 10s for advertising sticks…");
  const found = await scanDevices(10_000);

  if (found.length === 0) {
    console.error("no warlock-* devices found.");
    console.error("→ power on a stick (press A) and try again.");
    console.error("→ if it's bonded to another machine (e.g. Claude desktop), forget it there first.");
    process.exit(1);
  }

  console.log("\nfound:");
  found.forEach((d, i) => {
    console.log(`  [${i + 1}] ${d.name}  rssi=${d.rssi}  id=${d.id.slice(0, 8)}…`);
  });

  const pick = found.length === 1
    ? "1"
    : await prompt(`\npick [1-${found.length}]: `);
  const idx = parseInt(pick, 10) - 1;
  if (isNaN(idx) || idx < 0 || idx >= found.length) {
    console.error("invalid selection");
    process.exit(1);
  }
  const device = found[idx];
  console.log(`\nconnecting to ${device.name}…`);
  console.log("(if macOS prompts for a passkey, enter the 6 digits shown on the stick)");

  const link = await attachById(device.id);
  console.log(`✓ connected to ${link.name}`);
  await upsertDevice(link.name, link.id);
  console.log(`✓ saved to config`);
  await link.disconnect();
  console.log(`\nstart the bridge with:  warlock-buddy daemon`);
}
