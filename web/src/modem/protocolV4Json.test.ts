import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import * as P from './protocol';

// protocol-v4.json is the shared v4 definition (Swift, Kotlin and C compare against it in their own tests).
// Here it is compared against the web modem and the executable reference.
const spec = JSON.parse(readFileSync(new URL('../../../protocol-v4.json', import.meta.url), 'utf8'));

describe('protocol-v4.json', () => {
  it('the web modem constants match the JSON', () => {
    expect(P.VERSION).toBe(spec.version);
    expect(P.SPACING_HZ).toBe(spec.spacingHz);
    expect(P.TONES).toBe(spec.tonesPerGroup);
    expect(P.RAMP_SECONDS).toBe(spec.rampSeconds);
    expect(P.GAP_SECONDS).toBe(spec.gapSeconds);
    expect(P.MAX_FRAME_SECONDS).toBe(spec.maxFrameSeconds);
    expect(P.HOP_SECONDS).toBe(spec.hopSeconds);
    expect(P.PREAMBLE_THRESHOLD).toBe(spec.preambleThreshold);
    expect(P.HEADER_BYTES).toBe(spec.headerBytes);
    expect(P.MAGIC_PAYLOAD).toBe(spec.magicPayload);
    expect(P.MAGIC_ACK).toBe(spec.magicAck);
    expect(P.MAGIC_REFERENCE).toBe(spec.magicReference);
    expect(P.MAGIC_RISK).toBe(spec.magicRisk);
    expect(P.REF_BYTES).toBe(spec.referenceBytes);
    expect(P.REF_DATA).toBe(spec.referenceDataBytes);
    expect(P.REF_PARITY).toBe(spec.referenceParity);
    expect(P.MOTHER_PARITY).toBe(spec.motherParity);
    expect(P.BLOCK_DATA).toBe(spec.blockData);
    expect(P.BLOCK_PARITY).toBe(spec.blockParity);
    expect(P.BLOCK_HEADER).toBe(spec.blockHeader);
    expect(P.MAX_BLOCKS).toBe(spec.maxBlocks);
    expect(P.RISK_LENGTH).toBe(spec.riskLength);
    expect(P.ACK_LENGTH).toBe(spec.ackLength);
    expect(P.PREAMBLE).toEqual(spec.preamble);
    expect(P.CHANNEL_IDS).toEqual(spec.channelIds);
    // The robust profile ones are in robust.test.ts.
    expect(P.PREAMBLE_KEYS).toEqual(spec.preambleKeys.normal);
  });

  it('the web modem bands match the JSON', () => {
    expect(Object.keys(P.BANDS).sort()).toEqual(Object.keys(spec.bands).sort());
    for (const [id, b] of Object.entries<any>(spec.bands)) {
      const band = P.BANDS[id as P.BandId];
      expect({ bin: band.bin, groups: band.groups, amp: band.amp, sym: band.sym, windowStart: band.windowStart }, id).toEqual({
        bin: b.bin, groups: b.groups, amp: b.toneAmplitude, sym: b.symbolSeconds, windowStart: b.windowStartSeconds,
      });
    }
  });

  it('the executable reference (v4-reference.mjs) matches the JSON', async () => {
    const { P4 } = await import(/* @vite-ignore */ String(new URL('../../../protocol/v4-reference.mjs', import.meta.url)));
    expect(P4.version).toBe(spec.version);
    expect(P4.spacing).toBe(spec.spacingHz);
    expect(P4.tones).toBe(spec.tonesPerGroup);
    expect(P4.gap).toBe(spec.gapSeconds);
    expect(P4.refBytes).toBe(spec.referenceBytes);
    expect(P4.refParity).toBe(spec.referenceParity);
    expect(P4.motherParity).toBe(spec.motherParity);
    expect(P4.blockData).toBe(spec.blockData);
    expect(P4.blockParity).toBe(spec.blockParity);
    expect(P4.ackParity).toBe(spec.ackParity);
    expect(P4.riskMaxHops).toBe(spec.riskMaxHops);
    expect(P4.magic).toEqual({ payload: spec.magicPayload, ack: spec.magicAck, reference: spec.magicReference, risk: spec.magicRisk });
    expect(P4.preamble).toEqual(spec.preamble);
    expect(P4.channelIds).toEqual(spec.channelIds);
    expect(P4.preambleKeys).toEqual(spec.preambleKeys);
    for (const [id, b] of Object.entries<any>(P4.bands)) {
      const s = spec.bands[id];
      expect({ bin: b.bin, groups: b.groups, amp: b.amp, sym: b.sym }, id).toEqual({ bin: s.bin, groups: s.groups, amp: s.toneAmplitude, sym: s.symbolSeconds });
    }
    expect(Object.keys(P4.bands).sort()).toEqual(Object.keys(spec.bands).sort());
  });
});
