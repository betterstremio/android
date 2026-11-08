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
/* a clip: the title itself (Settings > For You > Preview on focus), started past the opening credits, silent, 30 s at
   most. Its link is looked for the moment a card is settled on (0.35 s), while the card opens, and kept per title, so
   the clip can start 1.5 s in; none that suits: the trailer instead.
   The pick is the clip's own (not the Play button's best-for-this-TV choice): links as each addon answers, a small SDR
   one (720p first, 1080p next, never 4K or HDR / Dolby Vision: the TV would switch its picture mode for a preview),
   done as soon as a 720p one is in or 0.7 s after the first answer. */
static int clip; static double clipFrac, clipAbs, showAt;
#define CLIP_SECONDS 30
#define CLIP_IDLE 1.5f      /* a clip starts this long after the remote rests (a trailer: 3 s) */
#define PREFETCH_IDLE 0.35f /* its link is looked for this long after */
#define CLIP_GRACE 0.7      /* after the first addon's answer, the others this long to bring a better one */
typedef struct { char id[64], url[SB_URL_MAX], addon[96], label[160]; int score, done; double firstAt, askedAt, frac, abs; unsigned g; } ClipPick;
static ClipPick CP; static unsigned pgen;
static struct { char id[64], url[SB_URL_MAX], addon[96], label[160]; int score; double at, frac, abs; } CC[8]; static int ccNext; /* found links, an hour each */
static const char *preview_mode(void) { const char *m = store_get("bgPreview", ""); if (m[0]) return m; return strcmp(store_get("bgTrailer", "true"), "false") ? "clip" : "off"; }
int bgt_clip(void) { return clip; }
double bgt_clip_seek(double dur) { if (dur <= 0) return 0; double t = clipAbs > 0 ? clipAbs : dur * clipFrac; if (t > dur * 0.5) t = dur * 0.3; return t; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

void bgt_init(const BgtHooks *h) { if (h) H = *h; }
BgtState bgt_state(void) { static const BgtState m[] = { BGT_OFF, BGT_WAIT, BGT_WAIT, BGT_LOAD, BGT_SHOW, BGT_GONE }; return m[st]; }

/* a soft stop: paused and hidden in the same frame; the decoder waits for a pause in the navigation (unloading
   the pipeline in the key handler made the next frame late). The link search goes on: it is for the card in focus */
static void stop_soft(void) {
  gen++; if (tok) { trailer_cancel(tok); tok = 0; }
  if (st == S_LOAD || st == S_SHOW) {
    int was = st == S_SHOW; if (H.hide) H.hide(!was, H.ud);
    st = S_GONE; hiddenAt = now_s() + (was ? 0.46 : 0); /* it was showing: the picture fades back over it first */
  } else if (st == S_META || st == S_FIND) st = S_OFF;
}
void bgt_release_now(void) {
  gen++; if (tok) { trailer_cancel(tok); tok = 0; }
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
static void pick_keep(void) { /* a finished search, kept for the next time this card is settled on */
  int k = ccNext++ % 8; snprintf(CC[k].id, sizeof CC[k].id, "%s", CP.id); snprintf(CC[k].url, sizeof CC[k].url, "%s", CP.url);
  snprintf(CC[k].addon, sizeof CC[k].addon, "%s", CP.addon); snprintf(CC[k].label, sizeof CC[k].label, "%s", CP.label);
  CC[k].score = CP.score; CC[k].at = now_s(); CC[k].frac = CP.frac; CC[k].abs = CP.abs;
}
static int pick_ready(double t) { return CP.done || CP.score >= 100 || (CP.firstAt && CP.score > 0 && t - CP.firstAt > CLIP_GRACE) || t - CP.askedAt > 6; }
static void on_clip_part(StreamGroup *g, int gi, void *ud) {
  (void)gi; if ((unsigned)(uintptr_t)ud != CP.g || CP.done || !g) return;
  if (!CP.firstAt) CP.firstAt = now_s();
  for (int i = 0; i < g->n; i++) { int sc = clip_score(&g->items[i]); if (sc > CP.score) { const Stream *s = &g->items[i]; CP.score = sc;
      snprintf(CP.url, sizeof CP.url, "%s", s->url); snprintf(CP.addon, sizeof CP.addon, "%s", s->addon); snprintf(CP.label, sizeof CP.label, "%.150s", s->name); } }
  if (CP.score >= 100) { CP.done = 1; pick_keep(); } /* a 720p one: no need to wait for the others */
}
static void on_clip_done(int total, void *ud) { (void)total; if ((unsigned)(uintptr_t)ud != CP.g || CP.done) return; CP.done = 1; if (CP.score > 0) pick_keep(); }
/* the search for a card's link: from the kept ones, else the addons asked now */
static void clip_search(const Item *it) {
  if (!strcmp(CP.id, it->id)) return;
  memset(&CP, 0, sizeof CP); snprintf(CP.id, sizeof CP.id, "%s", it->id); CP.askedAt = now_s();
  int series = !strcmp(it->type, "series"); CP.abs = series ? 240 : 0; CP.frac = series ? 0 : 0.15;
  for (int k = 0; k < 8; k++) if (!strcmp(CC[k].id, it->id) && CP.askedAt - CC[k].at < 3600) {
    snprintf(CP.url, sizeof CP.url, "%s", CC[k].url); snprintf(CP.addon, sizeof CP.addon, "%s", CC[k].addon); snprintf(CP.label, sizeof CP.label, "%s", CC[k].label);
    CP.score = CC[k].score; CP.done = 1; return; }
  char vid[96]; CwInfo ci;
  if (series) { if (lib_cw_info(it, &ci) && ci.vid[0] && strcmp(ci.vid, it->id)) snprintf(vid, sizeof vid, "%s", ci.vid); else snprintf(vid, sizeof vid, "%s:1:1", it->id); }
  else snprintf(vid, sizeof vid, "%s", it->id);
  CP.g = ++pgen; if (!sb_streams(it->type, vid, on_clip_part, on_clip_done, (void *)(uintptr_t)CP.g) && !CP.done) CP.done = 1; /* (no addon to ask) */
}
/* plays what the search has, or the trailer when it has nothing */
static void clip_go(double t) {
  if (!idleOn) { st = S_OFF; return; }
  clipAbs = CP.abs; clipFrac = CP.frac;
  if (CP.score <= 0) { fprintf(stderr, "[btr] clip: no small SDR link for %s (%.1f s), the trailer instead\n", tid, t - CP.askedAt); clip = 0; st = S_META; sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)gen); return; }
  st = S_LOAD; memAt = t + 10; fprintf(stderr, "[btr] clip %s, link found in %.1f s from %s: %s, %d MB free\n", tid, (CP.firstAt ? CP.firstAt : t) - CP.askedAt, CP.addon, CP.label, plat_mem_free_mb());
  if (H.load) H.load(CP.url, "", tid, H.ud);
}
static int clip_for(const Item *it) { return it && !strcmp(preview_mode(), "clip") && !strncmp(it->id, "tt", 2) && (!strcmp(it->type, "movie") || !strcmp(it->type, "series")); }
static void try_start(const BgtView *v) {
  if (v->shedding) return;   /* the TV is struggling: no trailer for now */
  if (v->spotlight) return;  /* the spotlight moves on by itself */
  if (st != S_OFF || v->blocked || !v->focused || !v->focused->id[0] || !v->focused->type[0]) return;
  const char *mode = preview_mode(); if (!strcmp(mode, "off") || getenv("SB_NOBTR")) return; /* (SB_NOBTR: development, measuring without it) */
  snprintf(tid, sizeof tid, "%s", v->focused->id); snprintf(ttype, sizeof ttype, "%s", v->focused->type);
  if (clip_for(v->focused)) { clip_search(v->focused); clip = 1; st = S_FIND; gen++; return; } /* (the search may be done already: the tick plays it) */
  clip = 0;
  st = S_META; sb_meta(ttype, tid, on_meta, (void *)(uintptr_t)++gen);
}
void bgt_tick(const BgtView *v) {
  double t = now_s();
  if (st == S_GONE && t >= hiddenAt && (v->idle >= 0.7f || t - hiddenAt >= 4)) { st = S_OFF; if (H.unload) H.unload(H.ud); }
  int can = v->browsing && v->focused && !v->blocked && !v->spotlight && !v->shedding && !getenv("SB_NOBTR"), isClip = can && clip_for(v->focused);
  if (isClip && v->idle > PREFETCH_IDLE) clip_search(v->focused); /* the link, looked for while the card opens */
  if (clip && st == S_FIND && !strcmp(CP.id, tid) && pick_ready(t)) clip_go(t);
  if (clip && st == S_SHOW && showAt && t - showAt > CLIP_SECONDS) { fprintf(stderr, "[btr] clip: %d s shown, the card goes back\n", CLIP_SECONDS); stop_soft(); }
  if (isClip && v->idle > CLIP_IDLE && !idleOn) { idleOn = 1; try_start(v); return; } /* a clip: sooner than a trailer, checked every frame */
  if (t - lastCheck < 1) return;
  lastCheck = t;
  int idle = v->idle > 3 && v->browsing && v->focused && !v->blocked;
  if (idle && !idleOn) { idleOn = 1; try_start(v); }
  else if (!idle && idleOn && !(isClip && v->idle > CLIP_IDLE)) { if (st == S_LOAD || st == S_SHOW) fprintf(stderr, "[btr] stop: idle %.1f s, board %d, focused %d, blocked %d\n", v->idle, v->browsing, v->focused != NULL, v->blocked); idleOn = 0; stop_soft(); }
}
