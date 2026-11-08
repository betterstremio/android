/* "For You": recommendations from the watch history, with the user's own TMDB key (recs_core.c does the maths).
   Main thread only, never blocks on the network: recs_tick() runs a small request queue (3 at a time, started
   a few seconds after launch so Home's own rows go first), TMDB answers are cached on disk (lists 24 h, Top 10
   12 h, id lookups for good) and the finished lists are kept too, so Home shows them at once on the next launch.
   Without a TMDB key, or with Settings > Recommendations off, everything here is empty and nothing is asked. */
#ifndef SB_RECS_H
#define SB_RECS_H
#include "core/stremio.h"

void     recs_tick(void);
unsigned recs_version(void);              /* bumps when the lists below change: Home rebuilds */
/* copies (free with itemlist_clear); 0 when there is nothing */
int  recs_billboard(ItemList *out);       /* Top picks: the rotating billboard, 8 at most */
int  recs_because(ItemList *out, char *title, size_t cap);   /* "Because you watched <title>" */
int  recs_top10(int series, ItemList *out, char *title, size_t cap); /* "Top 10 movies in India this week" */
int  recs_rank_of(const Item *it);        /* 1-10 in a Top 10 row (for its numeral), else 0 */
int  recs_badges(const Item *it);         /* RECS_NEW | RECS_LOVE: the wide card's badges */
enum { RECS_NEW = 1, RECS_LOVE = 2 };
void recs_dismiss(const Item *it);        /* "Not interested": hidden and kept out from now on */

/* settings (kept on this device: the Stremio account has nowhere for them) */
int  recs_on(void);
void recs_set_on(int on);
int  recs_paused(void);                   /* "Pause history": what is watched from now on is not learnt from */
void recs_set_paused(int on);
int  recs_hide_cinema(void);             /* "Hide films still in cinemas": films out less than 30 days ago are left out (off by default:
                                            many go straight to streaming) */
void recs_set_hide_cinema(int on);
const char *recs_region(void);            /* "IN": the TV's country unless chosen */
void recs_set_region(const char *cc);
const char *recs_langs(void);             /* "hi,en": the region's usual languages unless chosen */
void recs_set_langs(const char *csv);
const char *recs_region_name(const char *cc);   /* "India" */
void recs_refresh(void);                  /* ask again now (a setting changed) */
#endif
