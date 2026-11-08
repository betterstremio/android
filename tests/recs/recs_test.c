/* tests/recs: the recommendation maths (src/core/recs_core.c), no app.
   recs_test               synthetic cases with checks (exit 1 on a failure)
   recs_test --real DIR    ranks what tests/recs/fetch.py saved from TMDB for a sample library and prints it */
#include "core/recs_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else printf("ok   " __VA_ARGS__), printf("\n"); } while (0)

/* a TMDB-like result entry */
static cJSON *ent(int id, const char *title, const char *date, const char *lang, int g1, int g2, double vote, int votes, int adult) {
  cJSON *e = cJSON_CreateObject(); cJSON_AddNumberToObject(e, "id", id); cJSON_AddStringToObject(e, "title", title);
  cJSON_AddStringToObject(e, "release_date", date); cJSON_AddStringToObject(e, "original_language", lang);
  cJSON_AddStringToObject(e, "poster_path", "/p.jpg"); cJSON_AddStringToObject(e, "backdrop_path", "/b.jpg");
  cJSON *g = cJSON_AddArrayToObject(e, "genre_ids"); if (g1) cJSON_AddItemToArray(g, cJSON_CreateNumber(g1)); if (g2) cJSON_AddItemToArray(g, cJSON_CreateNumber(g2));
  cJSON_AddNumberToObject(e, "vote_average", vote); cJSON_AddNumberToObject(e, "vote_count", votes); cJSON_AddBoolToObject(e, "adult", adult);
  return e;
}
static int find(const RecCand *c, int n, int id) { for (int i = 0; i < n; i++) if (c[i].tmdb == id) return i; return -1; }

static void synthetic(void) {
  long today = recs_days("2026-11-08");
  /* weights */
  double fresh = recs_seed_weight(1, 0.95, 0, 0, 0), old = recs_seed_weight(60, 0.95, 0, 0, 0), half = recs_seed_weight(1, 0.4, 0, 0, 0), barely = recs_seed_weight(1, 0.1, 0, 0, 0);
  CHECK(fresh > old, "recent finished film outweighs an old one (%.3f > %.3f)", fresh, old);
  CHECK(half > 0 && half < fresh, "half-watched counts, less (%.3f)", half);
  CHECK(barely == 0, "under 20%% watched is ignored");
  CHECK(recs_seed_weight(1, 0.1, 0, 1, 4) > fresh, "a series 4 episodes in counts more than a film");
  CHECK(recs_key(5, 'm') != recs_key(5, 't'), "movie and tv ids kept apart");
  CHECK(recs_days("1970-01-02") == 1 && recs_days("2026-11-08") > 20000, "date to days");

  /* seed A (Hindi drama, fresh, finished) and seed B (English sci-fi, older) */
  cJSON *la = cJSON_CreateArray(), *lb = cJSON_CreateArray();
  cJSON_AddItemToArray(la, ent(101, "Both lists", "2026-06-01", "hi", 18, 35, 7.5, 900, 0));
  for (int i = 0; i < 18; i++) { char t[32]; snprintf(t, sizeof t, "A-%d", i); cJSON_AddItemToArray(la, ent(200 + i, t, "2025-12-01", "hi", 18, 0, 6.5, 300, 0)); }
  cJSON_AddItemToArray(la, ent(999, "Watched already", "2025-01-01", "hi", 18, 0, 8, 900, 0));
  cJSON_AddItemToArray(lb, ent(101, "Both lists", "2026-06-01", "hi", 18, 35, 7.5, 900, 0));
  cJSON_AddItemToArray(lb, ent(300, "Sci-fi one", "2026-01-01", "en", 878, 0, 7.0, 500, 0));
  cJSON_AddItemToArray(lb, ent(301, "Adult one", "2026-01-01", "en", 878, 0, 7.0, 500, 1));
  cJSON_AddItemToArray(lb, ent(302, "Not out yet", "2027-05-01", "en", 878, 0, 0, 0, 0));
  cJSON_AddItemToArray(lb, ent(303, "Dismissed", "2026-02-01", "en", 878, 0, 7.0, 500, 0));
  cJSON_AddItemToArray(lb, ent(400, "Seed A itself", "2025-10-01", "hi", 18, 0, 7.0, 500, 0));
  RecCand ca[RECS_LIST], cb[RECS_LIST]; int na = recs_parse(la, 'm', ca, RECS_LIST), nb = recs_parse(lb, 'm', cb, RECS_LIST);
  CHECK(na == 20 && nb == 6, "parse keeps entries with art (%d, %d)", na, nb);
  RecSeed s[2] = { { .tmdb = 400, .kind = 'm', .title = "Seed A", .genres = { 18 }, .ng = 1, .lang = "hi", .w = fresh, .list = ca, .n = na },
                   { .tmdb = 500, .kind = 'm', .title = "Seed B", .genres = { 878 }, .ng = 1, .lang = "en", .w = old, .list = cb, .n = nb } };
  int ex[2] = { recs_key(999, 'm'), recs_key(303, 'm') };
  RecPrefs p = { .langs = { "hi", "en" }, .nlangs = 2, .exclude = ex, .nexclude = 2, .today = today };
  RecCand out[64]; int n = recs_rank(s, 2, &p, out, 64);
  CHECK(n > 0 && out[0].tmdb == 101, "the title both seeds recommend comes first (got %d)", n ? out[0].tmdb : -1);
  CHECK(find(out, n, 999) < 0 && find(out, n, 303) < 0, "watched and dismissed titles are left out");
  CHECK(find(out, n, 301) < 0, "adult titles are left out");
  CHECK(find(out, n, 302) < 0, "unreleased titles are left out");
  CHECK(find(out, n, 400) < 0, "a seed never recommends itself");
  int fromA = 0, fifthA = -1; for (int i = 0; i < n; i++) if (out[i].bestSeed == 0 && ++fromA == 5) { fifthA = i; break; }
  CHECK(find(out, n, 300) >= 0 && find(out, n, 300) < fifthA, "diversity: the other seed's pick (at %d) comes before the dominant seed's 5th (at %d)", find(out, n, 300), fifthA);
  CHECK(n >= 20, "with too few alternatives the row still fills up from the dominant seed (%d picks)", n);

  /* Because you watched Seed B, minus the billboard's first pick */
  int skip[1] = { recs_key(out[0].tmdb, 'm') }; RecCand byw[20];
  int nbw = recs_rank_seed(&s[1], s, 2, &p, skip, 1, byw, 20);
  CHECK(nbw == 1 && byw[0].tmdb == 300, "because-you-watched B: only B's own list, minus the billboard pick (%d)", nbw);

  /* language preference: same everything, Hindi picked */
  cJSON *lc = cJSON_CreateArray();
  cJSON_AddItemToArray(lc, ent(600, "English twin", "2026-03-01", "en", 18, 0, 7, 500, 0));
  cJSON_AddItemToArray(lc, ent(601, "Hindi twin", "2026-03-01", "hi", 18, 0, 7, 500, 0));
  RecCand cc[4]; int nc = recs_parse(lc, 'm', cc, 4);
  RecSeed s2 = { .tmdb = 700, .kind = 'm', .genres = { 18 }, .ng = 1, .lang = "hi", .w = 1, .list = cc, .n = nc };
  RecPrefs p2 = { .langs = { "hi" }, .nlangs = 1, .today = today };
  RecCand o2[4]; int n2 = recs_rank(&s2, 1, &p2, o2, 4);
  CHECK(n2 == 2 && o2[0].tmdb == 601, "a picked language wins a tie, even from lower in the list");

  /* Top 10: kept in order, dismissed left out */
  RecCand t10[RECS_LIST]; int nt = recs_filter(cb, nb, &p, t10, 10);
  CHECK(nt == 3 && t10[0].tmdb == 101 && t10[1].tmdb == 300, "Top 10 keeps the chart order, drops dismissed/adult/unreleased (%d)", nt);
  cJSON_Delete(la); cJSON_Delete(lb); cJSON_Delete(lc);
}

/* ---- real data saved by fetch.py ---- */
static cJSON *load(const char *dir, const char *name) {
  char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name); FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc((size_t)len + 1); if (!b) { fclose(f); return NULL; }
  size_t got = fread(b, 1, (size_t)len, f); b[got] = 0; fclose(f); cJSON *j = cJSON_Parse(b); free(b); return j;
}
static void print_list(const char *title, const RecCand *c, int n, const RecSeed *s) {
  printf("\n== %s ==\n", title);
  for (int i = 0; i < n; i++) printf("%2d. %-38.38s %s %s %4.2f%s%s\n", i + 1, c[i].title, c[i].lang, c[i].date, c[i].score,
                                     s && c[i].bestSeed >= 0 ? "  via " : "", s && c[i].bestSeed >= 0 ? s[c[i].bestSeed].title : "");
}
static int real(const char *dir) {
  cJSON *seeds = load(dir, "seeds.json"); if (!seeds) { printf("no %s/seeds.json: run fetch.py first\n", dir); return 1; }
  static RecCand lists[RECS_MAX_SEEDS][RECS_LIST], discs[RECS_MAX_SEEDS][RECS_LIST]; RecSeed s[RECS_MAX_SEEDS]; int ns = 0; int ex[RECS_MAX_SEEDS]; const cJSON *e;
  time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); char ymd[16]; strftime(ymd, sizeof ymd, "%Y-%m-%d", &tm);
  cJSON_ArrayForEach(e, seeds) {
    if (ns >= RECS_MAX_SEEDS) break;
    RecSeed *x = &s[ns]; memset(x, 0, sizeof *x);
    x->tmdb = cJSON_GetObjectItem(e, "tmdb")->valueint; x->kind = cJSON_GetObjectItem(e, "kind")->valuestring[0];
    snprintf(x->title, sizeof x->title, "%s", cJSON_GetObjectItem(e, "title")->valuestring);
    snprintf(x->lang, sizeof x->lang, "%s", cJSON_GetObjectItem(e, "lang")->valuestring);
    const cJSON *g; cJSON_ArrayForEach(g, cJSON_GetObjectItem(e, "genres")) if (x->ng < 8) x->genres[x->ng++] = g->valueint;
    x->w = recs_seed_weight(cJSON_GetObjectItem(e, "ageDays")->valuedouble, cJSON_GetObjectItem(e, "frac")->valuedouble,
                            cJSON_IsTrue(cJSON_GetObjectItem(e, "marked")), x->kind == 't', cJSON_GetObjectItem(e, "eps")->valueint);
    cJSON *r = load(dir, cJSON_GetObjectItem(e, "file")->valuestring);
    x->n = r ? recs_parse(cJSON_GetObjectItem(r, "results"), x->kind, lists[ns], RECS_LIST) : 0; x->list = lists[ns]; cJSON_Delete(r);
    cJSON *d = load(dir, cJSON_GetObjectItem(e, "disc")->valuestring);
    x->nd = d ? recs_parse(cJSON_GetObjectItem(d, "results"), x->kind, discs[ns], RECS_LIST) : 0; x->disc = discs[ns]; cJSON_Delete(d);
    ex[ns] = recs_key(x->tmdb, x->kind);
    printf("seed %-34.34s %s w=%.3f  %d recommended + %d same language/genre\n", x->title, x->lang, x->w, x->n, x->nd); ns++;
  }
  RecPrefs p = { .langs = { "hi", "en" }, .nlangs = 2, .exclude = ex, .nexclude = ns, .today = recs_days(ymd) };
  RecCand out[64]; int n = recs_rank(s, ns, &p, out, 64);
  print_list("Billboard (Top picks, first 8)", out, n < 8 ? n : 8, s);
  int best = 0; for (int i = 1; i < ns; i++) if (s[i].w > s[best].w) best = i;
  int skip[8]; for (int i = 0; i < 8 && i < n; i++) skip[i] = recs_key(out[i].tmdb, out[i].kind);
  RecCand bw[20]; int nbw = recs_rank_seed(&s[best], s, ns, &p, skip, n < 8 ? n : 8, bw, 20);
  char t[200]; snprintf(t, sizeof t, "Because you watched %s", s[best].title); print_list(t, bw, nbw, s);
  cJSON *lj = load(dir, "langs.json"); const cJSON *lv; int nl = 0; static RecCand per[8][RECS_LIST]; const RecCand *pl[8]; int pn[8];
  for (int k = 0; k < 2; k++) {
    nl = 0;
    cJSON_ArrayForEach(lv, lj) { if (nl >= 8) break; char f[64]; snprintf(f, sizeof f, "top_%s_%s.json", k ? "tv" : "movie", lv->valuestring);
      cJSON *r = load(dir, f); pn[nl] = r ? recs_parse(cJSON_GetObjectItem(r, "results"), k ? 't' : 'm', per[nl], RECS_LIST) : 0; pl[nl] = per[nl]; nl++; cJSON_Delete(r); }
    RecPrefs pt = p; pt.nexclude = 0; RecCand top[10];
    print_list(k ? "Top 10 series this week" : "Top 10 movies this week", top, recs_interleave(pl, pn, nl, &pt, top, 10), NULL);
  }
  cJSON_Delete(lj);
  cJSON_Delete(seeds); return 0;
}

int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "--real")) return real(argv[2]);
  synthetic();
  printf("\n%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
  return fails ? 1 : 0;
}
