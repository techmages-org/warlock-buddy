// `warlock-buddy list` — show known devices + currently-advertising ones.
//
// Known = from ~/.config/warlock-buddy/config.json
// Live  = from a 5s BLE scan
// Output table merges them: a device can be known+live, known-only, or live-only.

import { readConfig, configPath } from "../config.ts";
import { scanDevices } from "../ble.ts";

export async function runList(): Promise<void> {
  const cfg = await readConfig();
  console.log(`config: ${configPath()}`);
  console.log("scanning 5s for live devices…\n");

  const live = await scanDevices(5_000);
  const liveById = new Map(live.map((d) => [d.id, d]));

  const known = cfg.devices;
  const knownIds = new Set(known.map((d) => d.id));
  const liveOnly = live.filter((d) => !knownIds.has(d.id));

  if (known.length === 0 && live.length === 0) {
    console.log("no devices known and none in range.");
    console.log("→ run `warlock-buddy pair` after powering on a stick.");
    return;
  }

  const fmt = (s: string, n: number) => s.padEnd(n).slice(0, n);
  console.log(fmt("NAME", 18) + fmt("STATUS", 10) + fmt("RSSI", 8) + "LAST SEEN");
  console.log("-".repeat(70));

  for (const k of known) {
    const live = liveById.get(k.id);
    const status = live ? "live" : "offline";
    const rssi = live ? `${live.rssi}` : "—";
    console.log(fmt(k.name, 18) + fmt(status, 10) + fmt(rssi, 8) + k.last_seen);
  }
  for (const l of liveOnly) {
    console.log(fmt(l.name, 18) + fmt("unpaired", 10) + fmt(`${l.rssi}`, 8) + "(new)");
  }
}
