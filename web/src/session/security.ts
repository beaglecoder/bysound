// Security levels (docs/SESSION.md §7). The table is test-vectors/security-levels.json; security.test.ts checks they are equal.

export type SecurityLevel = 'basic' | 'standard' | 'maximum';
export const DEFAULT_SECURITY_LEVEL: SecurityLevel = 'maximum';

export interface RiskSignal {
  /** 'auto': whoever detects another emitter sounds a risk frame. 'off': it only stops and reports. */
  emit: 'auto' | 'off';
  /** Listen for risk frames. */
  listen: boolean;
  /** Act on one: the sender stops (and relays), the receiver fails / closes the confirm gate. false: only onAlert. */
  honor: boolean;
  /** Receiver only: 'delivery' stops listening for risk frames at delivery, 'confirm' keeps listening until confirmed(). */
  listenUntil: 'delivery' | 'confirm';
  /** How long a receiver loops its risk frame. */
  durationMs: number;
  /** How long a sender loops its risk frame (after detecting another emitter, or when relaying one). */
  detectorDurationMs: number;
  /** Suggested quarantine sent to onQuarantine. Never enforced. */
  quarantineMs: number;
}

export interface Security {
  /** Sender: pre-listen and listen windows act on what they hear. Receiver: final listen and clash rejection. */
  detectMultipleEmitters: boolean;
  /** Sender: other emitters heard while sending (in the listen windows) stop the session. */
  listenWhileSending: boolean;
  /** Sender: listen this long before the first frame, plus up to `scanJitterSeconds` at random. Skipped without detection. */
  listenBeforeSendingSeconds: number;
  scanJitterSeconds: number;
  /** Receiver: keep watching after delivery until confirmed() / cancel(), at most `confirmWatchSeconds`. */
  confirmWatch: boolean;
  confirmWatchSeconds: number;
  riskSignal: RiskSignal;
}

const risk = (emit: RiskSignal['emit'], on: boolean, listenUntil: RiskSignal['listenUntil'], durationMs: number, detectorDurationMs: number, quarantineMs: number): RiskSignal =>
  ({ emit, listen: on, honor: on, listenUntil, durationMs, detectorDurationMs, quarantineMs });

export const SECURITY_LEVELS: Record<SecurityLevel, Security> = {
  basic: { detectMultipleEmitters: false, listenWhileSending: false, listenBeforeSendingSeconds: 1, scanJitterSeconds: 0.6, confirmWatch: false, confirmWatchSeconds: 120, riskSignal: risk('off', false, 'delivery', 4000, 4000, 30000) },
  standard: { detectMultipleEmitters: true, listenWhileSending: true, listenBeforeSendingSeconds: 2, scanJitterSeconds: 0.6, confirmWatch: true, confirmWatchSeconds: 120, riskSignal: risk('auto', true, 'confirm', 4000, 15000, 30000) },
  maximum: { detectMultipleEmitters: true, listenWhileSending: true, listenBeforeSendingSeconds: 3, scanJitterSeconds: 1.2, confirmWatch: true, confirmWatchSeconds: 120, riskSignal: risk('auto', true, 'confirm', 6000, 30000, 60000) },
};

/** Explicit options win over the level, field by field (also inside riskSignal). */
export type SecurityOverrides = Partial<Omit<Security, 'riskSignal'>> & { riskSignal?: Partial<RiskSignal> };

export function resolveSecurity(level: SecurityLevel = DEFAULT_SECURITY_LEVEL, overrides: SecurityOverrides = {}): Security {
  const base = SECURITY_LEVELS[level];
  if (!base) throw new Error(`Unknown securityLevel "${String(level)}" (basic, standard or maximum)`);
  const defined = <T extends object>(o: T) => Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined)) as Partial<T>;
  const { riskSignal, ...rest } = overrides;
  return { ...base, ...defined(rest), riskSignal: { ...base.riskSignal, ...defined(riskSignal ?? {}) } };
}
