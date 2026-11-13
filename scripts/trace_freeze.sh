#!/usr/bin/env bash
# Freeze diagnostics for winvm on DGX Spark.
#
#   ./scripts/trace_freeze.sh run      Start the VM (4 vCPUs, 4608 MB) recording every host
#                                      fsync/fdatasync with timestamps and durations, plus --stats.
#   ./scripts/trace_freeze.sh bt       While it is frozen (in another terminal): dump every thread's
#                                      backtrace (asks for your sudo password).
#   ./scripts/trace_freeze.sh report   After the run: show the slowest flushes and the stats gaps.
#
# Output goes to work/: fsync.log, winvm_stats3.log, freeze_bt.txt.
# Extra arguments after "run" are passed to launch_spark.sh (e.g. --install).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$ROOT_DIR/work"
FSYNC_LOG="$WORK/fsync.log"
STATS_LOG="$WORK/winvm_stats3.log"
BT_LOG="$WORK/freeze_bt.txt"

cmd="${1:-}"
[[ $# -gt 0 ]] && shift

case "$cmd" in
  run)
    if pgrep -x winvm >/dev/null; then
      echo "Error: winvm is already running (pid $(pgrep -x winvm | tr '\n' ' ')). Stop it first." >&2
      exit 1
    fi
    echo "Recording flushes to $FSYNC_LOG and stats to $STATS_LOG"
    echo "If it freezes: note the time ($(date +%T) now), then run: $0 bt"
    # strace launches the VM as its own child: no sudo needed. --seccomp-bpf stops only the traced
    # syscalls, so the VM runs at nearly full speed.
    CPUS="${CPUS:-4}" RAM="${RAM:-4608}" \
      strace -f --seccomp-bpf -tt -T -e trace=fsync,fdatasync -o "$FSYNC_LOG" \
      "$ROOT_DIR/scripts/launch_spark.sh" --stats 5 "$@" 2>&1 | tee "$STATS_LOG"
    ;;

  bt)
    pid="$(pgrep -x winvm | head -1 || true)"
    if [[ -z "$pid" ]]; then
      echo "Error: winvm is not running." >&2
      exit 1
    fi
    echo "Capturing winvm (pid $pid) thread state at $(date +%T) -> $BT_LOG"
    # Per-thread state from /proc needs no debugger: R = running (busy loop), S/D = blocked (wchan says where).
    thread_states() {
      for t in /proc/"$pid"/task/*; do
        set -- $(sed 's/.*) //' "$t/stat")
        printf "%s state=%s user_ticks=%s host_cpu=%s wchan=%s\n" "$(basename "$t")" "$1" "${12}" "${37}" "$(cat "$t/wchan" 2>/dev/null)"
      done | sort -n
    }
    {
      echo "# captured at $(date '+%F %T')"
      echo "## thread states"
      thread_states
      sleep 2
      echo "## 2 s later (user_ticks rising by ~200 = that thread is spinning at 100%)"
      thread_states
      # Only one tracer may attach: under 'run' that is strace, so gdb can't. Skip it then.
      tracer="$(awk '/^TracerPid:/ {print $2}' /proc/"$pid"/status)"
      if [[ "$tracer" == "0" ]]; then
        echo "## gdb backtraces"
        sudo gdb -p "$pid" -batch -ex "thread apply all bt"
      else
        echo "## gdb skipped: winvm is already traced by pid $tracer (strace)"
      fi
    } > "$BT_LOG" 2>&1
    echo "Done. Recent fault-loop reports:"
    grep -A12 "FAULT LOOP" "$STATS_LOG" 2>/dev/null | tail -40 || true
    ;;

  report)
    if [[ ! -s "$FSYNC_LOG" ]]; then
      echo "No $FSYNC_LOG yet: run '$0 run' first." >&2
      exit 1
    fi
    total=$(grep -cE 'fsync|fdatasync' "$FSYNC_LOG" || true)
    echo "== $total flush calls; the 20 slowest (seconds, wall-clock time):"
    grep -E '<[0-9.]+>' "$FSYNC_LOG" \
      | sed -E 's/^[0-9]+ +([0-9:.]+) .*<([0-9.]+)>$/\2 \1/' \
      | sort -n | tail -20 | awk '{printf "  %8.3fs  at %s\n", $1, $2}'
    echo
    echo "== flushes longer than 0.5 s: $(grep -oE '<[0-9.]+>' "$FSYNC_LOG" | tr -d '<>' | awk '$1>0.5' | wc -l)"
    if [[ -s "$STATS_LOG" ]]; then
      echo
      echo "== --stats gaps longer than 8 s (CPU 0 was blocked or starved):"
      grep -E '^stats' "$STATS_LOG" | sed -E 's/^stats +([0-9]+)s:.*/\1/' \
        | awk 'NR>1 && $1-p>8 {printf "  %ds gap before t=%ds\n", $1-p, $1} {p=$1}'
    fi
    ;;

  *)
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
