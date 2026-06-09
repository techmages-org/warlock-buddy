// Thin wrapper around @abandonware/noble. We act as BLE central, the M5
// device is the peripheral advertising the Nordic UART Service exactly like
// the Claude desktop apps expect.
//
// Service / characteristic UUIDs match claude-desktop-buddy/REFERENCE.md.

import noble, { type Peripheral } from "@abandonware/noble";

const NUS_SERVICE = "6e400001b5a3f393e0a9e50e24dcca9e";
const NUS_RX      = "6e400002b5a3f393e0a9e50e24dcca9e"; // write to device
const NUS_TX      = "6e400003b5a3f393e0a9e50e24dcca9e"; // notify from device

// The warlock-buddy firmware advertises under "warlock-XXXX". (If you flash a
// CoreS3 that previously ran subctl-buddy, macOS may briefly cache the old
// localName — power-cycle the device to clear it.)
const NAME_PREFIXES = ["warlock"];

export interface BuddyLink {
  name: string;
  id: string;
  write(obj: unknown): Promise<void>;
  onLine(cb: (obj: unknown) => void): void;
  // Fires when the BLE link drops (device powered off, unpaired, walked
  // out of range). The supervisor (launchd) is expected to restart the
  // daemon; in-process reconnect would otherwise pile up event listeners
  // on every failed write attempt.
  onDisconnect(cb: () => void): void;
  disconnect(): Promise<void>;
}

type Periph = Peripheral;

async function attach(peripheral: Periph): Promise<BuddyLink | null> {
  await peripheral.connectAsync();
  const { characteristics } = await peripheral.discoverSomeServicesAndCharacteristicsAsync(
    [NUS_SERVICE],
    [NUS_RX, NUS_TX],
  );

  const rx = characteristics.find((c) => c.uuid === NUS_RX);
  const tx = characteristics.find((c) => c.uuid === NUS_TX);
  if (!rx || !tx) {
    await peripheral.disconnectAsync();
    return null;
  }

  await tx.subscribeAsync();

  // MTU on macOS central typically negotiates ~185; ATT payload = MTU - 3.
  // We don't have noble's negotiated value; conservative 180 matches the
  // firmware's own chunking in ble_bridge.cpp.
  const CHUNK = 180;

  const name = peripheral.advertisement.localName || peripheral.id;
  let lineBuf = "";
  const lineCbs: Array<(obj: unknown) => void> = [];
  const disconnectCbs: Array<() => void> = [];

  peripheral.on("disconnect", () => {
    for (const cb of disconnectCbs) cb();
  });

  tx.on("data", (data: Buffer) => {
    lineBuf += data.toString("utf8");
    let nl: number;
    while ((nl = lineBuf.indexOf("\n")) >= 0) {
      const line = lineBuf.slice(0, nl).trim();
      lineBuf = lineBuf.slice(nl + 1);
      if (!line) continue;
      try {
        const obj = JSON.parse(line);
        for (const cb of lineCbs) cb(obj);
      } catch {
        // device may emit non-JSON debug; ignore
      }
    }
  });

  return {
    name,
    id: peripheral.id,
    async write(obj: unknown) {
      const line = JSON.stringify(obj) + "\n";
      const buf = Buffer.from(line, "utf8");
      for (let i = 0; i < buf.length; i += CHUNK) {
        const slice = buf.subarray(i, Math.min(i + CHUNK, buf.length));
        // withoutResponse=false: get confirmation, avoids overrunning device RX.
        await rx.writeAsync(slice, false);
      }
    },
    onLine(cb) {
      lineCbs.push(cb);
    },
    onDisconnect(cb) {
      disconnectCbs.push(cb);
    },
    async disconnect() {
      try {
        await peripheral.disconnectAsync();
      } catch { /* swallow */ }
    },
  };
}

export interface ScannedDevice {
  name: string;
  id: string;
  rssi: number;
}

// Scan for `timeoutMs` and return everything matching NAME_PREFIXES seen at
// least once during the window. Doesn't attach to anything. Use for `list`
// and `pair`'s interactive picker.
export async function scanDevices(timeoutMs = 5_000): Promise<ScannedDevice[]> {
  await waitFor(noble, "poweredOn");
  const seen = new Map<string, ScannedDevice>();

  return new Promise((resolve) => {
    const onDiscover = (peripheral: Periph) => {
      const name = peripheral.advertisement.localName ?? "";
      if (!NAME_PREFIXES.some((p) => name.startsWith(p))) return;
      seen.set(peripheral.id, { name, id: peripheral.id, rssi: peripheral.rssi });
    };
    noble.on("discover", onDiscover);
    noble.startScanning([NUS_SERVICE], true /* allowDuplicates */);

    setTimeout(() => {
      noble.stopScanning();
      noble.removeListener("discover", onDiscover);
      resolve([...seen.values()].sort((a, b) => b.rssi - a.rssi));
    }, timeoutMs);
  });
}

// Connect to a specific peripheral by id (the noble UUID, not the BT MAC).
// Used by `pair` after the user has picked one from scanDevices().
export async function attachById(id: string, timeoutMs = 15_000): Promise<BuddyLink> {
  await waitFor(noble, "poweredOn");

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      noble.stopScanning();
      reject(new Error(`device ${id} not seen in ${timeoutMs}ms`));
    }, timeoutMs);

    noble.on("discover", async (peripheral) => {
      if (peripheral.id !== id) return;
      noble.stopScanning();
      clearTimeout(timer);
      try {
        const link = await attach(peripheral);
        if (link) resolve(link);
        else reject(new Error("NUS characteristics not found"));
      } catch (err) {
        reject(err);
      }
    });

    noble.startScanning([NUS_SERVICE], false);
  });
}

// Discover the first matching peripheral. If `targetName` is given, only
// devices whose advertised localName matches exactly are accepted; otherwise
// any warlock-* device is fair game.
//
// `timeoutMs = 0` → scan forever. The daemon uses this so that when the
// device is powered off, the daemon keeps scanning instead of giving up;
// when the device comes back, we discover + connect within ~one BLE
// advertising interval (≤1s).
export async function discoverFirstBuddy(
  timeoutMs = 30_000,
  targetName?: string,
): Promise<BuddyLink> {
  await waitFor(noble, "poweredOn");

  return new Promise((resolve, reject) => {
    const timer = timeoutMs > 0
      ? setTimeout(() => {
          noble.stopScanning();
          reject(new Error(
            targetName
              ? `device ${targetName} not seen in ${timeoutMs}ms`
              : `no buddy found in ${timeoutMs}ms`,
          ));
        }, timeoutMs)
      : null;

    noble.on("discover", async (peripheral) => {
      const name = peripheral.advertisement.localName ?? "";
      if (targetName) {
        if (name !== targetName) return;
      } else {
        if (!NAME_PREFIXES.some((p) => name.startsWith(p))) return;
      }
      console.log(`[ble] found ${name} (${peripheral.id})`);
      noble.stopScanning();
      if (timer) clearTimeout(timer);
      try {
        const link = await attach(peripheral);
        if (link) resolve(link);
        else reject(new Error("NUS characteristics not found"));
      } catch (err) {
        reject(err);
      }
    });

    noble.startScanning([NUS_SERVICE], false);
  });
}

function waitFor(emitter: typeof noble, state: string): Promise<void> {
  // noble's type defs expose `_state`; `state` is the real runtime property.
  if ((emitter as unknown as { state: string }).state === state) return Promise.resolve();
  return new Promise((resolve) => {
    const handler = (s: string) => {
      if (s === state) {
        emitter.removeListener("stateChange", handler);
        resolve();
      }
    };
    emitter.on("stateChange", handler);
  });
}
