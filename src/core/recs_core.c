/* The recommendation maths (see recs_core.h). No app state, no I/O: tests/recs runs it on fixtures. */
#include "core/recs_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* seed, genre, lang, quality, fresh, old (per year past 8, max 10x), disc (a discover rank counts this much
   of a recommendation rank) */
const RecWeights RECS_W = { 1.0, 0.3, 0.35, 0.1, 0.1, 0.02, 0.6 };

double recs_seed_weight(double ageDays, double frac, int marked, int isSeries, int eps) {
  if (ageDays < 0) ageDays = 0;
  double comp = marked || frac >= 0.70 ? 1.0 : frac >= 0.20 ? 0.5 : 0.0;
  if (isSeries && eps >= 3) comp = comp < 1.0 ? 1.0 : comp; /* three episodes in: they like it, whatever the last one's progress */
  double w = pow(0.5, ageDays / RECS_HALF_LIFE_D) * comp;
  if (isSeries && eps >= 3) w *= 1.3;
  return w;
}

int recs_key(int tmdb, char kind) { return kind == 't' ? -tmdb : tmdb; }

long recs_days(const char *s) {
  int y, m, d; if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return 0;
  /* days from civil (Howard Hinnant) */
  y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static const char *jstr(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) && v->valuestring ? v->valuestring : ""; }
static double jnum(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : 0; }

int recs_parse(const cJSON *res, char kind, RecCand *out, int max) {
  int n = 0; const cJSON *e;
  cJSON_ArrayForEach(e, res) {
    if (n >= max) break;
    char k = kind; const char *mt = jstr(e, "media_type");
    if (!k) k = !strcmp(mt, "movie") ? 'm' : !strcmp(mt, "tv") ? 't' : 0;
    if (!k) continue;                                        /* a person, a collection */
    const char *poster = jstr(e, "poster_path"), *bd = jstr(e, "backdrop_path");
    if (!poster[0] || !bd[0]) continue;                      /* no art: it would look broken on Home */
    RecCand *c = &out[n]; memset(c, 0, sizeof *c);
    c->tmdb = (int)jnum(e, "id"); c->kind = k; if (c->tmdb <= 0) continue;
    snprintf(c->title, sizeof c->title, "%s", k == 'm' ? jstr(e, "title") : jstr(e, "name"));
    snprintf(c->poster, sizeof c->poster, "%s", poster); snprintf(c->backdrop, sizeof c->backdrop, "%s", bd);
    snprintf(c->date, sizeof c->date, "%s", k == 'm' ? jstr(e, "release_date") : jstr(e, "first_air_date"));
    snprintf(c->lang, sizeof c->lang, "%s", jstr(e, "original_language"));
    snprintf(c->overview, sizeof c->overview, "%s", jstr(e, "overview"));
    const cJSON *g, *gs = cJSON_GetObjectItemCaseSensitive(e, "genre_ids");
    cJSON_ArrayForEach(g, gs) if (cJSON_IsNumber(g) && c->ng < 8) c->genres[c->ng++] = g->valueint;
    c->adult = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "adult"));
    c->vote = (float)jnum(e, "vote_average"); c->votes = (int)jnum(e, "vote_count"); c->pop = (float)jnum(e, "popularity");
    n++;
  }
  return n;
}

/* ---- taste: genre and language shares over the seeds, by weight ---- */
typedef struct { int gid[RECS_GENRES]; double gw[RECS_GENRES]; int ng; double gmax; char lang[16][4]; double lw[16]; int nl; double wsum; } Taste;

static void taste_build(const RecSeed *s, int ns, Taste *t) {
  memset(t, 0, sizeof *t);
  for (int i = 0; i < ns; i++) {
    double w = s[i].w; if (w <= 0) continue; t->wsum += w;
    for (int g = 0; g < s[i].ng; g++) { int k = 0; while (k < t->ng && t->gid[k] != s[i].genres[g]) k++;
      if (k == t->ng) { if (t->ng == RECS_GENRES) continue; t->gid[t->ng++] = s[i].genres[g]; } t->gw[k] += w; }
    if (s[i].lang[0]) {
      int k = 0; while (k < t->nl && strcmp(t->lang[k], s[i].lang)) k++;
      if (k == t->nl && t->nl < 16) snprintf(t->lang[t->nl++], 4, "%s", s[i].lang);
      if (k < 16) t->lw[k] += w;
    }
  }
  for (int k = 0; k < t->ng; k++) if (t->gw[k] > t->gmax) t->gmax = t->gw[k];
}
static double taste_genre(const Taste *t, const RecCand *c) {
  if (!c->ng || t->gmax <= 0) return 0;
  double s = 0;
  for (int g = 0; g < c->ng; g++) for (int k = 0; k < t->ng; k++) if (t->gid[k] == c->genres[g]) { s += t->gw[k] / t->gmax; break; }
  return s / c->ng;
}
static double taste_lang(const Taste *t, const RecPrefs *p, const RecCand *c) {
  double pick = 0, share = 0;
  for (int i = 0; i < p->nlangs; i++) if (!strcmp(p->langs[i], c->lang)) { pick = 1; break; }
  if (t->wsum > 0) for (int k = 0; k < t->nl; k++) if (!strcmp(t->lang[k], c->lang)) { share = t->lw[k] / t->wsum; break; }
  return 0.5 * pick + 0.5 * share;
}
static int excluded(const RecPrefs *p, int key) { for (int i = 0; i < p->nexclude; i++) if (p->exclude[i] == key) return 1; return 0; }

static double part_rest(const Taste *t, const RecPrefs *p, const RecCand *c) {
  double q = c->vote / 10.0 * (c->votes >= 200 ? 1.0 : c->votes / 200.0);
  double fresh = 0, old = 0; long d = recs_days(c->date);
  if (d && p->today) { long age = p->today - d; fresh = age < 0 || age > 730 ? 0 : 1.0 - age / 730.0;
    double yrs = age / 365.0 - 8; if (yrs > 0) old = yrs > 10 ? 10 : yrs; }
  return RECS_W.genre * taste_genre(t, c) + RECS_W.lang * taste_lang(t, p, c) + RECS_W.quality * q + RECS_W.fresh * fresh - RECS_W.old * old;
}
static int unreleased(const RecPrefs *p, const RecCand *c) { long d = recs_days(c->date); return !d || (p->today && d > p->today); }
static int cmp_score(const void *a, const void *b) { double x = ((const RecCand *)a)->score, y = ((const RecCand *)b)->score; return x < y ? 1 : x > y ? -1 : 0; }

/* every seed's list merged into one scored, de-duplicated array (not yet ordered) */
static int merge(const RecSeed *s, int ns, const RecPrefs *p, const Taste *t, RecCand *all, int max) {
  int n = 0; double wsum = 0; for (int i = 0; i < ns; i++) if (s[i].w > 0) wsum += s[i].w;
  if (wsum <= 0) return 0;
  int *seedKeys = calloc((size_t)ns + 1, sizeof(int)); if (!seedKeys) return 0;
  for (int i = 0; i < ns; i++) seedKeys[i] = recs_key(s[i].tmdb, s[i].kind);
  for (int i = 0; i < ns; i++) {
    if (s[i].w <= 0) continue;
    for (int r2 = 0; r2 < s[i].n + s[i].nd; r2++) {
      int fromDisc = r2 >= s[i].n, r = fromDisc ? r2 - s[i].n : r2; if (r >= RECS_LIST) continue;
      const RecCand *c = fromDisc ? &s[i].disc[r] : &s[i].list[r]; int key = recs_key(c->tmdb, c->kind);
      if (excluded(p, key) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
      int self = 0; for (int k = 0; k < ns && !self; k++) self = seedKeys[k] == key; if (self) continue; /* a watched title itself */
      int at = 0; while (at < n && recs_key(all[at].tmdb, all[at].kind) != key) at++;
      if (at == n) { if (n == max) continue; all[n] = *c; all[n].seedPart = 0; all[n].score = 0; all[n].nSeeds = 0; all[n].bestSeed = -1; n++; }
      double contrib = s[i].w * (1.0 - (double)r / RECS_LIST) / wsum * (fromDisc ? RECS_W.disc : 1.0);
      RecCand *a = &all[at]; a->seedPart += contrib; a->nSeeds++;
      if (a->bestSeed < 0 || contrib > a->score) { a->bestSeed = i; a->score = contrib; } /* (score holds the best contribution until scoring) */
    }
  }
  free(seedKeys);
  for (int i = 0; i < n; i++) all[i].score = RECS_W.seed * all[i].seedPart + part_rest(t, p, &all[i]);
  return n;
}

int recs_rank(const RecSeed *s, int ns, const RecPrefs *p, RecCand *out, int max) {
  RecCand *all = malloc(sizeof(RecCand) * RECS_MAX_CAND); if (!all) return 0;
  Taste t; taste_build(s, ns, &t);
  int n = merge(s, ns, p, &t, all, RECS_MAX_CAND);
  qsort(all, (size_t)n, sizeof *all, cmp_score);
  /* diversity: the first 20 take at most 4 per best seed; the rest wait their turn after them */
  int perSeed[RECS_MAX_SEEDS] = { 0 }, m = 0; char *used = calloc((size_t)n + 1, 1);
  for (int i = 0; i < n && m < max && m < 20; i++) { int b = all[i].bestSeed; if (b >= 0 && b < RECS_MAX_SEEDS && perSeed[b] >= 4) continue; if (b >= 0 && b < RECS_MAX_SEEDS) perSeed[b]++; out[m++] = all[i]; used[i] = 1; }
  for (int i = 0; i < n && m < max; i++) if (!used[i]) out[m++] = all[i];
  free(used); free(all); return m;
}

int recs_rank_seed(const RecSeed *seed, const RecSeed *s, int ns, const RecPrefs *p, const int *skip, int nskip, RecCand *out, int max) {
  RecCand *all = malloc(sizeof(RecCand) * RECS_MAX_CAND); if (!all) return 0;
  Taste t; taste_build(s, ns, &t);
  int n = merge(s, ns, p, &t, all, RECS_MAX_CAND), m = 0;
  qsort(all, (size_t)n, sizeof *all, cmp_score);
  double top = 0;
  for (int i = 0; i < n && m < max; i++) {
    int key = recs_key(all[i].tmdb, all[i].kind), in = 0, sk = 0;
    for (int r = 0; r < seed->n && !in; r++) in = recs_key(seed->list[r].tmdb, seed->list[r].kind) == key;
    for (int r = 0; r < seed->nd && !in; r++) in = recs_key(seed->disc[r].tmdb, seed->disc[r].kind) == key;
    for (int k = 0; k < nskip && !sk; k++) sk = skip[k] == key;
    if (!in || sk) continue;
    if (!m) top = all[i].score;
    if (m >= 8 && all[i].score < RECS_ROW_FLOOR * top) break; /* a weak tail ends the row early */
    out[m++] = all[i];
  }
  free(all); return m;
}

int recs_filter(const RecCand *in, int n, const RecPrefs *p, RecCand *out, int max) {
  int m = 0;
  for (int i = 0; i < n && m < max; i++) { const RecCand *c = &in[i];
    if (excluded(p, recs_key(c->tmdb, c->kind)) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
    out[m++] = *c; }
  return m;
}

int recs_interleave(const RecCand *const *lists, const int *n, int nl, const RecPrefs *p, RecCand *out, int max) {
  int m = 0, at[8] = { 0 }; if (nl > 8) nl = 8;
  for (int more = 1; more && m < max; ) {
    more = 0;
    for (int l = 0; l < nl && m < max; l++) {
      while (at[l] < n[l]) {
        const RecCand *c = &lists[l][at[l]++]; int key = recs_key(c->tmdb, c->kind), dup = 0;
        for (int k = 0; k < m && !dup; k++) dup = recs_key(out[k].tmdb, out[k].kind) == key;
        if (dup || excluded(p, key) || (c->adult && !p->allowAdult) || unreleased(p, c)) continue;
        out[m++] = *c; more = 1; break;
      }
    }
  }
  return m;
}
