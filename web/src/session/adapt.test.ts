import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import type { BandId, Profile } from '../modem/protocol';
import {
  CALIBRATION_USES, LOUD_PLACE_DB, ROBUST_NOISE_DB, RobustLadder, channelOrder, lapped, localStorageCalibrationStore, memoryCalibrationStore, parseCalibration, plan, readAmbient,
  type CalibrationStore,
} from './adapt';
import { crc16 } from '../modem/codec';
import { RiskMemory, SessionError } from './common';
import { ackFrame, ackId, parseAck, type AckFrame } from './frames';
import { ReceiverSession } from './receiver';
import { SenderSession, type Delivered, type SenderOptions } from './sender';
import { Air, brokenTweeter, hiss, type Device } from './testAir';

const TEXT = 'pay 12.50 to shop 7';
const h = (a: number[]) => a.map((b) => b.toString(16).padStart(2, '0')).join('');

describe('acks say where the data was read', () => {
  const file = JSON.parse(readFileSync(new URL('../../../test-vectors/session-acks.json', import.meta.url), 'utf8'));
  for (const v of file.vectors) {
    it(`${v.band} ${v.receipt ? 'receipt' : 'ack'} heard ${v.heard ? `${v.heard.band}/${v.heard.profile}` : 'not stated'}`, () => {
      const ack: AckFrame = { receipt: v.receipt, id: ackId(v.text), ...(v.heard ? { heard: v.heard } : {}) };
      const f = ackFrame(ack, v.band);
      expect(h(f.header)).toBe(v.header);
      expect(h(f.body)).toBe(v.body);
      expect(f.symbols).toEqual(v.symbols);
      expect(parseAck(f.body.slice(0, 7))).toEqual(ack);
    });
  }
  it('rejects a robust flag without a band', () => {
    const withByte0 = (b0: number) => { const d = [b0, 0, 0, 0, 0]; const c = crc16(d); return parseAck([...d, c >> 8, c & 255]); };
    expect(withByte0(0x82)).toBeNull();
    expect(withByte0(0x72)).toEqual({ receipt: false, id: '00000000', heard: { band: 'wide', profile: 'normal' } });
  });
});

describe('ladder, ambient and calibration rules', () => {
  it('channel order: down from the start, then up', () => {
    const chain: BandId[] = ['top', 'high', 'upper', 'mid'];
    expect(channelOrder('top', chain)).toEqual(['top', 'high', 'upper', 'mid']);
    expect(channelOrder('upper', chain)).toEqual(['upper', 'mid', 'high', 'top']);
    expect(channelOrder('mid', chain)).toEqual(['mid', 'upper', 'high', 'top']);
  });

  it('ambient: noisy channels are skipped, a loud place is flagged', () => {
    const a = readAmbient([{ top: -50, high: -52, upper: -80, mid: -78 }, { top: -49, high: -51, upper: -81, mid: -77 }, { top: -51, high: -53, upper: -79, mid: -79 }], ['top', 'high', 'upper', 'mid'])!;
    expect(a.noisy).toEqual(['top', 'high']);
    expect(a.loud).toBe(false);
    expect(readAmbient([{ top: LOUD_PLACE_DB, high: LOUD_PLACE_DB + 1, upper: LOUD_PLACE_DB, mid: LOUD_PLACE_DB }], ['top', 'high', 'upper', 'mid'])!.loud).toBe(true);
    expect(readAmbient([], ['top'])).toBeNull();
    const p = plan({ calibration: null, ambient: a });
    expect(p.route).toEqual(['upper', 'mid']);
    expect(p.startRobust).toBeNull();
  });

  it('plan: calibration, expiry, fixed band, robust start', () => {
    expect(plan({ calibration: { band: 'mid', profile: 'normal', uses: 3 }, ambient: null })).toEqual({ route: ['mid', 'upper', 'high', 'top'], every: 6, calibration: { band: 'mid', profile: 'normal', uses: 3 }, startRobust: null });
    expect(plan({ calibration: { band: 'mid', profile: 'normal', uses: CALIBRATION_USES }, ambient: null }).route[0]).toBe('top');
    expect(plan({ band: 'xhigh', calibration: { band: 'mid', profile: 'normal', uses: 0 }, ambient: null }).route).toEqual(['xhigh']);
    expect(plan({ calibration: { band: 'upper', profile: 'robust', uses: 0 }, ambient: null }).startRobust).toBe('calibration');
    expect(plan({ profile: 'normal', calibration: { band: 'upper', profile: 'robust', uses: 0 }, ambient: null }).startRobust).toBeNull();
    const noisy = { noise: { top: ROBUST_NOISE_DB + 1, high: ROBUST_NOISE_DB, upper: ROBUST_NOISE_DB, mid: ROBUST_NOISE_DB }, noisy: [], loud: false };
    expect(plan({ calibration: null, ambient: noisy }).startRobust).toBe('noise');
    // The calibrated channel stays in the route even if it measures noisy.
    expect(plan({ calibration: { band: 'top', profile: 'normal', uses: 0 }, ambient: { noise: {}, noisy: ['top', 'high'], loud: false } }).route).toEqual(['top', 'upper', 'mid']);
  });

  it('robust ladder: after 6 normal repetitions, only where robust exists', () => {
    const l = new RobustLadder(true, false);
    const seen: Profile[] = (['top', 'top', 'top', 'high', 'high', 'high', 'xhigh', 'upper'] as BandId[]).map((b) => l.next(b));
    expect(seen).toEqual(['normal', 'normal', 'normal', 'normal', 'normal', 'normal', 'normal', 'robust']);
    expect(l.reason).toBe('repetitions');
    expect(new RobustLadder(false, false).next('top')).toBe('normal');
  });

  it('lapped: back to a channel after the whole route', () => {
    expect(lapped(['top', 'top', 'high', 'upper', 'mid'], 4)).toBe(false);
    expect(lapped(['top', 'high', 'upper', 'mid', 'top'], 4)).toBe(true);
  });

  it('stores: memory and localStorage (validated)', () => {
    const m = memoryCalibrationStore();
    m.save({ band: 'mid', profile: 'normal', uses: 1 });
    expect(m.load()).toEqual({ band: 'mid', profile: 'normal', uses: 1 });
    const data = new Map<string, string>();
    const g = globalThis as { localStorage?: unknown };
    const before = g.localStorage;
    g.localStorage = { getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => data.set(k, v), removeItem: (k: string) => data.delete(k) };
    try {
      const s = localStorageCalibrationStore('k');
      s.save({ band: 'upper', profile: 'robust', uses: 2 });
      expect(localStorageCalibrationStore('k').load()).toEqual({ band: 'upper', profile: 'robust', uses: 2 });
      data.set('k', '{"band":"nowhere","uses":1}');
      expect(localStorageCalibrationStore('k').load()).toBeNull();
    } finally {
      g.localStorage = before;
    }
    expect(parseCalibration({ band: 'mid', uses: -1 })).toBeNull();
  });
});

function send(d: Device, o: SenderOptions) {
  const s = new SenderSession(TEXT, d, { memory: new RiskMemory(), ...o });
  const reps: { band: BandId; profile: Profile }[] = [];
  const box: { value?: Delivered; error?: SessionError; done: boolean } = { done: false };
  s.delivered.then((v) => { box.value = v; box.done = true; }, (e: SessionError) => { box.error = e; box.done = true; });
  const opts = s as unknown as { options: SenderOptions };
  opts.options.onRepetition = (r) => reps.push(r);
  d.listen((e) => s.hear(e));
  s.start();
  return { s, reps, box };
}
function receive(d: Device) {
  const r = new ReceiverSession(d, { memory: new RiskMemory(), securityLevel: 'standard' });
  d.listen((e) => r.hear(e), { robust: true });
  r.start();
  return r;
}

describe('session: automatic adaptation', () => {
  it('a phone with a broken tweeter climbs down to mid, remembers it, and the next send is read at once', async () => {
    const air = new Air();
    const phone = air.device('phone'), rx = air.device('receiver');
    phone.speaker = brokenTweeter(17_300); // only mid gets through
    const store: CalibrationStore = memoryCalibrationStore();
    const first = send(phone, { securityLevel: 'standard', calibration: store });
    let r = receive(rx);
    await air.run(70, () => first.box.done);
    expect(first.box.value).toMatchObject({ band: 'mid', stated: true });
    expect(first.reps.slice(0, 3).map((x) => x.band)).toEqual(['top', 'top', 'top']);
    expect(new Set(first.reps.map((x) => x.band))).toEqual(new Set(['top', 'high', 'upper', 'mid']));
    expect(first.reps.some((x) => x.profile === 'robust')).toBe(true); // nobody read the first 6: the ladder went robust
    expect(store.load()).toEqual({ band: 'mid', profile: 'normal', uses: 0 }); // robust by the ladder is not the speaker's
    await r.confirmed().catch(() => undefined);
    first.s.stop();
    // No receipt reaches the receiver (receipts are on high), so it acks for ACK_TOTAL_SECONDS; the next pre-listen would measure
    // those acks as noise on mid. Wait until it is quiet.
    let quiet = false;
    void r.finished.then(() => { quiet = true; });
    await air.run(40, () => quiet);

    const second = send(phone, { securityLevel: 'standard', calibration: store });
    r = receive(rx);
    await air.run(30, () => second.box.done);
    expect(second.reps[0]).toEqual({ index: 0, band: 'mid', profile: 'normal' });
    expect(second.box.value).toMatchObject({ band: 'mid', repetitions: 1 });
    expect(store.load()).toEqual({ band: 'mid', profile: 'normal', uses: 0 });
    second.s.stop();
  }, 60_000);

  it('skips channels the ambient check finds noisy', async () => {
    const air = new Air();
    const fan = air.device('fan'), phone = air.device('phone');
    fan.play(hiss(18_900, 19_500, 0.02, 12), () => undefined); // tones only top and high cover
    const routes: BandId[][] = [];
    const a = send(phone, { securityLevel: 'standard', calibration: null, onAmbient: (_, route) => routes.push(route), timeoutSeconds: 3 });
    await air.run(8, () => a.box.done);
    expect(routes[0]).toEqual(['upper', 'mid']);
    expect(a.reps[0]!.band).toBe('upper');
  });

  it('a noisy place starts robust; an explicit profile wins', async () => {
    for (const [profile, expected] of [[undefined, 'robust'], ['normal', 'normal']] as const) {
      const air = new Air(48000, 0.05); // white noise ≈ −40 dB per tone: over ROBUST_NOISE_DB on every channel
      const a = send(air.device('phone'), { securityLevel: 'basic', calibration: null, timeoutSeconds: 2, ...(profile ? { profile } : {}) });
      await air.run(5, () => a.box.done);
      expect(a.reps[0]!.profile).toBe(expected);
    }
  });

  it('a calibration that keeps failing expires after CALIBRATION_USES sends', async () => {
    const store = memoryCalibrationStore({ band: 'upper', profile: 'normal', uses: CALIBRATION_USES - 1 });
    const air = new Air();
    const a = send(air.device('phone'), { securityLevel: 'basic', calibration: store, timeoutSeconds: 2 });
    await air.run(5, () => a.box.done);
    expect(a.reps[0]!.band).toBe('upper');
    expect(store.load()?.uses).toBe(CALIBRATION_USES);
    const b = send(air.device('phone2'), { securityLevel: 'basic', calibration: store, timeoutSeconds: 2 });
    await air.run(5, () => b.box.done);
    expect(b.reps[0]!.band).toBe('top');
  });
});
