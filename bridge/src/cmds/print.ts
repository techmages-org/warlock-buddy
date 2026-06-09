// `warlock-buddy print` — poll the Warlock API once and print the heartbeat the
// daemon would send over BLE. Lets us validate the whole read+map path with NO
// hardware (just point WARLOCK_API_URL at a running deck or a local instance).

import { getDashboard, getEngagements, getAarStatus, apiUrl } from "../warlock.ts";
import { buildHeartbeat } from "../protocol.ts";

export async function runPrint(): Promise<void> {
  const [dash, engs, aar] = await Promise.all([
    getDashboard(),
    getEngagements(),
    getAarStatus(),
  ]);
  if (!dash) {
    console.error(`[print] deck API unreachable at ${apiUrl()} (set WARLOCK_API_URL / _USER / _PASS)`);
  }
  const hb = buildHeartbeat(dash, engs, aar);
  console.log(JSON.stringify(hb, null, 2));
}
