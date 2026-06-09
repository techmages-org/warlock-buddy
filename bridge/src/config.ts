// On-disk config for warlock-buddy. Lives at ~/.config/warlock-buddy/config.json.
// Holds the list of known devices so `list` / `forget` / `daemon` don't need
// to rediscover every time.

import { homedir } from "node:os";
import { join, dirname } from "node:path";
import { mkdir } from "node:fs/promises";

const CONFIG_DIR = join(homedir(), ".config", "warlock-buddy");
const CONFIG_PATH = join(CONFIG_DIR, "config.json");

export interface Device {
  name: string;          // BLE advertised local name, e.g. "warlock-E4C1"
  id: string;            // peripheral id (macOS UUID, not the BT MAC)
  label?: string;        // user-friendly label, optional
  first_seen: string;    // ISO timestamp
  last_seen: string;     // ISO timestamp
}

export interface Config {
  version: 1;
  devices: Device[];
}

const DEFAULT: Config = { version: 1, devices: [] };

export async function readConfig(): Promise<Config> {
  const f = Bun.file(CONFIG_PATH);
  if (!(await f.exists())) return { ...DEFAULT };
  try {
    const body = (await f.json()) as Config;
    if (body.version !== 1) return { ...DEFAULT, ...body, version: 1 };
    return body;
  } catch {
    return { ...DEFAULT };
  }
}

export async function writeConfig(c: Config): Promise<void> {
  await mkdir(dirname(CONFIG_PATH), { recursive: true });
  await Bun.write(CONFIG_PATH, JSON.stringify(c, null, 2) + "\n");
}

export async function upsertDevice(name: string, id: string): Promise<Device> {
  const cfg = await readConfig();
  const now = new Date().toISOString();
  let dev = cfg.devices.find((d) => d.id === id);
  if (dev) {
    dev.last_seen = now;
    dev.name = name; // name might change if firmware rebranded
  } else {
    dev = { name, id, first_seen: now, last_seen: now };
    cfg.devices.push(dev);
  }
  await writeConfig(cfg);
  return dev;
}

export async function forgetDevice(nameOrId: string): Promise<boolean> {
  const cfg = await readConfig();
  const before = cfg.devices.length;
  cfg.devices = cfg.devices.filter((d) => d.id !== nameOrId && d.name !== nameOrId);
  await writeConfig(cfg);
  return cfg.devices.length < before;
}

export function configPath(): string {
  return CONFIG_PATH;
}
