// Maps Warlock backend state to the claude-desktop-buddy wire protocol so the
// CoreS3 firmware can render a warlock dashboard with no protocol-specific
// firmware churn. Legacy top-level fields keep the base wire format intact; the
// `warlock.*` extension carries the engagement-gate + deck-health view the
// CoreS3 dashboard draws.
//
// Reference: ../../claude-desktop-buddy/REFERENCE.md (base protocol)

import type { DashboardStatus, EngagementListItem, AarStatus } from "./warlock.ts";

export type Persona = "OFFLINE" | "SAFE" | "ARMED" | "ATTENTION";

export interface Heartbeat {
  // Legacy top-level fields — base firmware reads these.
  total: number;     // engagements known to the deck
  running: number;   // 1 when an engagement is ARMED (live), else 0
  waiting: number;   // staged (draft) engagements awaiting an ARM tap
  msg: string;       // one-line glance summary (≤23 chars for small HUDs)
  entries: string[];
  tokens: number;        // XP for the level-up loop = signed AAR records
  tokens_today: number;
  prompt?: { id: string; tool: string; hint: string };

  // Rich extension — CoreS3 firmware reads this.
  warlock?: {
    online: boolean;          // is the deck API reachable
    engaged: boolean;         // engagement.mode === "on"
    persona: Persona;         // drives the familiar's mood
    engagement: {
      id: string | null;
      name: string;
      started_at: string | null;
      scope: { ssids: number; bssids: number; ip_ranges: number };
    };
    // Staged engagements the operator can ARM with a tap (id needed for the
    // {cmd:"arm",id} round-trip).
    drafts: Array<{ id: string; name: string }>;
    aar: { enabled: boolean; records: number; subject: string };
    sys: {
      temp_c: number | null;
      cpu_pct: number | null;
      mem_pct: number | null;
      gps_fix: boolean;      // gps.ok && mode >= 2
      mesh_nodes: number | null;
      sdr: number | null;
    };
  };
}

function scopeCounts(s: DashboardStatus["engagement"]["scope"]) {
  return {
    ssids: s?.ssids?.length ?? 0,
    bssids: s?.bssids?.length ?? 0,
    ip_ranges: s?.ip_ranges?.length ?? 0,
  };
}

export function buildHeartbeat(
  dash: DashboardStatus | null,
  engagements: EngagementListItem[],
  aar: AarStatus | null,
): Heartbeat {
  const online = dash !== null;
  const eng = dash?.engagement;
  const engaged = eng?.mode === "on";
  const drafts = engagements.filter((e) => e.status === "draft");

  const persona: Persona = !online
    ? "OFFLINE"
    : engaged
      ? "ARMED"
      : drafts.length > 0
        ? "ATTENTION"
        : "SAFE";

  // One-line glance summary.
  let msg: string;
  if (!online) msg = "deck offline";
  else if (engaged) msg = `ARMED: ${eng?.name || "engagement"}`.slice(0, 23);
  else if (drafts.length > 0) msg = `${drafts.length} staged · SAFE`.slice(0, 23);
  else msg = "SAFE · no engagement";

  const records = aar?.records ?? 0;
  const gps = dash?.gps;
  const gpsFix = !!(gps?.ok && (gps?.mode ?? 0) >= 2);

  return {
    total: engagements.length,
    running: engaged ? 1 : 0,
    waiting: drafts.length,
    msg,
    entries: [],
    tokens: records,
    tokens_today: records,
    warlock: {
      online,
      engaged,
      persona,
      engagement: {
        id: eng?.engagement_id ?? null,
        name: eng?.name ?? "",
        started_at: eng?.started_at ?? null,
        scope: scopeCounts(eng?.scope ?? { ssids: [], bssids: [], ip_ranges: [] }),
      },
      drafts: drafts.map((d) => ({ id: d.id, name: d.name })),
      aar: {
        enabled: aar?.enabled ?? false,
        records,
        subject: aar?.subject ?? "",
      },
      sys: {
        temp_c: dash?.temp_c ?? null,
        cpu_pct: dash?.cpu?.percent ?? null,
        mem_pct: dash?.memory?.percent ?? null,
        gps_fix: gpsFix,
        mesh_nodes: dash?.mesh_node_count ?? null,
        sdr: dash?.sdr?.ok ? (dash?.sdr?.count ?? 0) : null,
      },
    },
  };
}
