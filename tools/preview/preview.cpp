// PC preview: renders the exact firmware drawing code into an 800x480 1-bit canvas.
// usage: preview payload.json YYYY-MM-DD HH:MM out.pgm ["status"] [stale]
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "model.h"
#include "render.h"
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
  std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf(); std::string json = ss.str();
  static Dashboard d;
  if (!parseDashboard(json.c_str(), json.size(), d)) { fprintf(stderr, "parse failed: %s\n", d.error); return 1; }
  int y, mo, da, hh, mm;
  sscanf(argv[2], "%d-%d-%d", &y, &mo, &da); sscanf(argv[3], "%d:%d", &hh, &mm);
  NowInfo now{daysFromCivil(y, mo, da), hh * 60 + mm, hh};
  RenderOptions opt;
  if (argc > 5) snprintf(opt.status, sizeof(opt.status), "%s", argv[5]);
  opt.stale = argc > 6;
  GFXcanvas1 canvas(800, 480);
  U8G2_FOR_ADAFRUIT_GFX u8;
  u8.begin(canvas);
  renderDashboard(canvas, u8, d, now, opt);
  FILE* o = fopen(argv[4], "wb");
  fprintf(o, "P5\n800 480\n255\n");
  for (int yy = 0; yy < 480; yy++) for (int xx = 0; xx < 800; xx++) fputc(canvas.getPixel(xx, yy) ? 255 : 0, o);
  fclose(o);
  printf("rendered %s (sizeof Dashboard = %zu)\n", argv[4], sizeof(Dashboard));
  return 0;
}
