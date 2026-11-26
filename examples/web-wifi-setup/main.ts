import { decode, encode, listen, send, type Listening } from '../../web/src/index';
import { CODE_VALID_SECONDS, newCode, parseWifi, protect, unprotect, wifiText } from './format';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const ssid = $<HTMLInputElement>('ssid'), pass = $<HTMLInputElement>('pass'), code = $<HTMLInputElement>('code');
const RATE = 48000;

async function message(): Promise<string> {
  const plain = wifiText({ ssid: ssid.value, password: pass.value, security: pass.value ? 'WPA' : 'nopass' });
  return code.value.trim() ? protect(plain, code.value) : plain;
}
async function preview() {
  const m = await message();
  $('preview').textContent = `${code.value.trim() ? 'Protected' : 'PLAIN (anyone nearby can read it)'}: ${m.length} characters, about ${(encode(m, { band: 'mid', sampleRate: RATE }).length / RATE).toFixed(1)} s on mid.`;
}
for (const el of [ssid, pass, code]) el.oninput = () => void preview();
void preview();

$('send').onclick = async () => {
  try {
    const m = await message();
    $('txstatus').textContent = 'playing…';
    const s = await send(m, { band: 'mid', repeat: 2 }); // mid: the most widely audible-to-microphone near-ultrasonic band
    await s.done;
    $('txstatus').textContent = 'Played. The device shows what it understood.';
  } catch (e) { $('txstatus').textContent = `✗ ${(e as Error).message}`; }
};

// ---- the device side ----
let current: { code: string; expires: number } | null = null;
let timer: ReturnType<typeof setInterval> | undefined;
function showCode() {
  current = { code: newCode(), expires: Date.now() + CODE_VALID_SECONDS * 1000 };
  $('screen').textContent = current.code;
  clearInterval(timer);
  timer = setInterval(() => {
    const left = Math.round(((current?.expires ?? 0) - Date.now()) / 1000);
    $('left').textContent = left > 0 ? `valid ${left} s` : 'expired';
    if (left <= 0) clearInterval(timer);
  }, 500);
}
$('newcode').onclick = showCode;

function say(kind: 'theirs' | 'error' | 'alert', text: string) {
  const li = document.createElement('li');
  li.className = kind; li.textContent = text;
  $('got').prepend(li);
}

/** What the device does with a text it heard. */
async function handle(text: string) {
  let plain: string | null = text;
  if (text.startsWith('BSW1|')) {
    if (!current || Date.now() > current.expires) return say('error', 'Protected message heard, but no valid code is showing: ignored. (Press "Show a new code".)');
    plain = await unprotect(text, current.code);
    if (!plain) return say('error', 'Protected message heard but it did not open with the code on screen: ignored.');
    current = null; $('screen').textContent = '–'; $('left').textContent = 'used'; // one-time: burn the code
  } else if (current) {
    say('alert', 'A PLAIN message arrived while a code was showing. A device that requires protection would refuse it.');
  }
  const wifi = parseWifi(plain);
  if (!wifi) return say('error', 'Heard text that is not Wi-Fi credentials.');
  say('theirs', `Would join "${wifi.ssid}" (${wifi.security}, password ${'•'.repeat(Math.min(wifi.password.length, 12))}). On the ESP32 this is where esp_wifi_set_config is called.`);
}

let ear: Listening | null = null;
$('listen').onclick = async () => {
  try {
    ear = await listen({ onFrame: (h) => { if (h.kind === 'payload') void handle(h.payload); } });
    $<HTMLButtonElement>('listen').disabled = true; $<HTMLButtonElement>('stop').disabled = false;
  } catch (e) { say('error', (e as Error).message); }
};
$('stop').onclick = () => { ear?.stop(); ear = null; $<HTMLButtonElement>('listen').disabled = false; $<HTMLButtonElement>('stop').disabled = true; };

$('loop').onclick = async () => {
  if (code.value.trim() && !current) { // make the test self-contained: pretend the device shows the code typed above
    current = { code: code.value, expires: Date.now() + CODE_VALID_SECONDS * 1000 }; $('screen').textContent = code.value;
  }
  const m = await message();
  const heard = decode(encode(m, { band: 'mid', sampleRate: RATE }), RATE)[0];
  if (heard?.kind === 'payload') await handle(heard.payload); else say('error', 'loopback: nothing decoded');
};
