#include "screens/btrailer.h"
#include "screens/player.h"
#include "screens/saver.h"
#include "screens/board.h"
#include "core/app.h"
#include "screens/cwmenu.h"
#include <stdlib.h>
#include "core/store.h"
#include "core/bgtrailer.h"
#include "core/trailer.h"
#include "core/video.h"
#include "core/pace.h"
#include "ui/gfx.h"
#include <stdio.h>

/* own = the decoder holds the trailer (the player takes it over the moment it opens) */
static struct { int own, live, held, resumes, byKey, quiet, endFade, seekWant; double pauseAt, resumeAt, steadyAt; } B;

/* the trailer in the expanded card (as Nuvio's expanded_card): the plane goes behind the card's rectangle, cover-cropped;
   the board makes the window see-through there. The page itself never gives way */
static struct { float x, y, w, h, px, py, pw, ph; double at; int on; Tween a; } CD;
void btr_card_rect(float x, float y, float w, float h) { CD.x = x; CD.y = y; CD.w = w; CD.h = h; CD.at = app_now(); }
float btr_card_alpha(void) { return CD.a.v; }
static int card_place(void) {
  int vw = video_width(), vh = video_height(); if (!vw || !vh || CD.w <= 0 || app_now() - CD.at > 0.25) return 0;
  float k = CD.w / vw > CD.h / vh ? CD.w / vw : CD.h / vh; int sw = (int)(CD.w / k), sh = (int)(CD.h / k), sx = (vw - sw) / 2, sy = (vh - sh) / 2;
  int crop = video_set_crop(sx, sy, sw, sh, CD.x, CD.y, CD.w, CD.h); if (!crop) video_set_window(CD.x, CD.y, CD.w, CD.h);
  CD.px = CD.x; CD.py = CD.y; CD.pw = CD.w; CD.ph = CD.h; fprintf(stderr, "[btr] card: %dx%d, %s -> %.0f,%.0f %.0fx%.0f\n", vw, vh, crop ? "crop" : "window", CD.x, CD.y, CD.w, CD.h); return 1;
}
static void h_load(const char *url, const char *mime, const char *titleId, void *ud) {
  (void)mime; (void)titleId; (void)ud; if (player_open()) return;
  fprintf(stderr, "[trailer] background %s\n", url);
  video_load(url); B.own = 1; B.live = 0; B.held = 0; B.resumes = 0; B.steadyAt = 0; B.seekWant = bgt_clip();
  B.quiet = 1; /* silent until it shows (set once the media is loaded) */
}
/* a key: the card shuts at once and the video plays on silent, until the decoder is let go in a pause of the remote.
   Ended by itself: it closes softly over the last frame */
static void h_hide(int now, void *ud) {
  (void)ud; CD.on = 0; if (B.byKey || now) tween_set(&CD.a, 0); else tween_to(&CD.a, 0, 0.4f, EASE_CSS_EASE);
  if (B.own && !player_open()) { if (now) { video_set_volume(0, 0); video_pause(); B.held = 1; } else { video_set_volume(0, 450); B.pauseAt = B.byKey ? 0 : app_now() + 0.47; } }
}
static void h_unload(void *ud) { (void)ud; if (B.own && !player_open()) video_unload(); B.own = 0; B.live = 0; }
void btr_init(void) { static BgtHooks h = { h_load, h_hide, h_unload, NULL }; bgt_init(&h); tween_set(&CD.a, 0); }
void btr_key(int arrow) { (void)arrow; if (CD.on) B.byKey = 1; bgt_key(); B.byKey = 0; }
void btr_release(void) { bgt_release_now(); tween_set(&CD.a, 0); }
void btr_tick(float dt) {
  trailer_tick();
  if (player_open() && (B.own || bgt_state() != BGT_OFF)) { B.own = 0; btr_release(); } /* the player has the decoder now */
  const Item *f = NULL;
  int cardScr = SCR == SCR_HOME || SCR == SCR_DISC || SCR == SCR_COL; /* where a card opens */
  if (cardScr && !topNav.active && screens[SCR] && screens[SCR]->focused) f = screens[SCR]->focused();
  BgtView v = { app_trailer_idle(), cardScr, f, topNav.active || app_overlay != NULL || /* (a sheet or the long-press menu: it gives way, softly, and none starts under it) */ player_open() || !app_splash_done || saver_on(),
                board_spot_up(), pace_period() > 21.5 };
  v.next = cardScr ? board_next_item() : NULL;
  bgt_tick(&v);
  if (B.own && !player_open()) {
    video_pump();
    VidState vs = video_state(); double now = app_now();
    if (B.quiet && video_has_media()) { B.quiet = 0; video_set_volume(0, 0); } /* (as soon as the media exists: silent until it shows) */
    /* the card opens onto it only once it has been playing steadily for a moment (the plane placed early): the TV says
       "playing" a few frames before the picture is there, which showed as black */
    if (B.seekWant && video_duration() > 0) { B.seekWant = 0; double to = bgt_clip_seek(video_duration()); fprintf(stderr, "[btr] clip: from %.0f s\n", to); video_seek(to); B.steadyAt = 0; }
    if (!B.live && !B.seekWant && !video_settling() && video_live() && vs == VID_PLAYING) { if (!B.steadyAt) { B.steadyAt = now; card_place(); } } else if (!B.live) B.steadyAt = 0;
    if (!B.live && B.steadyAt && now - B.steadyAt > 0.5) { B.steadyAt = 0;
      if (!video_partial_plane()) { fprintf(stderr, "[btr] this TV places the plane full-screen only: no trailer in a card\n"); B.live = 1; bgt_ended(); }
      else if (card_place()) { B.live = 1; /* (the card not open yet: asked again next frame, the video runs on behind the page) */
        if (bgt_state() == BGT_LOAD) { bgt_playing(); CD.on = 1; fprintf(stderr, "[btr] showing %dx%d\n", video_width(), video_height());
          B.endFade = 0; B.pauseAt = 0; if (!bgt_clip()) video_set_volume(atoi(store_get("bgTrailerVol", "70")), 1800); /* the sound comes up softly to the chosen level */
          tween_set(&CD.a, 0); tween_to(&CD.a, 1, 0.35f, EASE_CSS_EASE); } } else B.steadyAt = now; }
    if (CD.on && now - CD.at < 0.25 && (CD.x != CD.px || CD.y != CD.py || CD.w != CD.pw || CD.h != CD.ph)) card_place(); /* the card moved under it */
    if ((vs == VID_ENDED || vs == VID_ERROR) && (bgt_state() == BGT_LOAD || bgt_state() == BGT_SHOW)) bgt_ended();
    /* paused by something other than us (the pipeline settling, a focus change of the TV's own): it plays on, a few
       times at most, a second apart, so a pipeline that insists is not fought every frame */
    if (vs == VID_PAUSED && !B.held && (bgt_state() == BGT_LOAD || bgt_state() == BGT_SHOW) && B.resumes < 3 && now >= B.resumeAt) {
      B.resumes++; B.resumeAt = now + 1.0; fprintf(stderr, "[btr] paused without us: play again (%d)\n", B.resumes); video_play(); }
  }
  if (B.pauseAt && app_now() >= B.pauseAt) { B.pauseAt = 0; if (B.own && !player_open()) { video_pause(); B.held = 1; } } /* silent now: it stops */
  if (CD.on && !B.endFade) { double d = video_duration(), p = video_time(); if (d > 3 && p > d - 1.6) { B.endFade = 1; video_set_volume(0, 1300); } } /* its last moments: the sound fades out before it ends */
  tween_tick(&CD.a, dt);
}
