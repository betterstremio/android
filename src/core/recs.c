/* "For You" (see recs.h): the watch history -> seeds -> TMDB lists -> recs_core ranking -> Home's lists. */
#include "core/recs.h"
#include "core/recs_core.h"
#include "core/app.h"
#include "core/i18n.h"
#include "core/net.h"
#include "core/store.h"
#include "core/tmdb.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMG_P "https://image.tmdb.org/t/p/w342"
#define IMG_B "https://image.tmdb.org/t/p/w1280"
#define LIST_AGE (24 * 3600)
#define TOP_AGE (12 * 3600)
#define RESULT_AGE (12 * 3600)
#define MAX_JOBS 160
#define INFLIGHT 3

/* ---- small JSON helpers ---- */
static const char *rjs(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) && v->valuestring ? v->valuestring : ""; }
static double rjn(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }
static long today_days(void) { return (long)(time(NULL) / 86400); }

/* ---- settings ---- */
static const struct { const char *cc, *name, *langs; } REGIONS[] = {
  { "IN", "India", "hi,en" }, { "US", "the US", "en" }, { "GB", "the UK", "en" }, { "IE", "Ireland", "en" }, { "CA", "Canada", "en,fr" },
  { "AU", "Australia", "en" }, { "DE", "Germany", "de,en" }, { "FR", "France", "fr,en" }, { "ES", "Spain", "es,en" }, { "IT", "Italy", "it,en" },
  { "BR", "Brazil", "pt,en" }, { "MX", "Mexico", "es,en" }, { "JP", "Japan", "ja,en" }, { "KR", "South Korea", "ko,en" }, { "PK", "Pakistan", "ur,hi,en" },
  { "BD", "Bangladesh", "bn,hi,en" }, { "AE", "the UAE", "ar,hi,en" }, { "NL", "the Netherlands", "nl,en" }, { "TR", "Turkey", "tr,en" }, { "PL", "Poland", "pl,en" } };
#define NREG ((int)(sizeof REGIONS / sizeof REGIONS[0]))
const char *recs_region_name(const char *cc) { for (int i = 0; i < NREG; i++) if (!strcmp(REGIONS[i].cc, cc)) return REGIONS[i].name; return cc; }
int  recs_on(void) { return strcmp(store_get("recsOn", "true"), "false") != 0; }
void recs_set_on(int on) { store_set("recsOn", on ? "true" : "false"); recs_refresh(); }
int  recs_paused(void) { return !strcmp(store_get("recsPaused", "false"), "true"); }
void recs_set_paused(int on) { store_set("recsPaused", on ? "true" : "false"); }
int  recs_hide_cinema(void) { return !strcmp(store_get("recsHideCinema", "false"), "true"); }
void recs_set_hide_cinema(int on) { store_set("recsHideCinema", on ? "true" : "false"); recs_refresh(); }
const char *recs_region(void) {
  const char *r = store_get("recsRegion", ""); if (r[0]) return r;
  static char cc[4]; if (cc[0]) return cc;
  const char *loc = plat_ui_locale(); const char *d = loc ? strpbrk(loc, "-_") : NULL; /* "en-IN" */
  if (d && strlen(d + 1) >= 2) { cc[0] = (char)(d[1] & ~0x20); cc[1] = (char)(d[2] & ~0x20); cc[2] = 0; } else snprintf(cc, sizeof cc, "US");
  return cc;
}
void recs_set_region(const char *cc) { store_set("recsRegion", cc ? cc : ""); store_set("recsLangs", ""); recs_refresh(); }
const char *recs_langs(void) {
  const char *l = store_get("recsLangs", ""); if (l[0]) return l;
  const char *cc = recs_region(); for (int i = 0; i < NREG; i++) if (!strcmp(REGIONS[i].cc, cc)) return REGIONS[i].langs;
  return "en";
}
void recs_set_langs(const char *csv) { store_set("recsLangs", csv ? csv : ""); recs_refresh(); }

/* ---- the lists Home shows ---- */
typedef struct { Item *it; int n; int rank[16]; } Out;
static Out bill, byw, topM, topS; static char bywTitle[160], topMTitle[160], topSTitle[160];
static unsigned ver; static int loaded, haveResult;
unsigned recs_version(void) { return ver; }
static int copy_out(const Out *o, ItemList *l) {
  memset(l, 0, sizeof *l); if (!o->n) return 0;
  l->items = malloc(sizeof(Item) * (size_t)o->n); if (!l->items) return 0;
  memcpy(l->items, o->it, sizeof(Item) * (size_t)o->n); l->n = l->total = o->n;
  for (int i = 0; i < l->n; i++) l->items[i].epName[0] = 0; /* (the date stays here: an episode name elsewhere) */
  return o->n;
}
int recs_billboard(ItemList *l) { return copy_out(&bill, l); }
int recs_because(ItemList *l, char *t, size_t cap) { snprintf(t, cap, "%s", bywTitle); return copy_out(&byw, l); }
int recs_top10(int series, ItemList *l, char *t, size_t cap) { snprintf(t, cap, "%s", series ? topSTitle : topMTitle); return copy_out(series ? &topS : &topM, l); }
int recs_rank_of(const Item *it) {
  for (int k = 0; k < 2; k++) { const Out *o = k ? &topS : &topM; for (int i = 0; i < o->n && i < 10; i++) if (!strcmp(o->it[i].id, it->id) && !strcmp(o->it[i].type, it->type)) return i + 1; }
  return 0;
}
static int in_out(const Out *o, const Item *it) { for (int i = 0; i < o->n; i++) if (!strcmp(o->it[i].id, it->id)) return i; return -1; }
int recs_badges(const Item *it) {
  int b = 0, i;
  if ((i = in_out(&bill, it)) >= 0 || (i = in_out(&byw, it)) >= 0) b |= RECS_LOVE;
  /* "Recently added": out in the last 60 days (the year alone says too little; the result keeps the date in metaLine's place) */
  for (int k = 0; k < 4; k++) { const Out *o = k == 0 ? &bill : k == 1 ? &byw : k == 2 ? &topM : &topS; int j = in_out(o, it); if (j < 0) continue;
    long d = recs_days(o->it[j].epName); if (d && today_days() - d >= 0 && today_days() - d <= 60) b |= RECS_NEW; break; }
  return b;
}

/* the finished lists, as kept on disk: {"bill":[...],"byw":{"title":..,"items":[...]},"topM":..,"topS":..} */
static cJSON *item_json(const Item *it) {
  cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "id", it->id); cJSON_AddStringToObject(o, "type", it->type); cJSON_AddStringToObject(o, "name", it->name);
  cJSON_AddStringToObject(o, "poster", it->poster); cJSON_AddStringToObject(o, "background", it->background); cJSON_AddStringToObject(o, "description", it->description);
  cJSON_AddStringToObject(o, "date", it->epName); return o;
}
static void item_read(Item *it, const cJSON *o) {
  memset(it, 0, sizeof *it);
  snprintf(it->id, sizeof it->id, "%s", rjs(o, "id")); snprintf(it->type, sizeof it->type, "%s", rjs(o, "type")); snprintf(it->name, sizeof it->name, "%s", rjs(o, "name"));
  snprintf(it->poster, sizeof it->poster, "%s", rjs(o, "poster")); snprintf(it->background, sizeof it->background, "%s", rjs(o, "background"));
  snprintf(it->description, sizeof it->description, "%s", rjs(o, "description")); snprintf(it->epName, sizeof it->epName, "%s", rjs(o, "date")); /* (epName: unused on a catalog card, it carries the date) */
  if (strlen(rjs(o, "date")) >= 4) snprintf(it->metaLine, sizeof it->metaLine, "%.4s", rjs(o, "date"));
}
static cJSON *out_json(const Out *o, const char *title) {
  cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "title", title ? title : ""); cJSON *a = cJSON_AddArrayToObject(j, "items");
  for (int i = 0; i < o->n; i++) cJSON_AddItemToArray(a, item_json(&o->it[i])); return j;
}
static void out_read(Out *o, const cJSON *j, char *title, size_t cap) {
  free(o->it); memset(o, 0, sizeof *o); if (title) snprintf(title, cap, "%s", rjs(j, "title"));
  const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "items"), *e; int n = cJSON_GetArraySize(a); if (n <= 0) return;
  o->it = calloc((size_t)n, sizeof(Item)); if (!o->it) return;
  cJSON_ArrayForEach(e, a) item_read(&o->it[o->n++], e);
}
static void result_save(void) {
  cJSON *j = cJSON_CreateObject(); cJSON_AddItemToObject(j, "bill", out_json(&bill, ""));
  cJSON_AddItemToObject(j, "byw", out_json(&byw, bywTitle)); cJSON_AddItemToObject(j, "topM", out_json(&topM, topMTitle)); cJSON_AddItemToObject(j, "topS", out_json(&topS, topSTitle));
  cJSON_AddStringToObject(j, "region", recs_region()); cJSON_AddStringToObject(j, "langs", recs_langs());
  char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); if (s) { cache_write("recs:result", (unsigned char *)s, strlen(s)); free(s); }
}
static int result_load(long maxAge) {
  size_t len = 0; unsigned char *b = cache_read("recs:result", maxAge, &len); if (!b) return 0;
  cJSON *j = cJSON_ParseWithLength((const char *)b, len); free(b); if (!j) return 0;
  int same = !strcmp(rjs(j, "region"), recs_region()) && !strcmp(rjs(j, "langs"), recs_langs());
  if (same) {
    out_read(&bill, cJSON_GetObjectItemCaseSensitive(j, "bill"), NULL, 0); out_read(&byw, cJSON_GetObjectItemCaseSensitive(j, "byw"), bywTitle, sizeof bywTitle);
    out_read(&topM, cJSON_GetObjectItemCaseSensitive(j, "topM"), topMTitle, sizeof topMTitle); out_read(&topS, cJSON_GetObjectItemCaseSensitive(j, "topS"), topSTitle, sizeof topSTitle);
    ver++; }
  cJSON_Delete(j); return same;
}

/* ---- the run: seeds, then their lists and the Top 10 lists, then ranking, then IMDb ids for what is shown ---- */
enum { J_FIND, J_REC, J_DISC, J_TOP, J_EXT };
typedef struct { int kind, seed, lang, series; int tmdb; char tk; char path[600], cache[96]; long maxAge; int state; /* 0 queued, 1 asked, 2 done */ } Job;
typedef struct { char imdb[24]; char tk; int tmdb; char title[128]; int genres[8], ng; char lang[4]; double w, age; RecCand rec[RECS_LIST], disc[RECS_LIST]; int nr, nd; } Seed;
static Job jobs[MAX_JOBS]; static int njobs, phase; /* 0 idle, 1 lists, 2 ids */ static unsigned runId;
static Seed seeds[RECS_MAX_SEEDS]; static int nseeds;
static RecCand topRaw[2][8][RECS_LIST]; static int topN[2][8]; static char langs[8][4]; static int nlangs;
static RecCand pick[4][32]; static int npick[4]; static int pickSeed; /* billboard, byw, topM, topS before their IMDb ids */
static double startAt, libAt; static time_t doneAt; static unsigned libSeen; static int requests;

static cJSON *idmap(void) { cJSON *m = store_node("recsIds"); if (!cJSON_IsObject(m)) { m = cJSON_CreateObject(); store_set_node("recsIds", m); } return m; }
static int add_job(int kind, const char *path, const char *cache, long maxAge) {
  if (njobs >= MAX_JOBS) return -1; Job *j = &jobs[njobs]; memset(j, 0, sizeof *j); j->kind = kind;
  snprintf(j->path, sizeof j->path, "%s", path); snprintf(j->cache, sizeof j->cache, "%s", cache ? cache : ""); j->maxAge = maxAge; return njobs++;
}
static void job_answer(int ji, const unsigned char *body, size_t len);
static void on_job(int status, const unsigned char *body, size_t len, void *ud) {
  uintptr_t u = (uintptr_t)ud; unsigned rid = (unsigned)(u >> 8); int ji = (int)(u & 0xff);
  if (rid != (runId & 0xffffff) || ji >= njobs) return;
  if (status == 200 && body) { if (jobs[ji].cache[0]) cache_write(jobs[ji].cache, body, len); job_answer(ji, body, len); }
  jobs[ji].state = 2;
}
static void job_answer(int ji, const unsigned char *body, size_t len) {
  Job *j = &jobs[ji]; cJSON *r = cJSON_ParseWithLength((const char *)body, len); if (!r) return;
  if (j->kind == J_FIND) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(r, seeds[j->seed].tk == 'm' ? "movie_results" : "tv_results"), *e = cJSON_GetArrayItem(a, 0);
    Seed *s = &seeds[j->seed];
    if (e) { s->tmdb = (int)rjn(e, "id"); snprintf(s->title, sizeof s->title, "%s", s->tk == 'm' ? rjs(e, "title") : rjs(e, "name")); snprintf(s->lang, sizeof s->lang, "%s", rjs(e, "original_language"));
      const cJSON *g; s->ng = 0; cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(e, "genre_ids")) if (s->ng < 8) s->genres[s->ng++] = g->valueint;
      cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "k", s->tk == 'm' ? "m" : "t"); cJSON_AddNumberToObject(m, "id", s->tmdb); cJSON_AddStringToObject(m, "t", s->title); cJSON_AddStringToObject(m, "l", s->lang);
      cJSON *ga = cJSON_AddArrayToObject(m, "g"); for (int i = 0; i < s->ng; i++) cJSON_AddItemToArray(ga, cJSON_CreateNumber(s->genres[i]));
      cJSON *map = idmap(); cJSON_DeleteItemFromObjectCaseSensitive(map, s->imdb); cJSON_AddItemToObject(map, s->imdb, m);
      char rk[24]; snprintf(rk, sizeof rk, "%c%d", s->tk, s->tmdb); cJSON_DeleteItemFromObjectCaseSensitive(map, rk); cJSON_AddStringToObject(map, rk, s->imdb); store_set_node("recsIds", map); }
  } else if (j->kind == J_REC || j->kind == J_DISC) {
    Seed *s = &seeds[j->seed]; const cJSON *res = cJSON_GetObjectItemCaseSensitive(r, "results");
    if (j->kind == J_REC) s->nr = recs_parse(res, s->tk, s->rec, RECS_LIST); else s->nd = recs_parse(res, s->tk, s->disc, RECS_LIST);
  } else if (j->kind == J_TOP) {
    topN[j->series][j->lang] = recs_parse(cJSON_GetObjectItemCaseSensitive(r, "results"), j->series ? 't' : 'm', topRaw[j->series][j->lang], RECS_LIST);
  } else if (j->kind == J_EXT) {
    const char *tt = rjs(r, "imdb_id");
    if (tt[0] == 't' && tt[1] == 't') { cJSON *map = idmap(); char rk[24]; snprintf(rk, sizeof rk, "%c%d", j->tk, j->tmdb); cJSON_DeleteItemFromObjectCaseSensitive(map, rk); cJSON_AddStringToObject(map, rk, tt); store_set_node("recsIds", map); }
  }
  cJSON_Delete(r);
}
/* a job answered from the disk cache at once, else asked (INFLIGHT at a time) */
static void pump(void) {
  int fly = 0; for (int i = 0; i < njobs; i++) fly += jobs[i].state == 1;
  for (int i = 0; i < njobs && fly < INFLIGHT; i++) { Job *j = &jobs[i]; if (j->state) continue;
    if (j->cache[0]) { size_t len = 0; unsigned char *b = cache_read(j->cache, j->maxAge, &len); if (b) { job_answer(i, b, len); free(b); j->state = 2; continue; } }
    unsigned id = tmdb_request(j->path, on_job, (void *)(uintptr_t)(((uintptr_t)(runId & 0xffffff) << 8) | (uintptr_t)i));
    if (!id) { j->state = 2; continue; } j->state = 1; fly++; requests++; }
}
static int all_done(void) { for (int i = 0; i < njobs; i++) if (jobs[i].state != 2) return 0; return 1; }

/* the seeds: what the library says was watched, weighted (recs_core), the strongest 15 */
static int cmp_seed(const void *a, const void *b) { double x = ((const Seed *)a)->w, y = ((const Seed *)b)->w; return x < y ? 1 : x > y ? -1 : 0; }
static void gather_seeds(void) {
  static Seed all[256]; int n = 0; const cJSON *it; long today = today_days();
  cJSON_ArrayForEach(it, lib_all()) {
    if (n >= 256) break; const char *id = rjs(it, "_id"), *ty = rjs(it, "type"); if (strncmp(id, "tt", 2) || (strcmp(ty, "movie") && strcmp(ty, "series"))) continue;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "state"); double dur = rjn(s, "duration"), off = rjn(s, "timeOffset"), overall = rjn(s, "overallTimeWatched");
    int marked = rjn(s, "flaggedWatched") > 0 || rjn(s, "timesWatched") > 0, series = !strcmp(ty, "series");
    double frac = marked ? 1 : dur > 0 ? off / dur : 0; int eps = series && dur > 0 ? (int)(overall / dur) : 0;
    const char *lw = rjs(s, "lastWatched"); if (!lw[0]) lw = rjs(it, "_mtime"); long d = recs_days(lw); double age = d ? (double)(today - d) : 365;
    double w = recs_seed_weight(age, frac, marked, series, eps); if (w <= 0.001) continue;
    Seed *x = &all[n++]; memset(x, 0, sizeof *x); snprintf(x->imdb, sizeof x->imdb, "%s", id); x->tk = series ? 't' : 'm'; x->w = w; x->age = age; snprintf(x->title, sizeof x->title, "%s", rjs(it, "name"));
  }
  qsort(all, (size_t)n, sizeof *all, cmp_seed); nseeds = n < RECS_MAX_SEEDS ? n : RECS_MAX_SEEDS; memcpy(seeds, all, sizeof(Seed) * (size_t)nseeds);
}
/* the languages the user watches a lot join the region's own (Ireland + a Hindi-heavy history: English and Hindi),
   unless languages were chosen in Settings */
static void langs_from_history(void) {
  if (store_get("recsLangs", "")[0]) return;
  double tot = 0; for (int i = 0; i < nseeds; i++) tot += seeds[i].tmdb ? seeds[i].w : 0; if (tot <= 0) return;
  for (int i = 0; i < nseeds && nlangs < 4; i++) { const char *l = seeds[i].lang; if (!seeds[i].tmdb || !l[0]) continue;
    int have = 0; for (int k = 0; k < nlangs && !have; k++) have = !strcmp(langs[k], l); if (have) continue;
    double w = 0; for (int j = 0; j < nseeds; j++) if (seeds[j].tmdb && !strcmp(seeds[j].lang, l)) w += seeds[j].w;
    if (w / tot >= 0.25) { /* first when it is most of the watching */ if (w / tot >= 0.5) { memmove(langs[1], langs[0], sizeof langs[0] * (size_t)nlangs); snprintf(langs[0], 4, "%s", l); } else snprintf(langs[nlangs], 4, "%s", l); nlangs++; } }
}
static void ask_lists(void) {
  char p[600], c[96]; langs_from_history(); int yr = (int)(1970 + today_days() / 365.25) - 10;
  for (int i = 0; i < nseeds; i++) { Seed *s = &seeds[i]; if (!s->tmdb) continue; const char *seg = s->tk == 'm' ? "movie" : "tv";
    snprintf(p, sizeof p, "/%s/%d/recommendations?page=1", seg, s->tmdb); snprintf(c, sizeof c, "recs:rec:%c%d", s->tk, s->tmdb); int j = add_job(J_REC, p, c, LIST_AGE); if (j >= 0) jobs[j].seed = i;
    snprintf(p, sizeof p, "/discover/%s?with_original_language=%s&with_genres=%d&sort_by=popularity.desc&vote_count.gte=20&%s.gte=%d-01-01", seg, s->lang, s->ng ? s->genres[0] : 18, s->tk == 'm' ? "primary_release_date" : "first_air_date", yr);
    snprintf(c, sizeof c, "recs:disc:%c%d", s->tk, s->tmdb); j = add_job(J_DISC, p, c, LIST_AGE); if (j >= 0) jobs[j].seed = i; }
  /* Top 10: per language, streaming in the region, recent */
  time_t now = time(NULL); char d0[16], d1[16], d2[16], d3[16]; struct tm tm;
  time_t t0 = now - 180 * 86400, t2 = now - 730 * 86400, t3 = now - 30 * 86400;
  gmtime_r(&t0, &tm); strftime(d0, sizeof d0, "%Y-%m-%d", &tm); gmtime_r(&now, &tm); strftime(d1, sizeof d1, "%Y-%m-%d", &tm);
  gmtime_r(&t2, &tm); strftime(d2, sizeof d2, "%Y-%m-%d", &tm); gmtime_r(&t3, &tm); strftime(d3, sizeof d3, "%Y-%m-%d", &tm);
  for (int l = 0; l < nlangs; l++) for (int s = 0; s < 2; s++) {
    if (!s) snprintf(p, sizeof p, "/discover/movie?watch_region=%s&with_watch_monetization_types=flatrate|free|ads&sort_by=popularity.desc&with_original_language=%s&primary_release_date.gte=%s&primary_release_date.lte=%s", recs_region(), langs[l], d0, d1);
    else snprintf(p, sizeof p, "/discover/tv?watch_region=%s&with_watch_monetization_types=flatrate|free|ads&sort_by=popularity.desc&with_original_language=%s&first_air_date.gte=%s&air_date.gte=%s", recs_region(), langs[l], d2, d3);
    snprintf(c, sizeof c, "recs:top:%s:%s:%c", recs_region(), langs[l], s ? 't' : 'm'); int j = add_job(J_TOP, p, c, TOP_AGE); if (j >= 0) { jobs[j].lang = l; jobs[j].series = s; } }
}
static const char *tt_of(char tk, int tmdb) { char rk[24]; snprintf(rk, sizeof rk, "%c%d", tk, tmdb); const cJSON *v = cJSON_GetObjectItemCaseSensitive(idmap(), rk); return cJSON_IsString(v) ? v->valuestring : NULL; }
static void rank_now(void) {
  RecSeed rs[RECS_MAX_SEEDS]; int ns = 0, ex[RECS_MAX_SEEDS + 64], nex = 0;
  for (int i = 0; i < nseeds; i++) { Seed *s = &seeds[i]; if (!s->tmdb) continue; RecSeed *r = &rs[ns++]; memset(r, 0, sizeof *r);
    r->tmdb = s->tmdb; r->kind = s->tk; snprintf(r->title, sizeof r->title, "%s", s->title); memcpy(r->genres, s->genres, sizeof r->genres); r->ng = s->ng; snprintf(r->lang, sizeof r->lang, "%s", s->lang);
    r->w = s->w; r->list = s->rec; r->n = s->nr; r->disc = s->disc; r->nd = s->nd; ex[nex++] = recs_key(s->tmdb, s->tk); }
  const cJSON *dm = store_node("recsDismissed"), *d; cJSON_ArrayForEach(d, dm) if (cJSON_IsNumber(d) && nex < (int)(sizeof ex / sizeof ex[0])) ex[nex++] = d->valueint;
  RecPrefs p; memset(&p, 0, sizeof p); for (int l = 0; l < nlangs; l++) snprintf(p.langs[p.nlangs++], 4, "%s", langs[l]); p.exclude = ex; p.nexclude = nex; p.today = today_days(); p.minMovieAge = recs_hide_cinema() ? 30 : 0;
  RecCand tmp[64]; int n = ns ? recs_rank(rs, ns, &p, tmp, 64) : 0;
  npick[0] = n < 8 ? n : 8; memcpy(pick[0], tmp, sizeof(RecCand) * (size_t)npick[0]);
  /* because-you-watched: the last thing watched (a series being watched included), as Netflix does */
  int best = -1; double bestAge = 1e9;
  for (int i = 0, k = 0; i < nseeds; i++) { if (!seeds[i].tmdb) continue; if (seeds[i].age < bestAge || (seeds[i].age == bestAge && best >= 0 && rs[k].w > rs[best].w)) { bestAge = seeds[i].age; best = k; } k++; }
  npick[1] = 0; pickSeed = best;
  if (best >= 0) { int skip[8]; for (int i = 0; i < npick[0]; i++) skip[i] = recs_key(pick[0][i].tmdb, pick[0][i].kind); npick[1] = recs_rank_seed(&rs[best], rs, ns, &p, skip, npick[0], pick[1], 20); }
  RecPrefs pt = p; int dex = nex - ns; pt.exclude = ex + ns; pt.nexclude = dex > 0 ? dex : 0; /* the charts keep what was watched; only "Not interested" leaves them */
  for (int s = 0; s < 2; s++) { const RecCand *pl[8]; for (int l = 0; l < nlangs; l++) pl[l] = topRaw[s][l]; npick[2 + s] = recs_interleave(pl, topN[s], nlangs, &pt, pick[2 + s], 14); }
  /* the IMDb ids the stream addons need, for what will be shown */
  char pth[96];
  for (int k = 0; k < 4; k++) for (int i = 0; i < npick[k]; i++) { RecCand *c = &pick[k][i]; if (tt_of(c->kind, c->tmdb)) continue;
    int dup = 0; for (int q = 0; q < njobs && !dup; q++) dup = jobs[q].kind == J_EXT && jobs[q].tmdb == c->tmdb && jobs[q].tk == c->kind; if (dup) continue;
    snprintf(pth, sizeof pth, "/%s/%d/external_ids", c->kind == 'm' ? "movie" : "tv", c->tmdb); int j = add_job(J_EXT, pth, NULL, 0); if (j >= 0) { jobs[j].tmdb = c->tmdb; jobs[j].tk = c->kind; } }
}
static void build_out(Out *o, const RecCand *c, int n, int max) {
  free(o->it); memset(o, 0, sizeof *o); o->it = calloc((size_t)(n ? n : 1), sizeof(Item)); if (!o->it) return;
  for (int i = 0; i < n && o->n < max; i++) { const char *tt = tt_of(c[i].kind, c[i].tmdb); if (!tt) continue; Item *it = &o->it[o->n++];
    snprintf(it->id, sizeof it->id, "%s", tt); snprintf(it->type, sizeof it->type, "%s", c[i].kind == 'm' ? "movie" : "series"); snprintf(it->name, sizeof it->name, "%s", c[i].title);
    snprintf(it->poster, sizeof it->poster, IMG_P "%s", c[i].poster); snprintf(it->background, sizeof it->background, IMG_B "%s", c[i].backdrop);
    snprintf(it->description, sizeof it->description, "%s", c[i].overview); snprintf(it->epName, sizeof it->epName, "%s", c[i].date);
    if (strlen(c[i].date) >= 4) snprintf(it->metaLine, sizeof it->metaLine, "%.4s", c[i].date); }
}
static void finish(void) {
  build_out(&bill, pick[0], npick[0], 8); build_out(&byw, pick[1], npick[1], 20); build_out(&topM, pick[2], npick[2], 10); build_out(&topS, pick[3], npick[3], 10);
  { const char *t = ""; for (int i = 0, k = 0; i < nseeds; i++) { if (!seeds[i].tmdb) continue; if (k++ == pickSeed) { t = seeds[i].title; break; } }
    snprintf(bywTitle, sizeof bywTitle, "%s %s", T("Because you watched"), t); }
  doneAt = time(NULL);
  const char *rn = recs_region_name(recs_region());
  snprintf(topMTitle, sizeof topMTitle, "%s %s %s", T("Top 10 movies in"), rn, T("this week")); snprintf(topSTitle, sizeof topSTitle, "%s %s %s", T("Top 10 series in"), rn, T("this week"));
  if (byw.n < 4) byw.n = 0; /* a row of two or three looks broken: none instead */
  result_save(); haveResult = 1; ver++;
  fprintf(stderr, "[recs] ready: %d seeds, billboard %d, byw %d, top %d/%d, %d requests, languages", nseeds, bill.n, byw.n, topM.n, topS.n, requests);
  for (int l = 0; l < nlangs; l++) fprintf(stderr, " %s", langs[l]);
  fprintf(stderr, "\n");
}
static void start_run(void) {
  runId++; njobs = 0; phase = 1; requests = 0; memset(topN, 0, sizeof topN);
  nlangs = 0; { char buf[64]; snprintf(buf, sizeof buf, "%s", recs_langs()); for (char *t = strtok(buf, ","); t && nlangs < 8; t = strtok(NULL, ",")) snprintf(langs[nlangs++], 4, "%.3s", t); }
  gather_seeds();
  for (int i = 0; i < nseeds; i++) { Seed *s = &seeds[i]; const cJSON *m = cJSON_GetObjectItemCaseSensitive(idmap(), s->imdb);
    if (cJSON_IsObject(m)) { s->tmdb = (int)rjn(m, "id"); snprintf(s->title, sizeof s->title, "%s", rjs(m, "t")); snprintf(s->lang, sizeof s->lang, "%s", rjs(m, "l"));
      const cJSON *g; s->ng = 0; cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(m, "g")) if (s->ng < 8) s->genres[s->ng++] = g->valueint; }
    else { char p[96]; snprintf(p, sizeof p, "/find/%s?external_source=imdb_id", s->imdb); int j = add_job(J_FIND, p, NULL, 0); if (j >= 0) jobs[j].seed = i; } }
  fprintf(stderr, "[recs] run: %d seeds, region %s, languages %s\n", nseeds, recs_region(), recs_langs());
}
void recs_refresh(void) { haveResult = 0; phase = 0; startAt = app_now(); }
void recs_dismiss(const Item *it) {
  for (int k = 0; k < 4; k++) { Out *o = k == 0 ? &bill : k == 1 ? &byw : k == 2 ? &topM : &topS; int i = in_out(o, it); if (i < 0) continue;
    const char *rk = NULL; const cJSON *e; cJSON_ArrayForEach(e, idmap()) if (cJSON_IsString(e) && !strcmp(e->valuestring, it->id)) { rk = e->string; break; }
    if (rk) { cJSON *dm = store_node("recsDismissed"); if (!cJSON_IsArray(dm)) { dm = cJSON_CreateArray(); store_set_node("recsDismissed", dm); }
      cJSON_AddItemToArray(dm, cJSON_CreateNumber(recs_key(atoi(rk + 1), rk[0]))); store_set_node("recsDismissed", dm); }
    memmove(&o->it[i], &o->it[i + 1], sizeof(Item) * (size_t)(o->n - i - 1)); o->n--; }
  result_save(); ver++;
}

int recs_region_count(void) { return NREG; }
const char *recs_region_code(int i) { return i >= 0 && i < NREG ? REGIONS[i].cc : ""; }
void recs_status(char *out, size_t cap) {
  if (!tmdb_has_key()) { snprintf(out, cap, "%s", T("Needs a TMDB key: add one under Keys")); return; }
  if (!recs_on()) { snprintf(out, cap, "%s", T("Off: Home shows your addons' rows only")); return; }
  if (phase) { snprintf(out, cap, "%s", T("On. Getting your picks from TMDB...")); return; }
  if (!bill.n && !byw.n) { snprintf(out, cap, "%s", T("On. Watch something and your picks appear on Home")); return; }
  long mins = doneAt ? (long)(time(NULL) - doneAt) / 60 : -1;
  if (mins < 0) snprintf(out, cap, "%s %d %s", T("On."), bill.n + byw.n, T("picks on Home"));
  else if (mins < 60) snprintf(out, cap, "%s %d %s, %ld %s", T("On."), bill.n + byw.n, T("picks on Home"), mins, T("min ago"));
  else snprintf(out, cap, "%s %d %s, %ld %s", T("On."), bill.n + byw.n, T("picks on Home"), mins / 60, T("h ago"));
}
void recs_tick(void) {
  { static unsigned ser; static char sig[160]; if (store_serial() != ser) { ser = store_serial(); char s2[160]; snprintf(s2, sizeof s2, "%s|%s|%d|%d", recs_region(), recs_langs(), recs_hide_cinema(), recs_on());
      if (sig[0] && strcmp(s2, sig)) recs_refresh(); snprintf(sig, sizeof sig, "%s", s2); } } /* a setting changed (Settings writes the store): ask again */
  if (!recs_on() || !tmdb_has_key()) { if (bill.n || byw.n || topM.n || topS.n) { bill.n = byw.n = topM.n = topS.n = 0; ver++; } return; }
  double now = app_now();
  if (!loaded) { loaded = 1; haveResult = result_load(RESULT_AGE * 4); startAt = now + (haveResult ? 20 : 4); libSeen = lib_version(); } /* kept lists at once; asking again waits for Home */
  if (!recs_paused() && lib_version() != libSeen) { libSeen = lib_version(); libAt = now + 30; } /* watched something: again in 30 s (one change, not each progress save) */
  if (libAt && now >= libAt && phase == 0) { libAt = 0; haveResult = 0; startAt = now; }
  if (phase == 0) {
    if (now < startAt || !app_splash_done || app_idle() < 0.5f) return;
    if (haveResult) { size_t l = 0; unsigned char *b = cache_read("recs:result", RESULT_AGE, &l); free(b); if (b) { startAt = now + 600; return; } } /* fresh enough */
    start_run(); }
  pump(); if (!all_done()) return;
  if (phase == 1) { int finds = 0; for (int i = 0; i < njobs; i++) finds += jobs[i].kind == J_FIND;
    if (finds) { /* the seeds found: now their lists */ for (int i = 0; i < njobs; i++) jobs[i].kind = -1; njobs = 0; ask_lists(); if (njobs) return; }
    else if (njobs == 0) { ask_lists(); if (njobs) return; }
    phase = 2; njobs = 0; rank_now(); if (njobs) return; }
  if (phase == 2) { finish(); phase = 0; startAt = now + RESULT_AGE; }
}
