#!/bin/bash
# Test media for tools/android/test/run.py, made once in $AB/testmedia (Linux or WSL, needs ffmpeg and python3-libtorrent):
#   clip.mp4  90 s, 1280x720 H.264 (moving test pattern, bright: a screenshot shows whether the picture is on screen) + AAC stereo
#   clip.mkv  the same picture, two audio tracks (AAC English, AC3 5.1 French) and English subtitles (SRT)
#   hls/      the clip as HLS
#   clip.torrent  clip.mkv as a torrent (its info hash in clip.ih), for the P2P test's own seeder
set -euo pipefail
AB="${AB:-$HOME/abuild}"; M="$AB/testmedia"; mkdir -p "$M"
cd "$M"
if [ ! -f clip.mp4 ]; then
  ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=90" -f lavfi -i "sine=frequency=440:duration=90:sample_rate=48000" \
    -vf "drawtext=text='%{pts\:hms}':fontsize=64:fontcolor=white:box=1:boxcolor=black@0.6:x=40:y=40" \
    -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 48 -c:a aac -b:a 128k -ac 2 -movflags +faststart clip.mp4
fi
if [ ! -f clip.mkv ]; then
  printf '1\n00:00:01,000 --> 00:00:30,000\nSubtitle line one\n\n2\n00:00:30,000 --> 00:01:20,000\nSubtitle line two\n' > clip.srt
  ffmpeg -loglevel error -y -i clip.mp4 -f lavfi -i "sine=frequency=660:duration=90:sample_rate=48000" -i clip.srt \
    -map 0:v -map 0:a -map 1:a -map 2:s -c:v copy -c:a:0 copy -c:a:1 ac3 -ac:a:1 6 -b:a:1 384k -c:s srt \
    -metadata:s:a:0 language=eng -metadata:s:a:0 title=English -metadata:s:a:1 language=fre -metadata:s:a:1 title="French 5.1" \
    -metadata:s:s:0 language=eng -metadata:s:s:0 title=English clip.mkv
fi
if [ ! -f clip-eac3.mkv ]; then   # the sound only as E-AC-3 (Dolby Digital Plus), which some Android devices (and the emulator) can't decode
  ffmpeg -loglevel error -y -i clip.mp4 -map 0:v -map 0:a -c:v copy -c:a eac3 -b:a 256k clip-eac3.mkv
fi
if [ ! -f hls/master.m3u8 ]; then
  mkdir -p hls; ffmpeg -loglevel error -y -i clip.mp4 -c copy -f hls -hls_time 4 -hls_playlist_type vod -hls_segment_filename 'hls/seg%03d.ts' hls/master.m3u8
fi
if [ ! -f clip.torrent ]; then
  python3 - <<'PY'
import libtorrent as lt
fs = lt.file_storage(); lt.add_files(fs, 'clip.mkv')
t = lt.create_torrent(fs, piece_size=256 * 1024, flags=lt.create_torrent.v1_only)   # v1 only, as almost every real torrent
t.set_creator('stremio-origin test')
lt.set_piece_hashes(t, '.')
open('clip.torrent', 'wb').write(lt.bencode(t.generate()))
info = lt.torrent_info('clip.torrent'); open('clip.ih', 'w').write(str(info.info_hash()))
print('torrent', info.info_hash(), info.total_size())
PY
fi
ls -l "$M" | awk 'NR>1 {print "  " $NF, $5}'; echo "ih $(cat clip.ih)"
echo MEDIA_OK
