// Public API of bysound v4. See README.md.

// Modem: one frame of text over sound, offline (encode / decode) or through the speaker and microphone (send / listen).
export { encode, decode, send, listen, mix, SoundError } from './audio';
export type { EncodeOptions, SendOptions, Sending, ListenOptions, Listening } from './audio';
export { maxPayloadBytes, TooLongError } from './modem/codec';
export type { Frame } from './modem/codec';
export { BANDS, BAND_IDS, ROBUST_BANDS, hasRobust, SPACING_HZ, TONES, VERSION, GAP_SECONDS, MAX_FRAME_SECONDS, toneHz } from './modem/protocol';
export type { Band, BandId, Profile } from './modem/protocol';
export type { Heard } from './modem/receiver';
export type { LevelEvent } from './modem/host';
export { renderMelody } from './music';

// Session layer (docs/SESSION.md): acks and receipts, the confirm gate, other-emitter detection, risk frames, security levels.
export { sendSession, receiveSession } from './session/browser';
export type { SendSession, ReceiveSession } from './session/browser';
export { SenderSession } from './session/sender';
export type { SenderOptions, SenderState, Delivered } from './session/sender';
export { ReceiverSession } from './session/receiver';
export type { ReceiverOptions, ReceiverState, Received, Confirmation } from './session/receiver';
export { SessionError, RiskMemory } from './session/common';
export type { Alert, QuarantineNotice, SessionErrorCode } from './session/common';
export { SECURITY_LEVELS, DEFAULT_SECURITY_LEVEL, resolveSecurity } from './session/security';
export type { SecurityLevel, Security, SecurityOverrides, RiskSignal } from './session/security';
export { Ears } from './session/ears';
export type { AirEvent, Platform } from './session/ears';
export { ControlListener } from './session/controlListener';
export type { BandNoise, ControlHeard } from './session/controlListener';
export { ackFrame, riskFrame, parseAck, parseRisk, payloadId, fingerprint } from './session/frames';
export type { AckFrame, RiskFrame, RiskReason, Role } from './session/frames';
export {
  DEFAULT_CHANNELS, HOP_REPETITIONS, ROBUST_AFTER_REPETITIONS, CALIBRATION_USES, channelOrder, memoryCalibrationStore, localStorageCalibrationStore,
} from './session/adapt';
export type { Ambient, Calibration, CalibrationStore } from './session/adapt';
