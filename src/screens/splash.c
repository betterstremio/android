#include "screens/splash.h"
#include "screens/board.h"
#include "core/app.h"
#include "ui/gfx.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/text.h"
#include "ui/orbs.h"
#include "platform/plat.h"
#include "screens/login.h"
#include "screens/welcome.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* The start screen, quiet: on the page's own dark, the app's two glows drifting out from the middle to their corners
   as it fades in, and the mark: two discs meeting in the middle as one circle, then the play, then
   ORIGIN as its letters close up, still until Home is complete; leaving, it grows a touch and fades. Drawn, no
   picture to load. */
static int on, waitHome, gone; static float outS; static Orbs orbs; static double t0, cap, signedAt; static Timer1 out; 
#define MIN_UP 0.9  /* the entrance has room to finish */
/* ready = the first two rows' visible posters are in, the hero text is set and the backdrop is up:
   Home is revealed complete, never with empty cards; 2.5 s at most (a slow network), 9 s right after signing in (nothing cached yet) */
void splash_start(int waitForHome, double signedInAt) {
  int justSignedIn = signedInAt > 0; signedAt = signedInAt;
  on = 1; gone = 0; waitHome = waitForHome; memset(&orbs, 0, sizeof orbs); orbs_place(&orbs, 0, 2.2f); /* (drawn in close, then let go) */ t0 = justSignedIn ? app_now() - 2 : app_now(); cap = justSignedIn ? 9.0 : 2.5; outS = 1; /* the restart after signing in: already up, no second entrance */
  }
static void splash_end(void) {
  if (gone) return; gone = 1; timer_start(&out, 0.36f); app_splash_done = 1;
  fprintf(stderr, "[start] ready in %.0f ms\n", (app_now() - t0) * 1000);
  if (waitHome) app_reveal_start();
  if (signedAt > 0) welcome_open_now(); /* right after signing in: the guide comes straight from the loading (Home revealed under it) */
}
int splash_up(void) { return on; }
int splash_signed_in(void) { return on && signedAt > 0; }
void splash_tick(float dt) {
  if (!on) return;
  if (gone) { if (timer_tick(&out, dt) >= 1 && !out.on) on = 0; return; }
  double el = app_now() - t0;
  if (el < MIN_UP || (waitHome && el < cap && (!board_home_ready() || (signedAt > 0 && !welcome_ready())))) return; /* (signed in: the guide's pictures in too) */
  splash_end();
}
/* the mark, drawn (no tile): the circle in its violet, the rounded play in white, ORIGIN spaced out under it */
void splash_draw(void) {
  if (!on) return;
  float lv = gone ? ease_apply(EASE_CSS_EASE, timer_x(&out)) : 0, a = 1 - lv; gfx_opacity_push(a);
  float t = (float)(app_now() - t0), cx = SB_W / 2, cy = SB_H / 2 - 40, R = 118;
  gfx_rect(0, 0, SB_W, SB_H, PAGE);
  if (signedAt > 0) { login_backdrop_draw(); login_loading_draw(signedAt, 1); gfx_opacity_pop(); return; } /* right after signing in: the same loading carries on, no mark */
  { float g = ease_apply(EASE_IN_OUT, t / 1.1f), sp = 1 + 0.9f * (1 - ease_apply(EASE_OUT, t / 1.6f)); /* the glows: from the middle out round their corners as the screen comes, then drifting round them */
    orbs_step(&orbs, app_dt() * 2.2f, 0, sp, 2.2f); orbs_draw_col(&orbs, g, 0xb08cff, 0x7a4cf0); } /* (always the mark's purple, whatever the accent; their drift twice as quick here, the screen is short) */
  float in = ease_apply(EASE_OUT, t / 0.18f), m = ease_apply(EASE_IN_OUT, t / 0.55f), pop = ease_apply(EASE_OUT, (t - 0.5f) / 0.3f), kp = ease_apply(EASE_OUT, (t - 0.42f) / 0.4f), kt = ease_apply(EASE_OUT, (t - 0.5f) / 0.45f);
  float s = 0.96f + 0.04f * pop;
  if (gone) { s = outS + (1.05f - outS) * lv; } else outS = s;
  gfx_scale_push(s, s, cx, cy);
  gfx_opacity_push(in); /* two discs, one from each side, become the one circle (no logo, nothing to load; all inside MIN_UP) */
  { float r = R * (0.58f + 0.42f * m), off = 250 * (1 - m);
    #define MIX(a, b) ((int)((a) + ((b) - (a)) * m + 0.5f))
    for (int i = m < 1 ? 0 : 1; i < 2; i++) { float x = (i ? cx + off : cx - off) - r, y = cy - r;
      if (i) gfx_rrect_v(x, y, 2 * r, 2 * r, r, RGB(MIX(0x38, 0x6a), MIX(0xb6, 0x45), MIX(0xf0, 0xe0)), RGB(MIX(0x1c, 0x2f), MIX(0x6e, 0x1a), MIX(0xc8, 0x9e)));
      else gfx_rrect_v(x, y, 2 * r, 2 * r, r, RGB(MIX(0xa8, 0x6a), MIX(0x5a, 0x45), MIX(0xff, 0xe0)), RGB(MIX(0x62, 0x2f), MIX(0x24, 0x1a), MIX(0xc0, 0x9e)));
      gfx_rrect_v(x, y, 2 * r, 2 * r, r, WHITE_A(26), WHITE_A(0)); } /* (its light from the top, inside the circle) */
    #undef MIX
  }
  gfx_opacity_pop();
  { float S = 2 * R * 512 / 264.0f; gfx_opacity_push(kp); icon_draw("o_play", cx - S * 256 / 512 + 6 * (1 - kp), cy - S * 226 / 512, S, WHITE_A(255)); gfx_opacity_pop(); }
  gfx_scale_pop();
  gfx_opacity_push(kt); text_draw("ORIGIN", cx + 10, cy + R + 56 + 8 * (1 - kt), 40, 700, WHITE_A(240), 20 - 6 * kt, 0, ALIGN_CENTER); gfx_opacity_pop();
  gfx_opacity_pop();
}
