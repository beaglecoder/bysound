import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { DEFAULT_SECURITY_LEVEL, SECURITY_LEVELS, resolveSecurity } from './security';

const table = JSON.parse(readFileSync(new URL('../../../test-vectors/security-levels.json', import.meta.url), 'utf8'));

describe('security levels', () => {
  it('match test-vectors/security-levels.json', () => {
    expect(SECURITY_LEVELS).toEqual(table.levels);
    expect(DEFAULT_SECURITY_LEVEL).toBe(table.default);
    expect(resolveSecurity()).toEqual(table.levels.maximum);
  });

  it('explicit options win, field by field', () => {
    const s = resolveSecurity('standard', { listenBeforeSendingSeconds: 5, confirmWatchSeconds: undefined, riskSignal: { honor: false } });
    expect(s.listenBeforeSendingSeconds).toBe(5);
    expect(s.confirmWatchSeconds).toBe(120);
    expect(s.riskSignal).toEqual({ ...table.levels.standard.riskSignal, honor: false });
  });

  it('rejects an unknown level', () => {
    expect(() => resolveSecurity('paranoid' as never)).toThrow(/securityLevel/);
  });
});
