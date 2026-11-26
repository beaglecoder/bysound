import { describe, expect, it } from 'vitest';
import { payloadFrame } from '../modem/codec';
import { render } from '../modem/modulator';
import { GAP_SECONDS } from '../modem/protocol';
import { RiskMemory, SessionError, type Alert, type QuarantineNotice } from './common';
import { fingerprint, riskFrame, shortId } from './frames';
import { ReceiverSession, type ReceiverOptions } from './receiver';
import { SenderSession, type SenderOptions } from './sender';
import { Air, type Device } from './testAir';

const TEXT = 'pay 12.50 to shop 7';

function sender(d: Device, text: string, o: SenderOptions = {}) {
  const alerts: Alert[] = [], quarantines: QuarantineNotice[] = [];
  const s = new SenderSession(text, d, { memory: new RiskMemory(), onAlert: (a) => alerts.push(a), onQuarantine: (q) => quarantines.push(q), ...o });
  d.listen((e) => s.hear(e));
  s.start();
  return { s, alerts, quarantines, delivered: settled(s.delivered) };
}
function receiver(d: Device, o: ReceiverOptions = {}) {
  const alerts: Alert[] = [], quarantines: QuarantineNotice[] = [];
  const r = new ReceiverSession(d, { memory: new RiskMemory(), onAlert: (a) => alerts.push(a), onQuarantine: (q) => quarantines.push(q), ...o });
  d.listen((e) => r.hear(e));
  r.start();
  return { r, alerts, quarantines, result: settled(r.result) };
}
/** Tracks a promise so a test can look at it at any simulated time. */
function settled<T>(p: Promise<T>) {
  const box: { value?: T; error?: SessionError; done: boolean } = { done: false };
  p.then((v) => { box.value = v; box.done = true; }, (e: SessionError) => { box.error = e; box.done = true; });
  return box;
}
const loopAudio = (text: string, band: 'top' | 'high' | 'xhigh' | 'mid', rate = 48000) => {
  const f = payloadFrame(text, band);
  return render(f.symbols, f.band, rate, GAP_SECONDS);
};

describe('session: ack and receipt', () => {
  it('the ack stops the sender, the receipt reaches the receiver, the gate is clean', async () => {
    const air = new Air();
    const a = sender(air.device('sender'), TEXT);
    const b = receiver(air.device('receiver'));
    await air.run(30, () => a.delivered.done && b.result.done);
    expect(b.result.value).toEqual({ text: TEXT, band: 'top', via: 'payload' });
    expect(a.delivered.value?.band).toBe('top');
    expect(a.s.state).toBe('receipt'); // the payload stopped; it is answering with receipts
    const receipt = settled(b.r.receipt);
    await air.run(25, () => receipt.done && a.s.state === 'done');
    expect(receipt.value).toBe(true);
    expect(a.s.state).toBe('done');
    const c = await b.r.confirmed();
    expect(c.watch).toBe('clean');
    expect(c.watchedSeconds).toBeGreaterThan(0);
    expect(a.alerts).toEqual([]);
    expect(b.alerts).toEqual([]);
  });

  it('a sender with nobody acking times out', async () => {
    const air = new Air();
    const a = sender(air.device('sender'), TEXT, { securityLevel: 'basic', timeoutSeconds: 6 });
    await air.run(10, () => a.delivered.done);
    expect(a.delivered.error?.code).toBe('TIMEOUT');
  });
});

describe('session: two senders', () => {
  it('the receiver hears a second sender, rejects the data and sounds a risk frame; the real sender stops and relays it', async () => {
    const air = new Air();
    const legit = air.device('legit'), rogue = air.device('rogue'), rx = air.device('receiver');
    air.gain(rogue, legit, 0); // they cannot hear each other: only the receiver can warn the legit sender
    const a = sender(legit, TEXT, { securityLevel: 'standard' });
    const b = receiver(rx, { securityLevel: 'standard' });
    await air.run(2.5);
    rogue.loop(loopAudio('pay 99.00 to shop 666', 'high'), 20);
    await air.run(30, () => a.delivered.done && b.result.done);
    expect(b.result.error?.code).toBe('MULTIPLE_TRANSMISSIONS');
    expect(b.alerts[0]).toMatchObject({ direction: 'emitted', source: 'receiver' });
    expect(['overlap', 'other']).toContain(b.alerts[0]!.reason);
    expect(a.delivered.error?.code).toBe('RISK_SIGNAL');
    expect(a.delivered.error?.alert).toMatchObject({ direction: 'heard', source: 'receiver', hops: 0 });
    expect(a.alerts.find((x) => x.direction === 'emitted')).toMatchObject({ relay: true, hops: 1, source: 'receiver' });
    expect(b.quarantines[0]?.durationMs).toBe(30000);
    expect(a.quarantines[0]?.durationMs).toBe(30000);
  });

  it('a sender that hears another transmission in its listen window stops and warns the receiver', async () => {
    const air = new Air();
    const legit = air.device('legit'), rogue = air.device('rogue'), rx = air.device('receiver');
    air.gain(rogue, rx, 0); // the receiver cannot hear the rogue: only the sender can
    const a = sender(legit, TEXT, { securityLevel: 'standard' });
    const b = receiver(rx, { securityLevel: 'standard', ack: false });
    await air.run(2.2);
    rogue.loop(loopAudio('pay 99.00 to shop 666', 'xhigh'), 30);
    await air.run(30, () => a.delivered.done && b.result.done && b.alerts.length > 0);
    expect(a.delivered.error?.code).toBe('OTHER_EMITTER');
    expect(a.alerts[0]).toMatchObject({ direction: 'emitted', reason: 'other', source: 'sender', own: shortId(TEXT), intruder: shortId('pay 99.00 to shop 666') });
    expect(b.alerts[0]).toMatchObject({ direction: 'heard', reason: 'other', source: 'sender', matchesMine: true });
    // Delivered before the warning arrived: the gate is what catches it.
    if (b.result.value) await expect(b.r.confirmed()).rejects.toMatchObject({ code: 'RISK_SIGNAL' });
    else expect(b.result.error?.code).toBe('RISK_SIGNAL');
  });

  it('two frames overlapping on the same band are a clash: the receiver delivers nothing', async () => {
    const air = new Air();
    const legit = air.device('legit'), rogue = air.device('rogue'), rx = air.device('receiver');
    air.gain(rogue, legit, 0);
    const a = sender(legit, TEXT, { securityLevel: 'basic', timeoutSeconds: 10 });
    const b = receiver(rx, { securityLevel: 'standard', ack: false });
    await air.run(1.4); // basic pre-listens 1 s: the rogue starts 0.4 s into the legit frame, on the same band
    rogue.loop(loopAudio('pay 99.00 to shop 666', 'top'), 8);
    await air.run(12, () => b.result.done);
    expect(b.result.error?.code).toBe('MULTIPLE_TRANSMISSIONS');
    expect(b.alerts[0]).toMatchObject({ direction: 'emitted', reason: 'overlap', source: 'receiver', intruder: '000000', bands: ['mid', 'top'] }); // the clash band is unknown
    a.s.stop();
  });

  it('a copy of the sender\'s own data on another band is a clone', async () => {
    const air = new Air();
    const legit = air.device('legit'), replay = air.device('replay');
    const a = sender(legit, TEXT, { securityLevel: 'standard' });
    await air.run(2.2);
    replay.loop(loopAudio(TEXT, 'xhigh'), 20);
    await air.run(20, () => a.delivered.done);
    expect(a.delivered.error?.code).toBe('CLONED_TRANSMISSION');
    expect(a.alerts[0]).toMatchObject({ direction: 'emitted', reason: 'clone', own: shortId(TEXT), intruder: shortId(TEXT) });
  });

  it('the confirm gate: a different transmission after delivery closes it', async () => {
    const air = new Air();
    const legit = air.device('legit'), late = air.device('late'), rx = air.device('receiver');
    const a = sender(legit, TEXT, { securityLevel: 'standard' });
    const b = receiver(rx, { securityLevel: 'standard' });
    await air.run(30, () => a.delivered.done && b.result.done);
    expect(b.result.value?.text).toBe(TEXT);
    a.s.stop();
    late.loop(loopAudio('pay 99.00 to shop 666', 'top'), 10);
    await air.run(10, () => b.alerts.length > 0);
    await expect(b.r.confirmed()).rejects.toMatchObject({ code: 'MULTIPLE_TRANSMISSIONS', alert: { direction: 'emitted', reason: 'other', source: 'receiver' } });
  });

  it('pre-listen: a sender does not start while another transmission sounds', async () => {
    const air = new Air();
    const other = air.device('other'), me = air.device('me');
    other.loop(loopAudio('somebody else', 'top'), 10);
    const a = sender(me, TEXT, { securityLevel: 'standard' });
    await air.run(8, () => a.delivered.done);
    expect(a.delivered.error?.code).toBe('OTHER_TRANSMISSION');
    expect(a.alerts).toEqual([]); // nothing was sent, nothing to warn about
  });

  it('pre-listen: its own data already sounding is a clone', async () => {
    const air = new Air();
    const replay = air.device('replay'), me = air.device('me');
    replay.loop(loopAudio(TEXT, 'top'), 10);
    const a = sender(me, TEXT, { securityLevel: 'standard' });
    await air.run(8, () => a.delivered.done);
    expect(a.delivered.error?.code).toBe('CLONED_TRANSMISSION');
  });
});

describe('session: security levels', () => {
  it('basic delivers at the first frame and has no confirm watch', async () => {
    const air = new Air();
    const a = sender(air.device('sender'), TEXT, { securityLevel: 'basic' });
    const b = receiver(air.device('receiver'), { securityLevel: 'basic', ack: false });
    let heardAt = 0;
    b.r['options'].onHeard = () => { heardAt = air.t / air.rate; };
    await air.run(10, () => b.result.done);
    expect(air.t / air.rate - heardAt).toBeLessThan(0.1);
    expect(await b.r.confirmed()).toEqual({ watch: 'off', watchedSeconds: 0, uncoveredSeconds: 0 });
    a.s.stop();
  });

  it('standard and maximum wait for the final listen, and the gate reports an expired watch', async () => {
    for (const level of ['standard', 'maximum'] as const) {
      const air = new Air();
      const a = sender(air.device('sender'), TEXT, { securityLevel: level, timeoutSeconds: 8 });
      const b = receiver(air.device('receiver'), { securityLevel: level, ack: false, security: { confirmWatchSeconds: 2 } });
      let heardAt = 0;
      b.r['options'].onHeard = () => { heardAt = air.t / air.rate; };
      await air.run(15, () => b.result.done);
      expect(air.t / air.rate - heardAt).toBeGreaterThanOrEqual(1.5 - 0.03);
      await air.run(3);
      const c = await b.r.confirmed();
      expect(c.watch).toBe('expired');
      expect(c.watchedSeconds).toBeCloseTo(2, 1);
      expect(c.uncoveredSeconds).toBeGreaterThan(0.5);
      a.s.stop();
    }
  });

  it('the sender pre-listen lasts listenBeforeSendingSeconds plus jitter', async () => {
    for (const [level, min, max] of [['basic', 1, 1.05], ['standard', 2, 2.6], ['maximum', 3, 4.2]] as const) {
      const air = new Air();
      const d = air.device('sender');
      let sendingAt = -1;
      const a = sender(d, TEXT, { securityLevel: level, onState: (s) => { if (s === 'sending' && sendingAt < 0) sendingAt = d.now(); } });
      await air.run(5, () => sendingAt >= 0);
      expect(sendingAt).toBeGreaterThanOrEqual(min);
      expect(sendingAt).toBeLessThanOrEqual(max);
      a.s.stop();
    }
  });
});

describe('session: risk signal handling', () => {
  const risk = (hops: number) => riskFrame({ reason: 'other', source: 'receiver', hops, own: shortId('x'), intruder: shortId('y') }, 'mid');
  const riskLoop = (hops: number) => { const f = risk(hops); return render(f.symbols, f.band, 48000, 0.15); };

  it('quarantine is only a callback: the next session on the same device runs normally', async () => {
    const air = new Air();
    const alarm = air.device('alarm'), me = air.device('me'), rx = air.device('receiver');
    air.gain(alarm, rx, 0);
    alarm.loop(riskLoop(0), 3);
    const memory = new RiskMemory();
    const first = sender(me, TEXT, { securityLevel: 'standard', memory });
    await air.run(8, () => first.delivered.done);
    expect(first.delivered.error?.code).toBe('RISK_SIGNAL');
    expect(first.quarantines).toEqual([{ durationMs: 30000, reason: 'other', source: 'receiver' }]);
    first.s.stop(); // stop relaying
    await air.run(1);
    const second = new SenderSession(TEXT, me, { securityLevel: 'standard', memory });
    me.listen((e) => second.hear(e));
    second.start();
    const delivered = settled(second.delivered);
    const b = receiver(rx, { securityLevel: 'standard' });
    await air.run(25, () => delivered.done);
    expect(delivered.value?.band).toBe('top');
    expect(b.result.value?.text).toBe(TEXT);
  });

  it('honor false: the alert is reported but nothing stops', async () => {
    const air = new Air();
    const alarm = air.device('alarm'), me = air.device('me');
    alarm.loop(riskLoop(0), 3);
    const a = sender(me, TEXT, { securityLevel: 'standard', security: { riskSignal: { honor: false } }, timeoutSeconds: 6 });
    await air.run(10, () => a.delivered.done);
    expect(a.alerts.map((x) => x.direction)).toEqual(['heard']);
    expect(a.delivered.error?.code).toBe('TIMEOUT');
  });

  it('relays one hop further, and never past 2 hops', async () => {
    for (const [hops, relayed] of [[0, 1], [1, 2], [2, null]] as const) {
      const air = new Air();
      const alarm = air.device('alarm'), me = air.device('me');
      alarm.loop(riskLoop(hops), 2.5);
      const a = sender(me, TEXT, { securityLevel: 'standard' });
      await air.run(8, () => a.delivered.done);
      expect(a.delivered.error?.code).toBe('RISK_SIGNAL');
      const emitted = a.alerts.filter((x) => x.direction === 'emitted');
      expect(emitted.map((x) => x.hops)).toEqual(relayed === null ? [] : [relayed]);
      a.s.stop();
    }
  });

  it('basic does not listen for risk frames', async () => {
    const air = new Air();
    const alarm = air.device('alarm'), me = air.device('me');
    alarm.loop(riskLoop(0), 3);
    const a = sender(me, TEXT, { securityLevel: 'basic', timeoutSeconds: 4 });
    await air.run(6, () => a.delivered.done);
    expect(a.alerts).toEqual([]);
    expect(a.delivered.error?.code).toBe('TIMEOUT');
  });
});

describe('session: reference mode', () => {
  it('the receiver resolves the fingerprint with a local map and checks it', async () => {
    const catalog = new Map([[fingerprint(TEXT), TEXT]]);
    const air = new Air();
    const a = sender(air.device('sender'), TEXT, { mode: 'reference', securityLevel: 'standard' });
    const b = receiver(air.device('receiver'), { securityLevel: 'standard', resolve: (fp) => catalog.get(fp) });
    await air.run(25, () => a.delivered.done && b.result.done);
    expect(b.result.value).toEqual({ text: TEXT, band: 'top', via: 'reference' });
    expect(a.delivered.value?.band).toBe('top');
  });

  it('unknown fingerprint, wrong text, no resolver', async () => {
    for (const [resolve, code] of [[() => null, 'NOT_FOUND'], [() => 'something else', 'MISMATCH'], [undefined, 'NO_RESOLVER']] as const) {
      const air = new Air();
      const a = sender(air.device('sender'), TEXT, { mode: 'reference', securityLevel: 'basic' });
      const b = receiver(air.device('receiver'), { securityLevel: 'basic', resolve });
      await air.run(10, () => b.result.done);
      expect(b.result.error?.code).toBe(code);
      a.s.stop();
    }
  });
});

describe('session: cancel', () => {
  it('cancel before delivery rejects the result and never throws; confirmed() then fails with CANCELLED', async () => {
    const air = new Air();
    const b = receiver(air.device('receiver'));
    await air.run(0.5);
    b.r.cancel();
    b.r.cancel();
    await air.run(0.1);
    expect(b.result.error?.code).toBe('CANCELLED');
    await expect(b.r.confirmed()).rejects.toMatchObject({ code: 'CANCELLED' });
  });
});
