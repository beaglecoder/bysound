// Small DOM helpers shared by the three pages.
import { SessionError, type Alert } from '../../web/src/index';

export const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

export type Kind = 'mine' | 'theirs' | 'alert' | 'error';
/** Adds (or rewrites) a line of the page's log. Text only: nothing heard or resolved is ever inserted as HTML. */
export function line(kind: Kind, text: string, existing?: HTMLElement): HTMLElement {
  const li = existing ?? document.createElement('li');
  li.className = kind;
  li.textContent = text;
  if (!existing) $('log').prepend(li);
  return li;
}

/** The short ids (6 hex) an alert carries, without the "unknown" 000000. */
export const alertShortIds = (a?: Alert) => (a ? [a.own, a.intruder].filter((s) => s && s !== '000000') : []);

export function describe(e: unknown): string {
  if (!(e instanceof SessionError)) return `Error: ${(e as Error).message}`;
  switch (e.code) {
    case 'OTHER_TRANSMISSION': return 'Another transmission was already sounding: nothing was sent.';
    case 'OTHER_EMITTER': return 'Another sender was heard while sending: stopped and warned the others.';
    case 'CLONED_TRANSMISSION': return 'Someone else is playing this same data (a clone or a replay): stopped.';
    case 'MULTIPLE_TRANSMISSIONS': return 'Two different transmissions (or two overlapping frames): nothing accepted.';
    case 'RISK_SIGNAL': return 'Another device warned of a clash by sound: stopped to stay safe.';
    case 'NOT_FOUND': return 'The backend does not know this fingerprint (never published, expired, already used or blocked).';
    case 'MISMATCH': return 'The backend returned data that does not hash to the fingerprint heard: rejected.';
    case 'TIMEOUT': return 'Timed out.';
    case 'STOPPED': case 'CANCELLED': return 'Stopped.';
    default: return `${e.code}: ${e.message}`;
  }
}

/** Codes that mean "more than one emitter, or a warning": report them to the backend. */
export const isRisk = (e: unknown): e is SessionError =>
  e instanceof SessionError && ['MULTIPLE_TRANSMISSIONS', 'RISK_SIGNAL', 'OTHER_EMITTER', 'CLONED_TRANSMISSION'].includes(e.code);
