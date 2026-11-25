// Public API of bysound (bysound v4). See README.md.
export { encode, decode, send, listen, SoundError } from './audio';
export type { EncodeOptions, SendOptions, Sending, ListenOptions, Listening } from './audio';
export { BANDS, BAND_IDS, ROBUST_BANDS, hasRobust, SPACING_HZ, TONES, VERSION, GAP_SECONDS, toneHz } from './modem/protocol';
export type { Band, BandId, Profile } from './modem/protocol';
export type { Heard } from './modem/receiver';
export type { LevelEvent } from './modem/host';
