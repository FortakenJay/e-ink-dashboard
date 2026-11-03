/**
 * Bedside e-ink dashboard — backend (Google Apps Script web app)
 *
 * Reads your Google Calendar + Open-Meteo weather and returns one compact JSON
 * blob for the ESP32. All the "smart" stuff (recurrence expansion, time zones,
 * emoji stripping, rain windows, deadline detection) happens here so the
 * firmware only has to draw.
 *
 * Deploy: Deploy > New deployment > type "Web app"
 *         Execute as: Me     Who has access: Anyone
 * Then put the /exec URL + ?token=... into the firmware's config.h.
 */

const CONFIG = {
  // Shared secret. The ESP32 sends it as ?token=...  Make it long and random.
  TOKEN: 'CHANGE-ME-to-a-long-random-string',

  // 'primary' = jihbin.luo@gmail.com. Add more calendar IDs if you ever want them
  // (e.g. 'en.usa#holiday@group.v.calendar.google.com' for US holidays).
  CALENDARS: ['primary'],

  TZ: 'America/Chicago',
  LAT: 41.584,              // PNW Hammond campus
  LON: -87.474,
  METRIC: false,            // true = °C + km/h
  H24: false,               // 24-hour times in weather notes

  DAYS: 7,                  // today + 6
  KEY_LOOKAHEAD_DAYS: 14,   // how far ahead the "coming up" box looks

  // Week view only shows "anchor" events so it isn't flooded by daily routine.
  // Anchors = one-off events, all-day events, key events, and these colors.
  // '9' = Blueberry (your classes), '' = default calendar color (Work / AV Work).
  ANCHOR_COLORS: ['9', ''],
  ANCHOR_TITLES: /^(av )?work\b/i,   // backup in case a color isn't reported

  // Titles matching this become "key" events: flagged everywhere + countdown box.
  KEY_PATTERN: /\b(deadline|due|submission|submit|exam|midterm|final|quiz|interview|presentation|demo day)\b/i,

  MAX_EVENTS_PER_DAY: 24,
  RAIN_POP: 40,             // % chance that counts as "it will rain"
  GUST_WARN: 25,            // mph (use ~40 if METRIC)
  COLD_FEELS: 32,           // °F feels-like (use 0 if METRIC)
  WAKING_HOURS: [7, 22],    // weather notes only care about these hours

  // Daily brief + fun fact from Gemini (Google AI Studio). The API key lives in
  // Project Settings > Script properties as GEMINI_API_KEY — no key = box hidden.
  AI_MODEL: 'gemini-2.5-flash',
  TOMORROW_FROM_MIN: 21 * 60 + 30,   // match TOMORROW_VIEW_FROM in config.h
  AI_FACT_HISTORY: 20,               // remember this many facts so it doesn't repeat

  // Over-the-air firmware: set FW_VERSION above the board's (eink_dashboard.ino) and FW_URL to
  // an HTTPS link to the compiled .bin (e.g. a GitHub release asset). 0 / '' = no update.
  FW_VERSION: 0,
  FW_URL: '',
};

/* ------------------------------------------------------------------ */

function doGet(e) {
  const p = (e && e.parameter) || {};
  if (p.token !== CONFIG.TOKEN) return json_({ ok: false, error: 'bad token' });
  try {
    return json_(buildPayload_(new Date()));
  } catch (err) {
    console.error(err);
    return json_({ ok: false, error: String(err).slice(0, 100) });
  }
}

/** Run this from the editor to sanity-check the output (View > Logs). */
function testPayload() {
  const out = JSON.stringify(buildPayload_(new Date()));
  console.log(out.length + ' bytes');
  console.log(out);
}

function json_(o) {
  return ContentService.createTextOutput(JSON.stringify(o))
    .setMimeType(ContentService.MimeType.JSON);
}

/* ------------------------------------------------------------------ */

/**
 * Hourly trigger (see installTriggers): asks Gemini ahead of time so the board's request
 * never waits on it. Also covers the next hour, so tomorrow's brief is ready for the 9:30 PM flip.
 */
function refreshAi() {
  const now = new Date();
  const later = new Date(now.getTime() + 3600000);
  const a = buildPayload_(now, true);
  const b = buildPayload_(later, true);
  console.log('now: ' + JSON.stringify(a.ai) + '\nin an hour: ' + JSON.stringify(b.ai));
}

/** Run once from the editor: (re)creates the hourly refreshAi trigger. */
function installTriggers() {
  ScriptApp.getProjectTriggers().forEach(function (t) {
    if (t.getHandlerFunction() === 'refreshAi') ScriptApp.deleteTrigger(t);
  });
  ScriptApp.newTrigger('refreshAi').timeBased().everyHours(1).create();
  refreshAi();
}

/** fetchAi: true = may call Gemini (trigger); false = stored results only (the board's request). */
function buildPayload_(now, fetchAi) {
  const todayStr = fmt_(now, 'yyyy-MM-dd');
  const horizon = Math.max(CONFIG.DAYS, CONFIG.KEY_LOOKAHEAD_DAYS);
  const starts = [];
  for (let i = 0; i <= horizon; i++) starts.push(dayStart_(todayStr, i));

  const raw = fetchEvents_(starts[0], starts[horizon]);
  const wx = fetchWeather_(now);

  const days = [];
  for (let i = 0; i < CONFIG.DAYS; i++) {
    days.push(buildDay_(starts[i], starts[i + 1], raw, wx, i < 3));
  }

  const keys = buildKeys_(raw, now, todayStr);
  const tomorrow = mins_(now) >= CONFIG.TOMORROW_FROM_MIN;
  return {
    ok: true,
    v: 1,
    ts: Math.floor(now.getTime() / 1000),
    units: { t: CONFIG.METRIC ? 'C' : 'F', w: CONFIG.METRIC ? 'km/h' : 'mph' },
    cur: wx ? wx.current : null,
    days: days,
    keys: keys,
    ai: buildAi_(days[tomorrow ? 1 : 0], keys, tomorrow, !!fetchAi),
    fw: CONFIG.FW_VERSION && CONFIG.FW_URL ? { v: CONFIG.FW_VERSION, u: CONFIG.FW_URL } : undefined,
  };
}

/** Midnight (as a Date) of today+offset in CONFIG.TZ. Noon math dodges DST. */
function dayStart_(todayStr, offset) {
  const noon = Utilities.parseDate(todayStr + ' 12:00', CONFIG.TZ, 'yyyy-MM-dd HH:mm');
  const ds = fmt_(new Date(noon.getTime() + offset * 86400000), 'yyyy-MM-dd');
  return Utilities.parseDate(ds + ' 00:00', CONFIG.TZ, 'yyyy-MM-dd HH:mm');
}

function fetchEvents_(start, end) {
  const scriptTz = Session.getScriptTimeZone();
  const out = [];
  CONFIG.CALENDARS.forEach(function (id) {
    const cal = id === 'primary' ? CalendarApp.getDefaultCalendar() : CalendarApp.getCalendarById(id);
    if (!cal) return;
    cal.getEvents(start, end).forEach(function (ev) {
      let status = null;
      try { status = ev.getMyStatus(); } catch (e) { /* not a guest */ }
      if (status === CalendarApp.GuestStatus.NO) return;          // skip declined

      const rawTitle = ev.getTitle() || '(no title)';
      const o = {
        title: clean_(rawTitle, 60),
        loc: clean_(ev.getLocation() || '', 30),
        allDay: ev.isAllDayEvent(),
        color: String(ev.getColor() || ''),
        recurring: ev.isRecurringEvent(),
        key: CONFIG.KEY_PATTERN.test(rawTitle),
      };
      if (o.allDay) {
        // getAllDayEndDate() is the day AFTER the event ends (exclusive)
        o.startDate = Utilities.formatDate(ev.getAllDayStartDate(), scriptTz, 'yyyy-MM-dd');
        o.endDate = Utilities.formatDate(ev.getAllDayEndDate(), scriptTz, 'yyyy-MM-dd');
      } else {
        o.start = ev.getStartTime();
        o.end = ev.getEndTime();
      }
      out.push(o);
    });
  });
  return out;
}

/** 0 = routine, 1 = anchor (shows in week view), 2 = key (deadline/exam/...) */
function kindOf_(o) {
  if (o.key) return 2;
  if (o.allDay || !o.recurring || CONFIG.ANCHOR_COLORS.indexOf(o.color) >= 0) return 1;
  if (CONFIG.ANCHOR_TITLES.test(o.title)) return 1;
  return 0;
}

function buildDay_(ds, de, raw, wx, withHourly) {
  const date = fmt_(ds, 'yyyy-MM-dd');
  const next = fmt_(de, 'yyyy-MM-dd');
  const day = {
    date: date,
    dow: fmt_(ds, 'EEE'),        // Mon
    dl: fmt_(ds, 'EEEE'),        // Monday
    md: fmt_(ds, 'MMM d'),       // Oct 26
    ml: fmt_(ds, 'MMMM d'),      // October 26
    ev: [],
    ad: [],
  };

  raw.forEach(function (o) {
    if (o.allDay) {
      const endEx = o.endDate > o.startDate ? o.endDate : next;
      if (o.startDate <= date && date < endEx) day.ad.push({ t: o.title, k: kindOf_(o) });
      return;
    }
    // overlaps [ds, de) — zero-length events count if they start inside
    if (o.start < de && (o.end > ds || o.start >= ds)) {
      day.ev.push({
        s: o.start < ds ? 0 : mins_(o.start),
        e: o.end >= de ? 1440 : mins_(o.end),
        t: o.title,
        l: o.loc,
        k: kindOf_(o),
        f: (o.start < ds ? 1 : 0) | (o.end > de ? 2 : 0),   // 1 = from yesterday, 2 = into tomorrow
      });
    }
  });

  day.ev.sort(function (a, b) { return a.s - b.s || a.e - b.e; });
  day.ev = day.ev.slice(0, CONFIG.MAX_EVENTS_PER_DAY);
  day.ad.sort(function (a, b) { return b.k - a.k; });
  day.ad = day.ad.slice(0, 4);

  if (wx && wx.days[date]) {
    day.w = wx.days[date];
    if (withHourly && wx.hourly[date]) day.h = wx.hourly[date];
  }
  return day;
}

/** Next few key events (deadlines, exams...) for the countdown box. */
function buildKeys_(raw, now, todayStr) {
  const seen = {};
  return raw
    .filter(function (o) { return o.key && (o.allDay ? o.endDate > todayStr : o.end > now); })
    .map(function (o) {
      const d = o.allDay ? o.startDate : fmt_(o.start, 'yyyy-MM-dd');
      return {
        t: o.title,
        d: d < todayStr ? todayStr : d,
        m: o.allDay ? -1 : mins_(o.start),
        sortKey: (o.allDay ? o.startDate + ' 00:00' : fmt_(o.start, 'yyyy-MM-dd HH:mm')),
      };
    })
    .sort(function (a, b) { return a.sortKey < b.sortKey ? -1 : a.sortKey > b.sortKey ? 1 : 0; })
    .filter(function (k) { if (seen[k.t]) return false; seen[k.t] = true; return true; })
    .slice(0, 4)
    .map(function (k) {
      const dt = Utilities.parseDate(k.d + ' 12:00', CONFIG.TZ, 'yyyy-MM-dd HH:mm');
      return { t: shortKey_(k.t), d: k.d, w: fmt_(dt, 'EEE'), m: k.m };
    });
}

/* ---------------------------- weather ---------------------------- */

function fetchWeather_(now) {
  const url = 'https://api.open-meteo.com/v1/forecast'
    + '?latitude=' + CONFIG.LAT + '&longitude=' + CONFIG.LON
    + '&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,wind_gusts_10m,is_day'
    + '&hourly=temperature_2m,apparent_temperature,precipitation_probability,weather_code,wind_gusts_10m'
    + '&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset'
    + '&timezone=' + encodeURIComponent(CONFIG.TZ)
    + '&forecast_days=' + CONFIG.DAYS
    + (CONFIG.METRIC ? '&wind_speed_unit=kmh' : '&temperature_unit=fahrenheit&wind_speed_unit=mph');
  try {
    const r = UrlFetchApp.fetch(url, { muteHttpExceptions: true });
    if (r.getResponseCode() !== 200) {
      console.warn('open-meteo HTTP ' + r.getResponseCode());
      return null;
    }
    return shapeWeather_(JSON.parse(r.getContentText()), now);
  } catch (e) {
    console.warn('weather failed: ' + e);
    return null;
  }
}

function shapeWeather_(j, now) {
  const R = Math.round;
  const c = j.current || {};
  const current = {
    t: R(c.temperature_2m), f: R(c.apparent_temperature), c: c.weather_code,
    w: R(c.wind_speed_10m), g: R(c.wind_gusts_10m), d: c.is_day ? 1 : 0,
  };

  // group hourly rows by local date / hour
  const H = j.hourly, byDate = {};
  for (let i = 0; i < H.time.length; i++) {
    const parts = H.time[i].split('T');
    const hour = parseInt(parts[1], 10);
    (byDate[parts[0]] = byDate[parts[0]] || [])[hour] = {
      t: H.temperature_2m[i], f: H.apparent_temperature[i], c: H.weather_code[i],
      p: H.precipitation_probability[i] || 0, g: H.wind_gusts_10m[i] || 0,
    };
  }

  const nowDate = fmt_(now, 'yyyy-MM-dd');
  const nowHour = parseInt(fmt_(now, 'H'), 10);
  const D = j.daily, days = {}, hourly = {};
  for (let i = 0; i < D.time.length; i++) {
    const date = D.time[i];
    const hrs = byDate[date] || [];
    const from = date === nowDate ? Math.max(nowHour, CONFIG.WAKING_HOURS[0]) : CONFIG.WAKING_HOURS[0];
    days[date] = {
      hi: R(D.temperature_2m_max[i]), lo: R(D.temperature_2m_min[i]),
      c: D.weather_code[i], p: D.precipitation_probability_max[i] || 0,
      sr: String(D.sunrise[i]).split('T')[1] || '', ss: String(D.sunset[i]).split('T')[1] || '',
      n: notes_(hrs, from, CONFIG.WAKING_HOURS[1]),
    };
    const row = [];
    let last = { t: 0, c: 0, p: 0 };
    for (let h = 0; h < 24; h++) {
      const x = hrs[h] || last;            // spring-forward gap -> repeat previous hour
      row.push([R(x.t), x.c, x.p]);
      last = x;
    }
    hourly[date] = row;
  }
  return { current: current, days: days, hourly: hourly };
}

/** "Rain 2p-5p · Gusts 31 mph · Feels 24°" — what matters when you walk everywhere. */
function notes_(hrs, fromH, toH) {
  const parts = [];
  const runs = [];
  let cur = null;
  for (let h = fromH; h <= toH; h++) {
    const x = hrs[h];
    if (x && x.p >= CONFIG.RAIN_POP) {
      if (!cur) cur = { a: h, b: h, codes: [] };
      cur.b = h;
      cur.codes.push(x.c);
    } else if (cur) { runs.push(cur); cur = null; }
  }
  if (cur) runs.push(cur);

  if (runs.length) {
    const codes = [].concat.apply([], runs.map(function (r) { return r.codes; }));
    const has = function (f) { return codes.some(f); };
    const kind = has(function (c) { return c >= 95; }) ? 'Storms'
      : has(function (c) { return (c >= 71 && c <= 77) || c === 85 || c === 86; }) ? 'Snow'
      : has(function (c) { return [56, 57, 66, 67].indexOf(c) >= 0; }) ? 'Ice'
      : 'Rain';
    const spans = runs.slice(0, 2).map(function (r) {
      return (r.a <= fromH && fromH > CONFIG.WAKING_HOURS[0] ? 'now' : hr_(r.a)) + '-' + hr_(r.b + 1);
    });
    parts.push(kind + ' ' + spans.join(', '));
  }

  let gmax = 0, fmin = 999;
  for (let h = fromH; h <= toH; h++) {
    const x = hrs[h];
    if (!x) continue;
    gmax = Math.max(gmax, x.g);
    fmin = Math.min(fmin, x.f);
  }
  if (gmax >= CONFIG.GUST_WARN) parts.push('Gusts ' + Math.round(gmax) + (CONFIG.METRIC ? ' km/h' : ' mph'));
  if (fmin <= CONFIG.COLD_FEELS) parts.push('Feels ' + Math.round(fmin) + '°');
  return parts.join(' · ');
}

/* ------------------------------ AI ------------------------------- */

/**
 * { s: "one-or-two sentence brief of the day", f: "fun fact" } or null.
 * The brief is stored with a hash of the day's contents, so Gemini only runs again when the
 * calendar or forecast actually changes; the fact is picked once per date.
 * With fetch = false (the board's request) it never calls Gemini unless nothing at all is
 * stored for that date yet; a slightly outdated brief is fine until the hourly trigger runs.
 */
function buildAi_(day, keys, tomorrow, fetch) {
  const props = PropertiesService.getScriptProperties();
  const apiKey = props.getProperty('GEMINI_API_KEY');
  if (!apiKey || !day) return null;

  const facts = JSON.parse(props.getProperty('AI_FACTS') || '[]');    // [{d, f}], newest last
  const briefs = JSON.parse(props.getProperty('AI_BRIEFS') || '{}');  // {date: {h, s}}
  const todays = facts.filter(function (x) { return x.d === day.date; })[0];
  const stored = briefs[day.date];

  const brief = describeDay_(day, keys, tomorrow);
  const hash = digest_(brief);
  if (todays && stored && stored.h === hash) return { s: stored.s, f: todays.f };
  if (!fetch && (todays || stored)) return { s: stored ? stored.s : '', f: todays ? todays.f : '' };

  const prompt = [
    'You write the text for a small black-and-white e-ink dashboard on a college student\'s nightstand.',
    'Here is ' + (tomorrow ? 'TOMORROW' : 'TODAY') + ':',
    brief,
    '',
    'Return JSON with:',
    '"summary": at most 160 characters. A friendly heads-up about ' + (tomorrow ? 'tomorrow' : 'today') +
      ' — what the day looks like, the one thing not to forget, and weather only if it changes plans' +
      ' (they walk everywhere). Plain text, no emoji, no lists, don\'t just list every event.',
    '"fact": at most 200 characters. One surprising, true fun fact (science, history, space, nature,' +
      ' language, tech...). Plain text, no emoji, no "Did you know".' +
      (facts.length ? ' Not about the same thing as any of these: ' +
        facts.map(function (x) { return x.f; }).join(' | ') : ''),
  ].join('\n');

  const out = callGemini_(apiKey, prompt);
  if (!out) return todays || stored ? { s: stored ? stored.s : '', f: todays ? todays.f : '' } : null;

  const s = clean_(out.summary || '', 180);
  const f = todays ? todays.f : clean_(out.fact || '', 220);
  if (!todays && f) {
    facts.push({ d: day.date, f: f });
    props.setProperty('AI_FACTS', JSON.stringify(facts.slice(-CONFIG.AI_FACT_HISTORY)));
  }
  if (s) {
    briefs[day.date] = { h: hash, s: s };
    Object.keys(briefs).sort().slice(0, -3).forEach(function (d) { delete briefs[d]; });  // keep 3 dates
    props.setProperty('AI_BRIEFS', JSON.stringify(briefs));
  }
  return s || f ? { s: s, f: f } : null;
}

/** Compact plain-text version of the day for the prompt. */
function describeDay_(day, keys, tomorrow) {
  const lines = [day.dl + ', ' + day.ml + '.'];
  if (day.ad.length) lines.push('All day: ' + day.ad.map(function (a) { return a.t; }).join('; '));
  if (day.ev.length) {
    lines.push('Schedule: ' + day.ev.map(function (e) {
      return hm_(e.s) + '-' + hm_(e.e) + ' ' + e.t + (e.k === 2 ? ' (IMPORTANT)' : '');
    }).join('; '));
  } else {
    lines.push('Nothing on the calendar.');
  }
  if (day.w) {
    lines.push('Weather: high ' + day.w.hi + '°, low ' + day.w.lo + '°, ' + day.w.p + '% max precip chance' +
      (day.w.n ? ', ' + day.w.n : '') + '.');
  }
  if (keys.length) {
    lines.push('Coming up: ' + keys.map(function (k) { return k.t + ' on ' + k.w + ' ' + k.d; }).join('; '));
  }
  return lines.join('\n');
}

function callGemini_(apiKey, prompt) {
  const url = 'https://generativelanguage.googleapis.com/v1beta/models/' + CONFIG.AI_MODEL + ':generateContent';
  const gen = {
    temperature: 0.9,
    maxOutputTokens: 400,
    responseMimeType: 'application/json',
    responseSchema: {
      type: 'OBJECT',
      properties: { summary: { type: 'STRING' }, fact: { type: 'STRING' } },
      required: ['summary', 'fact'],
    },
  };
  if (/^gemini-2\.5-flash/.test(CONFIG.AI_MODEL)) gen.thinkingConfig = { thinkingBudget: 0 };  // fast + cheap
  try {
    const r = UrlFetchApp.fetch(url, {
      method: 'post',
      contentType: 'application/json',
      headers: { 'x-goog-api-key': apiKey },
      payload: JSON.stringify({ contents: [{ role: 'user', parts: [{ text: prompt }] }], generationConfig: gen }),
      muteHttpExceptions: true,
    });
    if (r.getResponseCode() !== 200) {
      console.warn('gemini HTTP ' + r.getResponseCode() + ': ' + r.getContentText().slice(0, 300));
      return null;
    }
    const j = JSON.parse(r.getContentText());
    const text = j.candidates[0].content.parts.map(function (p) { return p.text || ''; }).join('');
    return JSON.parse(text);
  } catch (e) {
    console.warn('gemini failed: ' + e);
    return null;
  }
}

/** Run from the editor to see the prompt input and what Gemini returns (View > Logs). */
function testAi() {
  const now = new Date();
  const p = buildPayload_(now, true);
  const tomorrow = mins_(now) >= CONFIG.TOMORROW_FROM_MIN;
  console.log(describeDay_(p.days[tomorrow ? 1 : 0], p.keys, tomorrow));
  console.log(JSON.stringify(p.ai));
}

/* ---------------------------- helpers ---------------------------- */

function hm_(m) {
  if (m >= 1440) return 'midnight';
  const h = Math.floor(m / 60), mm = m % 60;
  return hr_(h).replace(/[ap]$/, '') + (mm ? ':' + (mm < 10 ? '0' : '') + mm : '') + (CONFIG.H24 ? '' : h < 12 ? 'a' : 'p');
}

function digest_(s) {
  return Utilities.computeDigest(Utilities.DigestAlgorithm.MD5, s, Utilities.Charset.UTF_8)
    .map(function (b) { return ((b + 256) % 256).toString(16); }).join('').slice(0, 16);
}

/** "NVIDIA Hackathon: SUBMISSION DEADLINE" -> "NVIDIA Hackathon" (the box already says it's coming up) */
function shortKey_(t) {
  const s = t.replace(/\s*[:\-|,]?\s*(submission|deadline|due\b).*$/i, '').trim();
  return s || t;
}

function fmt_(d, pattern) { return Utilities.formatDate(d, CONFIG.TZ, pattern); }

function mins_(d) {
  const hm = fmt_(d, 'HH:mm').split(':');
  return parseInt(hm[0], 10) * 60 + parseInt(hm[1], 10);
}

function hr_(h) {
  h = h % 24;
  if (CONFIG.H24) return String(h);
  return (h % 12 === 0 ? 12 : h % 12) + (h < 12 ? 'a' : 'p');
}

/**
 * Make text drawable on the panel: the fonts cover Latin-1 (so Spanish
 * accents work) and GB2312 Chinese. Emoji and other symbols are dropped.
 */
function clean_(s, max) {
  if (!s) return '';
  s = String(s)
    .replace(/[‘’‚′]/g, "'")
    .replace(/[“”„″]/g, '"')
    .replace(/[–—−]/g, '-')
    .replace(/…/g, '...')
    .replace(/[→➡➔⇒]/g, '->')
    .replace(/←/g, '<-')
    .replace(/[   ]/g, ' ')
    .replace(/[^ -~¡-ÿ　-〿一-鿿！-～]/gu, '')
    .replace(/\s+/g, ' ')
    .trim();
  if (s.length > max) s = s.slice(0, max - 3).trim() + '...';
  return s;
}

// Exported for local testing under Node (ignored by Apps Script).
if (typeof module !== 'undefined') {
  module.exports = { CONFIG, buildPayload_, shapeWeather_, notes_, clean_, buildDay_, kindOf_, describeDay_, hm_ };
}
