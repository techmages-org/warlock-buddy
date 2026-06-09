// Thin client for the Warlock FastAPI backend.
//
// The buddy mirrors Warlock's engagement state and lets the operator drive the
// engagement gate (ARM / END / KILLSWITCH) with a physical tap — every action
// lands on Warlock's existing, audited endpoints, so the buddy adds ZERO new
// backend surface (and never trips the "must land on TUI + web" rule).
//
// Reads (all HTTP Basic auth):
//   GET /api/dashboard/status   composite: engagement + system health, 1 poll
//   GET /api/engagements        list (drafts → the ARM picker; active; ended)
//   GET /api/aar/status         signed-attestation count (drives the level-up loop)
//
// Gate actions (device-initiated, audited server-side):
//   POST /api/engagements/{id}/activate   ARM a staged engagement
//   POST /api/engagements/{id}/end        END the active engagement
//   POST /api/engagements/killswitch      abort all offensive ops + restore ifaces
//
// Config via env (the deck is usually remote, so the URL must be overridable):
//   WARLOCK_API_URL   default http://127.0.0.1:7777
//   WARLOCK_API_USER  default warlock
//   WARLOCK_API_PASS  default warlock

const API_URL = (process.env.WARLOCK_API_URL ?? "http://127.0.0.1:7777").replace(/\/$/, "");
const API_USER = process.env.WARLOCK_API_USER ?? "warlock";
const API_PASS = process.env.WARLOCK_API_PASS ?? "warlock";
const AUTH = "Basic " + btoa(`${API_USER}:${API_PASS}`);

export function apiUrl(): string {
  return API_URL;
}

// --- response shapes (verified against the live API, 2026-06-09) -------------

export interface Scope {
  ssids: string[];
  bssids: string[];
  ip_ranges: string[];
}

export interface EngagementStatus {
  mode: "off" | "on";
  engagement_id: string | null;
  name: string;
  scope: Scope;
  started_at: string | null;
}

export interface DashboardStatus {
  hostname: string;
  now: string;
  cpu: { load_1m: number; load_5m: number; load_15m: number; count: number; percent: number };
  memory: { total_mb: number; available_mb: number; percent: number };
  temp_c: number | null;
  temp_f: number | null;
  throttled: string | null;
  gps: { ok: boolean; mode?: number; [k: string]: unknown };
  mesh_node_count: number | null;
  sdr: { ok: boolean; count?: number; [k: string]: unknown };
  engagement: EngagementStatus;
  [k: string]: unknown;
}

export interface EngagementListItem {
  id: string;
  name: string;
  status: "draft" | "active" | "ended" | string;
  created_at: string;
  started_at: string | null;
  ended_at: string | null;
}

export interface AarStatus {
  ok: boolean;
  enabled: boolean;
  subject: string;
  principal: string;
  log_host: string;
  records: number;
}

// --- transport ---------------------------------------------------------------

async function api<T>(path: string, init?: RequestInit): Promise<T | null> {
  try {
    const r = await fetch(`${API_URL}${path}`, {
      ...init,
      headers: { authorization: AUTH, ...(init?.headers ?? {}) },
      signal: AbortSignal.timeout(3000),
    });
    if (!r.ok) return null;
    return (await r.json()) as T;
  } catch {
    return null;
  }
}

async function post(path: string): Promise<boolean> {
  try {
    const r = await fetch(`${API_URL}${path}`, {
      method: "POST",
      headers: { authorization: AUTH },
      signal: AbortSignal.timeout(5000),
    });
    return r.ok;
  } catch {
    return false;
  }
}

// --- reads -------------------------------------------------------------------

export function getDashboard(): Promise<DashboardStatus | null> {
  return api<DashboardStatus>("/api/dashboard/status");
}

export async function getEngagements(): Promise<EngagementListItem[]> {
  return (await api<EngagementListItem[]>("/api/engagements")) ?? [];
}

export function getAarStatus(): Promise<AarStatus | null> {
  return api<AarStatus>("/api/aar/status");
}

// --- gate actions (audited server-side) --------------------------------------

export function activateEngagement(id: string): Promise<boolean> {
  return post(`/api/engagements/${encodeURIComponent(id)}/activate`);
}

export function endEngagement(id: string): Promise<boolean> {
  return post(`/api/engagements/${encodeURIComponent(id)}/end`);
}

export function killswitch(): Promise<boolean> {
  return post(`/api/engagements/killswitch`);
}
