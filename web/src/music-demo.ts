import { encode, mix } from './audio';
import { renderMelody } from './music';

/** The demo melody with `text` repeated on the 'top' band underneath, exactly as the Web Audio demo plays it. */
export function renderMusicDemo(text: string, sampleRate: number): Float32Array {
  return mix(encode(text, { band: 'top', sampleRate, level: 0.5 }), renderMelody(sampleRate));
}
