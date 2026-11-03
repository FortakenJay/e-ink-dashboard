"""Builds tools/fixture.json: this week's real calendar (Oct 26 - Nov 2, 2026,
transcribed from the Google Calendar connector) + a synthetic Open-Meteo reply
(the sandbox can't reach Open-Meteo, so weather is plausible but made up)."""
import json, math, datetime as dt

R = True
mon = [
    ("06:30", "07:00", "☀️ Wake + breakfast", "5"),
    ("08:00", "09:15", "📘 Physics lecture", "9"),
    ("09:30", "10:45", "📘 NLP", "9"),
    ("11:00", "12:00", "📚 Homework", "7"),
    ("12:00", "12:40", "🍽️ Lunch", "5"),
    ("13:00", "16:30", "🛠️ Project deep work", "10"),
    ("17:15", "18:30", "🏋️ Gym", "11"),
]
evening = [
    ("18:30", "19:45", "🍳 Cook + dinner", "5"),
    ("19:45", "20:45", "🧠 NeetCode (timed, no AI)", "3"),
    ("20:45", "21:30", "📨 Job applications", "6"),
    ("21:30", "22:30", "🎹 Free time", "2"),
    ("22:30", "23:00", "🌙 Wind down → lights out 11:00", "8"),
]
no_jobs = [e for e in evening if "Job" not in e[2]]
tue = [
    ("06:30", "07:00", "☀️ Wake + breakfast", "5"),
    ("07:15", "09:15", "📚 Homework (Graphics + Stats)", "7"),
    ("09:30", "10:45", "📘 Computer Graphics", "9"),
    ("11:00", "15:00", "AV Work", ""),
    ("15:00", "15:25", "🍽️ Quick lunch (between AV and Stats)", "5"),
    ("15:30", "16:45", "📘 Statistics", "9"),
    ("17:15", "18:30", "🏋️ Gym", "11"),
]
wed = [
    ("06:30", "07:00", "☀️ Wake + breakfast", "5"),
    ("08:00", "09:15", "📘 Physics lecture", "9"),
    ("09:30", "10:45", "📘 NLP", "9"),
    ("11:00", "12:00", "📚 Homework", "7"),
    ("12:00", "12:40", "🍽️ Lunch", "5"),
    ("12:45", "14:00", "🧪 Physics lab", "9"),
    ("14:30", "18:30", "Work", ""),
]
thu = [("06:30", "07:00", "☀️ Wake + breakfast", "5"),
       ("07:15", "09:15", "🛠️ Project deep work (morning)", "10")] + tue[2:]
fri = [
    ("06:30", "07:00", "☀️ Wake + breakfast", "5"),
    ("07:30", "16:30", "Work", ""),
    ("16:45", "18:15", "📚 Homework / catch-up", "7"),
]
sat = [
    ("06:30", "07:00", "☀️ Wake + breakfast", "5"),
    ("09:00", "12:00", "🛠️ Project deep work", "10"),
    ("12:00", "12:40", "🍽️ Lunch", "5"),
    ("13:00", "14:00", "📝 Engineering Ethics", "9"),
    ("14:00", "15:00", "🎥 Content (Sat: film/edit · Sun: write)", "4"),
    ("17:15", "18:30", "🏋️ Gym", "11"),
]
sun = [
    ("06:30", "07:00", "☀️ Wake + breakfast", "5"),
    ("10:00", "12:00", "📚 Homework (week ahead)", "7"),
    ("12:00", "12:40", "🍽️ Lunch", "5"),
    ("14:00", "15:00", "🎥 Content (Sat: film/edit · Sun: write)", "4"),
    ("18:00", "18:30", "📊 Weekly review", "1"),
]
plan = {
    "2026-10-26": mon + evening, "2026-10-27": tue + evening, "2026-10-28": wed[:] + evening,
    "2026-10-29": thu + evening, "2026-10-30": fri + no_jobs, "2026-10-31": sat + no_jobs,
    "2026-11-01": sun + evening, "2026-11-02": mon + evening, "2026-11-03": tue + evening,
}
events = []
for date, evs in plan.items():
    for s, e, t, c in evs:
        events.append([date, s, e, t, c, R])
# one-offs straight from the calendar
events.append(["2026-10-27", "01:15", "01:45", "OpenCV AI Competition (AWS): SUBMISSION DEADLINE 1:45 AM", "", False, "Online"])
events.append(["2026-10-30", None, 1, "NVIDIA Hackathon: SUBMISSION DEADLINE", "", False])

# ---------------- synthetic weather (Hammond, late Oct) ----------------
days = [  # date, lo, hi, code, rain hours (pop), gust, special code during rain
    ("2026-10-26", 41, 54, 3, {}, 22, None),
    ("2026-10-27", 39, 51, 63, {h: p for h, p in zip(range(13, 19), [45, 70, 85, 80, 60, 40])}, 29, 63),
    ("2026-10-28", 34, 46, 3, {}, 33, None),
    ("2026-10-29", 30, 44, 2, {}, 18, None),
    ("2026-10-30", 36, 50, 1, {}, 15, None),
    ("2026-10-31", 42, 57, 0, {}, 12, None),
    ("2026-11-01", 40, 52, 80, {h: 50 for h in range(15, 19)}, 20, 80),
]
times, temp, app, pop, code, gust = [], [], [], [], [], []
for date, lo, hi, c, rain, g, rc in days:
    for h in range(24):
        x = (math.cos((h - 15) / 24 * 2 * math.pi) + 1) / 2  # 1 at 3pm, 0 at 3am
        t = lo + (hi - lo) * x
        wind = g * (0.5 + 0.5 * x)
        times.append(f"{date}T{h:02d}:00")
        temp.append(round(t, 1))
        app.append(round(t - 4 - wind * 0.25, 1))
        pop.append(rain.get(h, 5 if c < 50 else 20))
        code.append(rc if h in rain else (c if c < 50 else 3))
        gust.append(round(wind, 1))
weather = {
    "current": {"temperature_2m": 46.8, "apparent_temperature": 41.2, "weather_code": 3,
                "wind_speed_10m": 11.6, "wind_gusts_10m": 21.0, "is_day": 0},
    "hourly": {"time": times, "temperature_2m": temp, "apparent_temperature": app,
               "precipitation_probability": pop, "weather_code": code, "wind_gusts_10m": gust},
    "daily": {
        "time": [d[0] for d in days], "weather_code": [d[3] for d in days],
        "temperature_2m_max": [d[2] for d in days], "temperature_2m_min": [d[1] for d in days],
        "precipitation_probability_max": [max(d[4].values()) if d[4] else 5 for d in days],
        "sunrise": [f"{d[0]}T07:{16 + i:02d}" for i, d in enumerate(days)],
        "sunset": [f"{d[0]}T17:{52 - i:02d}" for i, d in enumerate(days)],
    },
}
json.dump({"events": events, "weather": weather}, open(__file__.replace("make_fixture.py", "fixture.json"), "w"), ensure_ascii=False)
print(len(events), "events")
