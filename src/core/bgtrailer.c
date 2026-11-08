#include "core/bgtrailer.h"
#include "core/trailer.h"
#include "core/store.h"
#include "core/best.h"
#include "core/stremio.h"
#include "platform/mem.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

enum { S_OFF, S_META, S_FIND, S_LOAD, S_SHOW, S_GONE };
static BgtHooks H; static int st, idleOn; static unsigned gen, tok;
static double lastCheck, hiddenAt, memAt; static char tid[64], ttype[16];
/* a clip: the title itself (Settings > For You > Preview on focus), from the one-click chooser, started past the
   opening credits, silent, 30 s at most; none that starts at once: the trailer instead */
static int clip; static unsigned btok; static double clipFrac, clipAbs, showAt;
/* the clip's own quick pick (not the Play button's best-for-this-TV choice): the first addons' links as they come, a
   small SDR one (720p first, 1080p next, never 4K or HDR / Dolby Vision: the TV would switch its picture mode for a
   preview), started as soon as a 720p one is in or 1.5 s after the first answer */
static struct { char url[SB_URL_MAX], addon[96], label[160]; int score; double firstAt, askedAt; } CP;
static void clip_go(void);
#define CLIP_SECONDS 30
static const char *preview_mode(void) { const char *m = store_get("bgPreview", ""); if (m[0]) return m; return strcmp(store_get("bgTrailer", "true"), "false") ? "clip" : "off"; }
int bgt_clip(void) { return clip; }
double bgt_clip_seek(double dur) { if (dur <= 0) return 0; double t = clipAbs > 0 ? clipAbs : dur * clipFrac; if (t > dur * 0.5) t = dur * 0.3; return t; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

void bgt_init(const BgtHooks *h) { if (h) H = *h; }
BgtState bgt_state(void) { static const BgtState m[] = { BGT_OFF, BGT_WAIT, BGT_WAIT, BGT_LOAD, BGT_SHOW, BGT_GONE }; return m[st]; }

/* a soft stop: paused and hidden in the same frame; the decoder waits for a pause in the navigation (unloading
   the pipeline in the key handler made the next frame late) */
static void stop_soft(void) {
  gen++; if (tok) { trailer_cancel(tok); tok = 0; } btok = 0;
  if (st == S_LOAD || st == S_SHOW) {
    int was = st == S_SHOW; if (H.hide) H.hide(!was, H.ud);
    st = S_GONE; hiddenAt = now_s() + (was ? 0.46 : 0); /* it was showing: the picture fades back over it first */
  } else if (st == S_META || st == S_FIND) st = S_OFF;
}
void bgt_release_now(void) {
  gen++; if (tok) { trailer_cancel(tok); tok = 0; } btok = 0;
  if (st == S_LOAD || st == S_SHOW) { if (H.hide) H.hide(1, H.ud); }
  if (st == S_LOAD || st == S_SHOW || st == S_GONE) { if (H.unload) H.unload(H.ud); }
  st = S_OFF;
}
void bgt_key(void) { if (idleOn || st != S_OFF) { if (st == S_LOAD || st == S_SHOW) fprintf(stderr, "[btr] stop: input\n"); idleOn = 0; stop_soft(); } }
void bgt_playing(void) { if (st == S_LOAD) { st = S_SHOW; showAt = now_s(); } }
void bgt_ended(void) { if (st == S_LOAD || st == S_SHOW) { fprintf(stderr, "[btr] stop: the video ended or failed\n"); stop_soft(); } }

static void on_found(const Trailer *t, const char *err, void *ud) {
  if ((unsigned)(uintptr_t)ud != gen || st != S_FIND) return;
  tok = 0;
  if (!t || !idleOn) { st = S_OFF; return; }
  /* the setting decides, not the free memory (the TV makes room for its own player); the free memory is only logged */
  st = S_LOAD; memAt = now_s() + 10; fprintf(stderr, "[btr] load %s (%s), %d MB free\n", tid, t->mime, plat_mem_free_mb());
  if (H.load) H.load(t->url, t->mime, tid, H.ud);
}
static void on_meta(cJSON *m, void *ud) {
  if ((unsigned)(uintptr_t)ud != gen || st != S_META) return;
  char imdb[24], yt[128]; trailer_imdb_id(m, tid, imdb, sizeof imdb); trailer_yt_ids(m, yt, sizeof yt);
  if (!m || !idleOn || (!imdb[0] && !yt[0])) { st = S_OFF; return; }
  st = S_FIND; tok = trailer_find_meta(m, tid, 1, on_found, (void *)(uintptr_t)gen);
}
static void on_meta(cJSON *m, void *ud);
static void clip_status(const char *t, void *ud) { (void)t; (void)ud; }
static void on_best(const Stream *s, const BestRate *r, void *ud) {
  (void)r; if ((unsigned)(uintptr_t)ud != gen || st != S_FIND) return; btok = 0;
  if (!idleOn) { st = S_OFF; return; }
  if (!s || s->p2p || strncmp(s->url, "http", 4) || s->hdrs[0]) { /* nothing that starts at once (P2P, a proxied link): the trailer */
    fprintf(stderr, "[btr] clip: no instant link for %s, the trailer instead\n", tid); clip = 0; st = S_META; sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)gen); return; }
  st = S_LOAD; memAt = now_s() + 10; fprintf(stderr, "[btr] clip %s from %s, %d MB free\n", tid, s->addon, plat_mem_free_mb());
  if (H.load) H.load(s->url, "", tid, H.ud);
}
/* a link's worth for a preview: -1 unusable; else higher is better (720p SDR, cached, small) */
static int clip_score(const Stream *s) {
  if (s->p2p || strncmp(s->url, "http", 4) || s->hdrs[0] || s->ext[0]) return -1;
  char lb[1100]; snprintf(lb, sizeof lb, "%s %s %s", s->name, s->title, s->filename);
  char hdr[64]; best_hdr_of(lb, hdr, sizeof hdr); if (hdr[0]) return -1;            /* HDR / Dolby Vision: the TV changes mode */
  const char *r = best_res_of(lb); int sc;
  if (!strcmp(r, "4K") || !strcmp(r, "1440p")) return -1;
  sc = !strcmp(r, "720p") ? 100 : !strcmp(r, "1080p") ? 80 : !strcmp(r, "SD") ? 55 : 50;
  if (strstr(lb, "\xe2\x8f\xb3") || strstr(lb, "ncached") || strstr(lb, "ownload")) return -1;   /* hourglass, "Uncached", "Download": not ready */
  if (strstr(lb, "\xe2\x9a\xa1") || strstr(lb, "ached")) sc += 15;                          /* lightning, "Cached" */
  if (s->size > 0) { int gb = (int)(s->size / 1e9); sc -= gb > 20 ? 20 : gb; }
  return sc;
}
static void on_clip_part(StreamGroup *g, int gi, void *ud) {
  (void)gi; if ((unsigned)(uintptr_t)ud != gen || st != S_FIND || !clip || !g) return;
  if (!CP.firstAt) CP.firstAt = now_s();
  for (int i = 0; i < g->n; i++) { int sc = clip_score(&g->items[i]); if (sc > CP.score) { const Stream *s = &g->items[i]; CP.score = sc;
      snprintf(CP.url, sizeof CP.url, "%s", s->url); snprintf(CP.addon, sizeof CP.addon, "%s", s->addon); snprintf(CP.label, sizeof CP.label, "%.150s", s->name); } }
  if (CP.score >= 100) clip_go(); /* a 720p one: no need to wait for the others */
}
static void on_clip_done(int total, void *ud) { (void)total; if ((unsigned)(uintptr_t)ud != gen || st != S_FIND || !clip) return; clip_go(); }
/* plays what the quick pick has, or the trailer when it has nothing */
static void clip_go(void) {
  btok = 0; if (!idleOn) { st = S_OFF; return; }
  if (CP.score <= 0) { fprintf(stderr, "[btr] clip: no small SDR link for %s (%.1f s), the trailer instead\n", tid, now_s() - CP.askedAt); clip = 0; st = S_META; sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)gen); return; }
  st = S_LOAD; memAt = now_s() + 10; fprintf(stderr, "[btr] clip %s in %.1f s from %s: %s, %d MB free\n", tid, now_s() - CP.askedAt, CP.addon, CP.label, plat_mem_free_mb());
  if (H.load) H.load(CP.url, "", tid, H.ud);
}
/* the title itself: a film from 15% in, a series' next episode (S1E1 when not started) 4 minutes in */
static void clip_start(const Item *it) {
  char vid[96]; CwInfo ci; int series = !strcmp(it->type, "series");
  if (series) { if (lib_cw_info(it, &ci) && ci.vid[0] && strcmp(ci.vid, it->id)) snprintf(vid, sizeof vid, "%s", ci.vid); else snprintf(vid, sizeof vid, "%s:1:1", it->id); clipAbs = 240; clipFrac = 0; }
  else { snprintf(vid, sizeof vid, "%s", it->id); clipAbs = 0; clipFrac = 0.15; }
  (void)clip_status; (void)on_best;
  memset(&CP, 0, sizeof CP); CP.askedAt = now_s();
  clip = 1; st = S_FIND; btok = sb_streams(ttype, vid, on_clip_part, on_clip_done, (void *)(uintptr_t)++gen);
  if (!btok && st == S_FIND && clip) clip_go(); /* (no addon to ask: it may have answered already) */
}
static void try_start(const BgtView *v) {
  if (v->shedding) return;   /* the TV is struggling: no trailer for now */
  if (v->spotlight) return;  /* the spotlight moves on by itself */
  if (st != S_OFF || v->blocked || !v->focused || !v->focused->id[0] || !v->focused->type[0]) return;
  const char *mode = preview_mode(); if (!strcmp(mode, "off") || getenv("SB_NOBTR")) return; /* (SB_NOBTR: development, measuring without it) */
  snprintf(tid, sizeof tid, "%s", v->focused->id); snprintf(ttype, sizeof ttype, "%s", v->focused->type);
  if (!strcmp(mode, "clip") && !strncmp(tid, "tt", 2) && (!strcmp(ttype, "movie") || !strcmp(ttype, "series"))) { clip_start(v->focused); return; }
  clip = 0;
  st = S_META; sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)++gen);
}
void bgt_tick(const BgtView *v) {
  double t = now_s();
  if (st == S_GONE && t >= hiddenAt && (v->idle >= 0.7f || t - hiddenAt >= 4)) { st = S_OFF; if (H.unload) H.unload(H.ud); }
  if (clip && st == S_FIND && ((CP.firstAt && CP.score > 0 && t - CP.firstAt > 1.5) || t - CP.askedAt > 6)) clip_go();
  if (clip && st == S_SHOW && showAt && t - showAt > CLIP_SECONDS) { fprintf(stderr, "[btr] clip: %d s shown, the card goes back\n", CLIP_SECONDS); stop_soft(); }
  if (t - lastCheck < 1) return;
  lastCheck = t;
  int idle = v->idle > 3 && v->browsing && v->focused && !v->blocked;
  if (idle && !idleOn) { idleOn = 1; try_start(v); }
  else if (!idle && idleOn) { if (st == S_LOAD || st == S_SHOW) fprintf(stderr, "[btr] stop: idle %.1f s, board %d, focused %d, blocked %d\n", v->idle, v->browsing, v->focused != NULL, v->blocked); idleOn = 0; stop_soft(); }
}
