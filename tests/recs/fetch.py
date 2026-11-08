#!/usr/bin/env python3
"""tests/recs/fetch.py DIR [--key-file PATH] [--region IN] [--langs hi,en] [--library FILE]

Asks TMDB for what the app's recommendation engine would ask for a sample library, the same way
(src/core/recs.c), and saves the answers in DIR for `tests/recs/run.sh --real DIR`. Every answer is cached
in DIR/http, so a second run shows the warm-cache request count. The key is read from the file and never
printed or written anywhere else."""
import argparse, datetime, hashlib, json, os, sys, urllib.parse, urllib.request

SAMPLE = [  # imdb id, type, days since watched, fraction watched, marked watched, episodes watched
    ("tt2181931", "movie", 2, 1.00, False, 0),    # English Vinglish
    ("tt12004706", "series", 5, 0.60, False, 8),  # Panchayat
    ("tt1187043", "movie", 12, 0.95, False, 0),   # 3 Idiots
    ("tt9432978", "series", 20, 0.30, False, 3),  # Kota Factory
    ("tt0816692", "movie", 35, 1.00, True, 0),    # Interstellar
    ("tt10530900", "series", 50, 0.80, False, 5), # Gullak
    ("tt0110413", "movie", 90, 0.15, False, 0),   # Leon (barely started: ignored)
]

ap = argparse.ArgumentParser()
ap.add_argument("dir"); ap.add_argument("--key-file", default=r"C:\Users\sagar\so\.tmdb-key")
ap.add_argument("--region", default="IN"); ap.add_argument("--langs", default="hi,en"); ap.add_argument("--library")
a = ap.parse_args()
kf = a.key_file
if not os.path.exists(kf) and kf.startswith("C:\\"): kf = "/mnt/c/" + kf[3:].replace("\\", "/")
key = open(kf, encoding="utf-8").read().strip()
bearer = len(key) > 40
os.makedirs(os.path.join(a.dir, "http"), exist_ok=True)
stats = {"network": 0, "cached": 0}

def get(path):
    """GET api.themoviedb.org/3<path>, cached in DIR/http by path (the key is not part of the name)"""
    name = hashlib.sha1(path.encode()).hexdigest()[:16] + ".json"
    cp = os.path.join(a.dir, "http", name)
    if os.path.exists(cp):
        stats["cached"] += 1
        return json.load(open(cp, encoding="utf-8"))
    url = "https://api.themoviedb.org/3" + path
    hdr = {"User-Agent": "recs-test/1.0", "Accept": "application/json"}
    if bearer: hdr["Authorization"] = "Bearer " + key
    else: url += ("&" if "?" in path else "?") + "api_key=" + urllib.parse.quote(key)
    stats["network"] += 1
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=hdr), timeout=20) as r: j = json.load(r)
    except Exception as e:
        print("request failed:", path, type(e).__name__, getattr(e, "code", ""), file=sys.stderr); return {}
    json.dump(j, open(cp, "w", encoding="utf-8"))
    return j

lib = SAMPLE if not a.library else [tuple(x) for x in json.load(open(a.library, encoding="utf-8"))]
seeds = []
for imdb, typ, age, frac, marked, eps in lib:
    f = get(f"/find/{imdb}?external_source=imdb_id")
    res = (f.get("movie_results") or []) if typ == "movie" else (f.get("tv_results") or [])
    if not res: print("not on TMDB:", imdb); continue
    r = res[0]; kind = "m" if typ == "movie" else "t"; seg = "movie" if kind == "m" else "tv"
    rec = get(f"/{seg}/{r['id']}/recommendations?page=1")
    if len(rec.get("results") or []) < 5:  # thin list: the similar titles instead
        rec = get(f"/{seg}/{r['id']}/similar?page=1")
    fn = f"rec_{kind}{r['id']}.json"; json.dump(rec, open(os.path.join(a.dir, fn), "w", encoding="utf-8"))
    # its language + main genre, popular, out in the last 10 years, with enough votes to be real
    g = (r.get("genre_ids") or [0])[0]; lang = r.get("original_language", "")
    datek = "primary_release_date" if kind == "m" else "first_air_date"
    disc = get(f"/discover/{seg}?with_original_language={lang}&with_genres={g}&sort_by=popularity.desc&vote_count.gte=20"
               f"&{datek}.gte={datetime.date.today().year - 10}-01-01")
    dn = f"disc_{kind}{r['id']}.json"; json.dump(disc, open(os.path.join(a.dir, dn), "w", encoding="utf-8"))
    seeds.append({"imdb": imdb, "tmdb": r["id"], "kind": kind, "title": r.get("title") or r.get("name") or imdb,
                  "lang": r.get("original_language", ""), "genres": r.get("genre_ids", []),
                  "ageDays": age, "frac": frac, "marked": marked, "eps": eps, "file": fn, "disc": dn})
json.dump(seeds, open(os.path.join(a.dir, "seeds.json"), "w", encoding="utf-8"), indent=1)

# Top 10: per language (the app's languages, first = main), streaming in the region, recent only; the test
# interleaves them so the main language is not drowned by globally popular English titles
today = datetime.date.today()
common = f"watch_region={a.region}&with_watch_monetization_types=flatrate|free|ads&sort_by=popularity.desc"
for lang in a.langs.split(","):
    m = get(f"/discover/movie?{common}&with_original_language={lang}&primary_release_date.gte={today - datetime.timedelta(days=180)}&primary_release_date.lte={today}")
    t = get(f"/discover/tv?{common}&with_original_language={lang}&first_air_date.gte={today - datetime.timedelta(days=730)}&air_date.gte={today - datetime.timedelta(days=30)}")
    json.dump(m, open(os.path.join(a.dir, f"top_movie_{lang}.json"), "w", encoding="utf-8"))
    json.dump(t, open(os.path.join(a.dir, f"top_tv_{lang}.json"), "w", encoding="utf-8"))
json.dump(a.langs.split(","), open(os.path.join(a.dir, "langs.json"), "w", encoding="utf-8"))

shown = 8 + 20 + 10 + 10  # billboard + one because-you-watched row + two Top 10 rows
print(f"\nseeds: {len(seeds)}   requests: {stats['network']} over the network, {stats['cached']} from the cache")
print(f"(the app also asks /external_ids for each shown card without an IMDb id: at most {shown} the first time, cached forever)")
