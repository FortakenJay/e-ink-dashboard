// Runs backend/Code.gs under Node with mocked Apps Script services.
// Usage: node test_backend.js <fixture.json> <nowISO> <out.json>
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const [, , fixturePath, nowIso, outPath] = process.argv;
const fixture = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
const TZ = 'America/Chicago';

/* ---------- time helpers (Intl-based, DST-correct) ---------- */
function partsOf(d, tz) {
  const f = new Intl.DateTimeFormat('en-US', {
    timeZone: tz, hourCycle: 'h23', year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', weekday: 'long', second: '2-digit',
  });
  const o = {};
  for (const p of f.formatToParts(d)) o[p.type] = p.value;
  return { y: +o.year, mo: +o.month, d: +o.day, h: +o.hour, mi: +o.minute, wd: o.weekday };
}
function zoned(y, mo, d, h, mi, tz) {
  const want = Date.UTC(y, mo - 1, d, h, mi);
  let g = want;
  for (let k = 0; k < 3; k++) {
    const p = partsOf(new Date(g), tz);
    g += want - Date.UTC(p.y, p.mo - 1, p.d, p.h, p.mi);
  }
  return new Date(g);
}
const MON = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const pad = n => String(n).padStart(2, '0');
function formatDate(d, tz, pat) {
  const p = partsOf(d, tz);
  const map = {
    'yyyy-MM-dd': `${p.y}-${pad(p.mo)}-${pad(p.d)}`,
    'yyyy-MM-dd HH:mm': `${p.y}-${pad(p.mo)}-${pad(p.d)} ${pad(p.h)}:${pad(p.mi)}`,
    'HH:mm': `${pad(p.h)}:${pad(p.mi)}`,
    'H': String(p.h),
    'EEE': p.wd.slice(0, 3),
    'EEEE': p.wd,
    'MMM d': `${MON[p.mo - 1].slice(0, 3)} ${p.d}`,
    'MMMM d': `${MON[p.mo - 1]} ${p.d}`,
  };
  if (!(pat in map)) throw new Error('pattern not mocked: ' + pat);
  return map[pat];
}
function parseDate(s, tz, pat) {
  assert.strictEqual(pat, 'yyyy-MM-dd HH:mm');
  const m = s.match(/^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d)$/);
  return zoned(+m[1], +m[2], +m[3], +m[4], +m[5], tz);
}

/* ---------- calendar mock from compact fixture ---------- */
// fixture.events: [date, "HH:MM"|null, "HH:MM"|null, title, color, recurring, location?]
function mkEvent([date, s, e, title, color, recurring, loc]) {
  const [y, mo, d] = date.split('-').map(Number);
  const allDay = s === null;
  let start, end;
  if (allDay) {
    start = zoned(y, mo, d, 0, 0, TZ);
    const nd = new Date(Date.UTC(y, mo - 1, d + (e || 1)));
    end = zoned(nd.getUTCFullYear(), nd.getUTCMonth() + 1, nd.getUTCDate(), 0, 0, TZ);
  } else {
    const [sh, sm] = s.split(':').map(Number);
    const [eh, em] = e.split(':').map(Number);
    start = zoned(y, mo, d, sh, sm, TZ);
    end = zoned(y, mo, d, eh, em, TZ);
    if (end < start) end = new Date(end.getTime() + 86400000);
  }
  return {
    getTitle: () => title, getLocation: () => loc || '', isAllDayEvent: () => allDay,
    getColor: () => color, isRecurringEvent: () => !!recurring,
    getAllDayStartDate: () => start, getAllDayEndDate: () => end,
    getStartTime: () => start, getEndTime: () => end,
    getMyStatus: () => 'OWNER', _s: start, _e: end,
  };
}
const events = fixture.events.map(mkEvent);

let weatherJson = fixture.weather;
const props = { GEMINI_API_KEY: 'test-key' };
let geminiCalls = 0, lastPrompt = '';
const geminiReply = {
  summary: 'Deep work, a design review, and a work block back to back - rain 1p-7p, so pack the umbrella before the work block.',
  fact: 'Octopuses have three hearts and blue blood; two hearts stop beating when they swim, which is why they prefer crawling.',
};
const ctx = {
  console,
  Utilities: {
    formatDate, parseDate,
    DigestAlgorithm: { MD5: 'md5' }, Charset: { UTF_8: 'utf8' },
    computeDigest: (alg, s) => [...require('crypto').createHash(alg).update(s, 'utf8').digest()].map(b => b > 127 ? b - 256 : b),
  },
  PropertiesService: {
    getScriptProperties: () => ({ getProperty: k => (k in props ? props[k] : null), setProperty: (k, v) => { props[k] = v; } }),
  },
  Session: { getScriptTimeZone: () => TZ },
  CalendarApp: {
    GuestStatus: { NO: 'NO', YES: 'YES', OWNER: 'OWNER' },
    getDefaultCalendar: () => ({
      getEvents: (a, b) => events.filter(ev => ev._s < b && (ev._e > a || ev._s >= a)),
    }),
    getCalendarById: () => null,
  },
  UrlFetchApp: {
    fetch: (url, opt) => {
      if (url.includes('generativelanguage')) {
        geminiCalls++;
        assert.strictEqual(opt.headers['x-goog-api-key'], 'test-key');
        lastPrompt = JSON.parse(opt.payload).contents[0].parts[0].text;
        const body = { candidates: [{ content: { parts: [{ text: JSON.stringify(geminiReply) }] } }] };
        return { getResponseCode: () => 200, getContentText: () => JSON.stringify(body) };
      }
      return { getResponseCode: () => 200, getContentText: () => JSON.stringify(weatherJson) };
    },
  },
  ContentService: {
    MimeType: { JSON: 'json' },
    createTextOutput: s => ({ setMimeType: () => s }),
  },
  module: { exports: {} },
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../backend/Code.gs'), 'utf8'), ctx);
const api = ctx.module.exports;

/* ---------- unit checks ---------- */
assert.strictEqual(api.clean_('☀️ Wake + breakfast', 60), 'Wake + breakfast');
assert.strictEqual(api.clean_('🌙 Wind down → lights out 11:00', 60), 'Wind down -> lights out 11:00');
assert.strictEqual(api.clean_('Reunión con Ana — café', 60), 'Reunión con Ana - café');
assert.strictEqual(api.clean_('📘 中秋节 活动 planning', 60), '中秋节 活动 planning');
assert.strictEqual(api.clean_('👨‍💻 Hack', 60), 'Hack');
// token check
const bad = vm.runInContext('doGet({parameter:{token:"nope"}})', ctx);
assert.ok(bad.includes('bad token'));
// no DASH_TOKEN set = every request rejected, even an empty token
assert.ok(vm.runInContext('doGet({parameter:{}})', ctx).includes('bad token'));
props.DASH_TOKEN = 'right-token';
assert.ok(vm.runInContext('doGet({parameter:{token:"nope"}})', ctx).includes('bad token'));
// (the right-token check is at the end: a successful doGet calls Gemini and would skew the AI call counts)

/* ---------- full payload ---------- */
const now = new Date(nowIso);
const payload = api.buildPayload_(now);
const json = JSON.stringify(payload);
fs.writeFileSync(outPath, json);

// AI brief: the board's request only bootstraps once per date; after that it reads what's stored
assert.strictEqual(geminiCalls, 1, 'bootstrap call when nothing is stored');
assert.ok(payload.ai && payload.ai.s && payload.ai.f, 'ai present');
assert.ok(lastPrompt.includes('Schedule:'), 'prompt has schedule');
assert.deepStrictEqual(api.buildPayload_(now).ai, payload.ai);
assert.deepStrictEqual(api.buildPayload_(now, true).ai, payload.ai);
assert.strictEqual(geminiCalls, 1, 'unchanged day -> stored, even for the trigger');
// calendar changes: the board keeps showing the stored brief, the trigger refreshes it
const added = [Object.keys(JSON.parse(props.AI_BRIEFS))[0], '18:00', '18:30', 'Coffee with Ana', '', false];
fixture.events.push(added);
events.push(mkEvent(added));
api.buildPayload_(now);
assert.strictEqual(geminiCalls, 1, 'board request never waits on Gemini');
geminiReply.summary = 'Updated brief.';
assert.strictEqual(api.buildPayload_(now, true).ai.s, 'Updated brief.');
assert.strictEqual(geminiCalls, 2, 'trigger refetches after a change');
assert.ok(JSON.parse(props.AI_FACTS).length === 1, 'fact kept for the day');
delete props.GEMINI_API_KEY;
assert.strictEqual(api.buildPayload_(now).ai, null, 'no key -> no ai');
props.GEMINI_API_KEY = 'test-key';
// OTA advert only when configured
assert.strictEqual(payload.fw, undefined);
api.CONFIG.FW_VERSION = 3; api.CONFIG.FW_URL = 'https://example.com/fw.bin';
assert.strictEqual(JSON.stringify(api.buildPayload_(now).fw), '{"v":3,"u":"https://example.com/fw.bin"}');
api.CONFIG.FW_VERSION = 0; api.CONFIG.FW_URL = '';
assert.strictEqual(api.hm_(570), '9:30a');
assert.strictEqual(api.hm_(780), '1p');
assert.strictEqual(api.hm_(0), '12a');

// sanity checks on the real-calendar fixture
const d0 = payload.days[0];
assert.strictEqual(payload.days.length, 7);
for (const d of payload.days) {
  for (let i = 1; i < d.ev.length; i++) assert.ok(d.ev[i - 1].s <= d.ev[i].s, 'sorted');
  for (const ev of d.ev) assert.ok(ev.s >= 0 && ev.e <= 1440 && ev.e >= ev.s, 'bounds ' + JSON.stringify(ev));
}
console.log(`now=${nowIso} bytes=${json.length} today=${d0.date} (${d0.dl}) events/day=${payload.days.map(d => d.ev.length).join(',')}`);
console.log('keys:', JSON.stringify(payload.keys));
console.log('ai:', JSON.stringify(payload.ai));
console.log('prompt:\n' + lastPrompt);
console.log('notes:', payload.days.map(d => `${d.dow}: ${d.w && d.w.n}`).join(' | '));
console.log('week anchors:', payload.days.slice(1).map(d => d.dow + '=' + d.ev.filter(e => e.k > 0).map(e => e.t).concat(d.ad.map(a => '[' + a.t + ']')).join('; ')).join(' | '));
const sun = payload.days.find(d => d.date === '2026-11-01');
if (sun) console.log('DST Sunday first/last:', sun.ev[0] && sun.ev[0].t, sun.ev[0] && sun.ev[0].s, '/', sun.ev[sun.ev.length - 1].t, sun.ev[sun.ev.length - 1].s);

// token check, success path: the DASH_TOKEN script property is accepted
assert.ok(vm.runInContext('doGet({parameter:{token:"right-token"}})', ctx).includes('"ok":true'));
console.log('token: ok');
