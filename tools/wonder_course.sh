#!/usr/bin/env bash
# The scripted course run on Linux (tools/wonder_course.ps1 + tools/wonder_run.ps1): a new game from the title to
# course 1-1, frames every DUMP presents, then a verdict from the log.
#
#   tools/wonder_course.sh <name> [seconds] [--windowed] [--course11] [--delay N] [--dump N] [--keep-save]
#
# Writes private/wonder/runs/<name>/: nx.log, stderr.txt, frames/*.png, summary.txt. Exit code 0 when the run
# passes: no crash or GPU hang, 300+ presents, peak memory under 6 GB. Headless (NX_HEADLESS=1 NX_MUTE=1) unless
# --windowed; a windowed run needs a desktop session (over SSH: XDG_RUNTIME_DIR and WAYLAND_DISPLAY or DISPLAY).
set -u
cd "$(dirname "$0")/.."
NAME=${1:?usage: tools/wonder_course.sh <name> [seconds] [--windowed] [--course11] [--delay N] [--dump N]}
shift
SECONDS_RUN=180
if [[ ${1:-} =~ ^[0-9]+$ ]]; then SECONDS_RUN=$1; shift; fi
WINDOWED=0 COURSE11=0 DELAY=0 DUMP=600 KEEP=0
while [ $# -gt 0 ]; do
    case $1 in
    --windowed) WINDOWED=1 ;;
    --course11) COURSE11=1 ;;
    --delay) DELAY=$2; shift ;;
    --dump) DUMP=$2; shift ;;
    --keep-save) KEEP=1 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
EXE=${EXE:-private/wonder/build/wonder}
if pgrep -x wonder >/dev/null; then echo "a wonder process is already running" >&2; exit 2; fi

# The timeline (seconds after the first pad read), as in wonder_course.ps1.
S="20:L:0.4;20:R:0.4"
for t in $(seq 26 3 50); do S="$S;$((t + DELAY)):A:0.25"; done
WALK=$((55 + DELAY))
if [ $COURSE11 = 1 ]; then
    STOP=$((140 + DELAY))
    S="$S;$WALK:RIGHT:$((STOP - WALK))"
    for t in $(seq $((WALK + 1)) 1.5 $((STOP - 1))); do S="$S;$t:A:0.35"; done
    for t in $(seq $((STOP + 5)) 3 $((200 + DELAY))); do S="$S;$t:A:0.25"; done
    WALK=$((205 + DELAY))
fi
S="$S;$WALK:RIGHT:$((SECONDS_RUN - WALK))"
for t in $(seq $((WALK + 1)) 1.5 $((SECONDS_RUN - 1))); do S="$S;$t:A:0.35"; done

RUN=private/wonder/runs/$NAME
rm -rf "$RUN" private/wonder/frames
mkdir -p "$RUN/frames"
[ $KEEP = 1 ] || rm -rf private/wonder/user/save

export NX_LOG=$RUN/nx.log NX_MEM_LIMIT_GB=8 NX_INPUT_SCRIPT=$S NX_DUMP_FRAMES=$DUMP
[ $WINDOWED = 1 ] || export NX_HEADLESS=1 NX_MUTE=1
T0=$(date +%s)
timeout --signal=KILL "$SECONDS_RUN" "$EXE" 2>"$RUN/stderr.txt"
CODE=$?
[ $CODE = 124 ] || [ $CODE = 137 ] && CODE="killed after $SECONDS_RUN s (expected)"

if [ -d private/wonder/frames ]; then
    PY=private/venv/bin/python
    [ -x $PY ] || PY=python3
    $PY tools/frames.py png private/wonder/frames "$RUN/frames" >/dev/null 2>&1 || cp private/wonder/frames/* "$RUN/frames/"
    rm -rf private/wonder/frames
fi

LOG=$RUN/nx.log
FPS=$(grep -oP 'per frame over \d+: \K[\d.]+(?= fps)' "$LOG" 2>/dev/null | awk '$1 > 0')
N=$(echo "$FPS" | grep -c . || true)
MEDIAN=$(echo "$FPS" | sort -n | awk '{a[NR]=$1} END {print NR ? a[int(NR/2)+1] : 0}')
PEAK=$(grep -oP 'memory: [\d.]+ GB \(peak \K[\d.]+' "$LOG" 2>/dev/null | sort -n | tail -1)
CRASH=$(grep -m1 -E 'CRASH|GPU hang|fatal|over the .* limit' "$LOG" 2>/dev/null)
FAIL=""
[ -n "$CRASH" ] && FAIL="$FAIL crash/hang: $CRASH;"
[ $((60 * N)) -lt 300 ] && FAIL="$FAIL presents $((60 * N)) < 300;"
awk -v p="${PEAK:-0}" 'BEGIN {exit !(p > 6)}' && FAIL="$FAIL peak memory $PEAK GB > 6;"
{
    echo "run $NAME: $(($(date +%s) - T0)) s, exit $CODE"
    grep -m1 'nvn/vk: using GPU' "$LOG"
    grep -m1 'nvn/vk: present mode' "$LOG"
    echo "presents ~$((60 * N)), median fps ${MEDIAN:-0}, peak memory ${PEAK:-?} GB"
    echo "frames: $(ls "$RUN/frames" | wc -l)"
    if [ -n "$FAIL" ]; then echo "FAIL:$FAIL"; else echo PASS; fi
} | tee "$RUN/summary.txt"
[ -z "$FAIL" ]
