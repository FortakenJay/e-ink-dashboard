#!/usr/bin/env bash
# Rebuilds the PC preview and renders the scenarios used to check the layout.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(dirname "$HERE"); ROOT=$(dirname "$TOOLS")
# Usage: LIBS=/path/to/libs tools/preview/build_and_render.sh
# LIBS must contain Adafruit-GFX-Library/, U8g2_for_Adafruit_GFX/, and ArduinoJson/.
if [ -z "${LIBS:-}" ]; then
  echo "Set LIBS to a directory containing Adafruit-GFX-Library, U8g2_for_Adafruit_GFX, and ArduinoJson" >&2
  exit 1
fi
L=$LIBS
FW=$ROOT/firmware/eink_dashboard
OUT=${OUT:-$HERE/out}; B=$OUT/build; mkdir -p "$B"
INC="-DARDUINO=10819 -I$HERE/shim -I$L/Adafruit-GFX-Library/src -I$L/U8g2_for_Adafruit_GFX/src -I$L/ArduinoJson/src -I$FW"
[ -f "$B/u8g2_fonts.o" ] || gcc -O1 -DU8G2_USE_LARGE_FONTS -c "$L/U8g2_for_Adafruit_GFX/src/u8g2_fonts.c" -I"$L/U8g2_for_Adafruit_GFX/src" -o "$B/u8g2_fonts.o"
[ -f "$B/gfx.o" ] || g++ -std=gnu++17 -O1 -w -c $INC "$L/Adafruit-GFX-Library/src/Adafruit_GFX.cpp" -o "$B/gfx.o"
[ -f "$B/u8g2gfx.o" ] || g++ -std=gnu++17 -O1 -w -c $INC "$L/U8g2_for_Adafruit_GFX/src/U8g2_for_Adafruit_GFX.cpp" -o "$B/u8g2gfx.o"
g++ -std=gnu++17 -O1 -Wall -Wextra -c -I"$L/ArduinoJson/src" -I"$FW" "$FW/model.cpp" -o "$B/model.o"
g++ -std=gnu++17 -O1 -Wall -Wextra -c $INC "$FW/render.cpp" -o "$B/render.o"
g++ -std=gnu++17 -O1 -w $INC "$HERE/preview.cpp" "$B/model.o" "$B/render.o" "$B/gfx.o" "$B/u8g2gfx.o" "$B/u8g2_fonts.o" -o "$B/preview"

cd "$TOOLS"
python3 make_fixture.py >/dev/null
node test_backend.js fixture.json 2026-10-27T00:44:00Z "$OUT/p_mon_evening.json" >/dev/null   # Mon 7:44 PM CDT
node test_backend.js fixture.json 2026-10-27T12:50:00Z "$OUT/p_tue_morning.json" >/dev/null   # Tue 7:50 AM
node test_backend.js fixture.json 2026-10-27T02:45:00Z "$OUT/p_mon_late.json" >/dev/null      # Mon 9:45 PM
# edge cases: Chinese + Spanish titles, a location, a long non-key all-day chip
python3 - "$OUT/p_tue_morning.json" "$OUT/p_edge.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
d = p["days"][0]
d["ev"].append({"s": 645, "e": 660, "t": "中秋节 picnic planning", "l": "Community center", "k": 1, "f": 0})
d["ev"].append({"s": 700, "e": 730, "t": "Llamar a la abuela (cumpleaños)", "l": "", "k": 1, "f": 0})
d["ev"].sort(key=lambda e: (e["s"], e["e"]))
d["ad"].append({"t": "Picnic planning week", "k": 1})
p["days"][2]["ev"].append({"s": 1080, "e": 1110, "t": "数据结构 quiz due", "l": "", "k": 2, "f": 0})
json.dump(p, open(sys.argv[2], "w"), ensure_ascii=False)
PY
"$B/preview" "$OUT/p_mon_evening.json" 2026-10-26 19:44 "$OUT/1_mon_evening.pgm" "Updated 7:44p"
"$B/preview" "$OUT/p_tue_morning.json" 2026-10-27 07:50 "$OUT/2_tue_morning.pgm" "Updated 7:50a"
"$B/preview" "$OUT/p_mon_late.json"    2026-10-26 21:45 "$OUT/3_mon_bedtime.pgm" "Updated 9:45p"
"$B/preview" "$OUT/p_edge.json"        2026-10-27 10:05 "$OUT/4_edge_cases.pgm" "Offline - last sync Tue 7:50a" stale
python3 -c "
import glob; from PIL import Image
for f in sorted(glob.glob('$OUT/*.pgm')): Image.open(f).save(f[:-4] + '.png')
"
ls "$OUT"/*.png
