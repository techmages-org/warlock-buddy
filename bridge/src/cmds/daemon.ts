// warlock-buddy bridge — main loop.
//
// 1. discover + connect to the first BLE peripheral advertising NUS with a
//    name starting with "warlock-"
// 2. poll Warlock: dashboard/status (2s), engagements + aar (5s)
// 3. on any change, push a heartbeat over BLE
// 4. on an incoming gate command from the device, drive the engagement gate:
//      {cmd:"arm", id}      → POST /api/engagements/{id}/activate
//      {cmd:"end", id}      → POST /api/engagements/{id}/end
//      {cmd:"killswitch"}   → POST /api/engagements/killswitch

import { discoverFirstBuddy, type BuddyLink } from "../ble.ts";
import {
  getDashboard,
  getEngagements,
  getAarStatus,
  activateEngagement,
  endEngagement,
  killswitch,
} from "../warlock.ts";
import { buildHeartbeat, type Heartbeat } from "../protocol.ts";
import { upsertDevice } from "../config.ts";

const TICK_STATUS = 2000; // dashboard/status (engagement + health)
const TICK_META    = 5000; // engagements list + aar status
const KEEPALIVE    = 10_000;

let lastHbJson = "";
let lastSentAt = 0;

async function pushIfChanged(link: BuddyLink, hb: Heartbeat) {
  const j = JSON.stringify(hb);
  const now = Date.now();
  // Send on change, or every KEEPALIVE — keeps the firmware's stale-link
  // detector happy.
  if (j === lastHbJson && now - lastSentAt < KEEPALIVE) return;
  const reason = j === lastHbJson ? "keepalive" : "changed";
  lastHbJson = j;
  lastSentAt = now;
  try {
    await link.write(hb);
    console.log(`[hb] ${reason} → ${j.length}B (${hb.warlock?.persona})`);
  } catch (e) {
    console.error(`[hb] write FAILED:`, e);
    throw e;
  }
}

async function sendTime(link: BuddyLink) {
  const now = Math.floor(Date.now() / 1000);
  const tzOffset = -new Date().getTimezoneOffset() * 60;
  await link.write({ time: [now, tzOffset] });
}

async function sendOwner(link: BuddyLink) {
  const owner = process.env.WARLOCK_OWNER_NAME || process.env.USER || "operator";
  await link.write({ cmd: "owner", name: owner });
}

async function handleIncoming(obj: unknown) {
  if (!obj || typeof obj !== "object") return;
  const o = obj as Record<string, unknown>;

  // Engagement-gate actions — all audited server-side by Warlock.
  if (o.cmd === "arm" && typeof o.id === "string") {
    console.log(`[gate] ARM engagement ${o.id}`);
    const ok = await activateEngagement(o.id);
    console.log(`[gate] activate ack: ${ok}`);
    return;
  }
  if (o.cmd === "end" && typeof o.id === "string") {
    console.log(`[gate] END engagement ${o.id}`);
    const ok = await endEngagement(o.id);
    console.log(`[gate] end ack: ${ok}`);
    return;
  }
  if (o.cmd === "killswitch") {
    console.log(`[gate] KILLSWITCH`);
    const ok = await killswitch();
    console.log(`[gate] killswitch ack: ${ok}`);
    return;
  }

  // Acks for our commands (owner/time/etc) — just log.
  if (typeof o.ack === "string") {
    console.log(`[ack] ${o.ack} ok=${o.ok}`);
  }
}

export async function runDaemon(args: string[] = []): Promise<void> {
  // Optional: --device <name> targets a specific peripheral by localName.
  let targetName: string | undefined;
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "--device" && args[i + 1]) {
      targetName = args[i + 1];
      i++;
    }
  }
  // Env fallback so the systemd unit on the deck needs no CLI args.
  if (!targetName && process.env.WARLOCK_BUDDY_DEVICE) {
    targetName = process.env.WARLOCK_BUDDY_DEVICE;
  }

  console.log("[bridge] warlock-buddy starting");
  console.log(targetName
    ? `[bridge] scanning continuously for ${targetName}…`
    : "[bridge] scanning continuously for warlock-* BLE peripherals…");

  // timeoutMs=0 → scan forever; reconnect is ~1s after the device re-advertises.
  const link = await discoverFirstBuddy(0, targetName);
  console.log(`[bridge] connected: ${link.name}`);
  await upsertDevice(link.name, link.id);
  link.onLine(handleIncoming);

  // BLE link dropped — exit so the supervisor (launchd) restarts us.
  link.onDisconnect(() => {
    console.error(`[bridge] BLE link to ${link.name} dropped — exiting (supervisor restarts)`);
    process.exit(1);
  });

  try { await sendTime(link); } catch (e) { console.error(`[bridge] sendTime FAILED:`, e); throw e; }
  try { await sendOwner(link); } catch (e) { console.error(`[bridge] sendOwner FAILED:`, e); throw e; }

  // Cached views — each updater refreshes its slice on its own cadence.
  let cachedDash: Awaited<ReturnType<typeof getDashboard>> = null;
  let cachedEngs: Awaited<ReturnType<typeof getEngagements>> = [];
  let cachedAar:  Awaited<ReturnType<typeof getAarStatus>>  = null;

  const tickStatus = setInterval(async () => {
    cachedDash = await getDashboard();
    await pushIfChanged(link, buildHeartbeat(cachedDash, cachedEngs, cachedAar));
  }, TICK_STATUS);

  const tickMeta = setInterval(async () => {
    cachedEngs = await getEngagements();
    cachedAar = await getAarStatus();
    await pushIfChanged(link, buildHeartbeat(cachedDash, cachedEngs, cachedAar));
  }, TICK_META);

  // First-pass populate so we send something within ~1s of connect.
  cachedDash = await getDashboard();
  cachedEngs = await getEngagements();
  cachedAar = await getAarStatus();
  await pushIfChanged(link, buildHeartbeat(cachedDash, cachedEngs, cachedAar));

  const shutdown = async () => {
    console.log("\n[bridge] shutting down");
    clearInterval(tickStatus);
    clearInterval(tickMeta);
    await link.disconnect();
    process.exit(0);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}
