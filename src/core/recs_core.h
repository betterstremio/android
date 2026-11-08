/* The recommendation maths, with no app dependencies (cJSON only), so tests/recs can run it on fixtures.
   recs.c feeds it: the watched titles (seeds) with their TMDB ids, genres and language, each seed's
   TMDB /recommendations list, and the region's popular lists; it gives back ranked candidates.

   Score of a candidate c (all parts 0..1, weights in RecWeights):
     seed    sum over the seeds s listing c: w(s) * (1 - rank(c in s)/20), divided by the sum of all w(s)
     genre   how much of c's genres the user watches (the taste vector, normalised)
     lang    half for a language the user picked, half for the share of their watching in that language
     quality vote_average/10, damped below 200 votes
     fresh   1 for a title out today, falling to 0 at two years; old: a penalty growing past 8 years
   A seed's weight w = recency (half-life RECS_HALF_LIFE_D days) x completion (1 if >= 70% watched or
   marked watched, 0.5 for 20-70%, 0 under 20%), x1.3 for a series with 3+ episodes watched. */
#ifndef SB_RECS_CORE_H
#define SB_RECS_CORE_H
#include "cJSON.h"

#define RECS_MAX_SEEDS 15
#define RECS_LIST 20          /* one /recommendations page */
#define RECS_MAX_CAND 400
#define RECS_GENRES 64        /* TMDB genre ids are < 11000; they are folded into this many slots */
#define RECS_HALF_LIFE_D 21.0
#define RECS_ROW_FLOOR 0.72     /* a because-you-watched row stops (after 8) below this share of its best score */

typedef struct {
  int tmdb; char kind;                /* 'm' movie, 't' tv */
  char title[128], poster[64], backdrop[64], date[12], lang[4], overview[600];
  int genres[8], ng, adult;
  float vote; int votes; float pop;
  /* filled by recs_rank */
  double score, seedPart; int bestSeed; int nSeeds;
} RecCand;

typedef struct {
  int tmdb; char kind; char title[128];
  int genres[8], ng; char lang[4];
  double w;                           /* recs_seed_weight */
  const RecCand *list; int n;         /* its /recommendations, in TMDB's order */
  const RecCand *disc; int nd;        /* popular titles in its language and main genre (/discover): TMDB's
                                         recommendations for Indian titles are mostly Western, this list is not */
} RecSeed;

typedef struct { double seed, genre, lang, quality, fresh, old, disc; } RecWeights;
extern const RecWeights RECS_W;       /* see recs_core.c */

typedef struct {
  char langs[8][4]; int nlangs;       /* the languages picked in Settings ("hi", "en") */
  const int *exclude; int nexclude;   /* TMDB ids (kind folded in: recs_key) already watched or dismissed */
  long today;                         /* days since 1970, for "fresh" */
  int allowAdult;
} RecPrefs;

/* a seed's weight from what the library says */
double recs_seed_weight(double ageDays, double fracWatched, int markedWatched, int isSeries, int episodesWatched);
/* one key for a TMDB id and its kind (movie and tv ids overlap) */
int  recs_key(int tmdb, char kind);
/* a TMDB result array (/recommendations, /similar, /discover, /trending) into candidates; kind 0 = read
   "media_type" per entry. Returns the count kept (people and entries without art are skipped). */
int  recs_parse(const cJSON *results, char kind, RecCand *out, int max);
/* "2026-03-14" -> days since 1970 (0 when unreadable) */
long recs_days(const char *ymd);

/* merges the seeds' lists, scores, drops excluded / adult / duplicate titles, and sorts best first.
   Diversity: within the first 20, at most 4 come from the same best seed. Returns the count in out. */
int  recs_rank(const RecSeed *seeds, int ns, const RecPrefs *p, RecCand *out, int max);
/* "Because you watched <seed>": that seed's own list, scored the same way, minus the ids in skip
   (e.g. the billboard's). */
int  recs_rank_seed(const RecSeed *seed, const RecSeed *all, int ns, const RecPrefs *p, const int *skip, int nskip, RecCand *out, int max);
/* a popular list (Top 10) filtered by the prefs' exclusions and adult flag, kept in its own order */
int  recs_filter(const RecCand *in, int n, const RecPrefs *p, RecCand *out, int max);
/* Top 10 from one popular list per language (main language first): taken in turn, so the main language
   is not drowned by globally popular English titles; duplicates and excluded titles are skipped */
int  recs_interleave(const RecCand *const *lists, const int *n, int nlists, const RecPrefs *p, RecCand *out, int max);
#endif
