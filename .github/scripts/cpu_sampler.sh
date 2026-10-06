#!/usr/bin/env bash
# Sample the runner's CPU use while a strength-lab shard plays.
#
# Usage: cpu_sampler.sh <interval-seconds> <output.tsv>
#
# Every interval, appends one row: seconds since start, busy and steal percent
# across all vCPUs, busy percent per vCPU, and the 1-minute load average. Busy
# excludes idle, iowait and steal; steal is time the hypervisor gave to another
# guest, which a busy shared host shows. Runs until killed.
set -euo pipefail

interval=$1
out=$2

# One line per cpu row of /proc/stat: name, total, idle+iowait, steal. guest and
# guest_nice are already counted in user, so the total stops at steal.
snapshot() {
  awk '/^cpu/ { total = 0; for (i = 2; i <= 9 && i <= NF; i++) total += $i
                print $1, total, $5 + $6, $9 + 0 }' /proc/stat
}

printf 'elapsed_s\tbusy_pct\tsteal_pct\tper_cpu_busy_pct\tload_1m\n' > "$out"
start=$SECONDS
prev=$(snapshot)
while sleep "$interval"; do
  cur=$(snapshot)
  read -r load _ < /proc/loadavg
  paste -d' ' <(printf '%s\n' "$prev") <(printf '%s\n' "$cur") | awk \
    -v elapsed=$(( SECONDS - start )) -v load1="$load" '
    { dt = $6 - $2; di = $7 - $3; ds = $8 - $4
      busy = dt > 0 ? 100 * (dt - di - ds) / dt : 0
      if ($1 == "cpu") { all = busy; steal = dt > 0 ? 100 * ds / dt : 0 }
      else per = per (per == "" ? "" : ",") sprintf("%.0f", busy) }
    END { printf "%d\t%.1f\t%.1f\t%s\t%s\n", elapsed, all, steal, per, load1 }' >> "$out"
  prev=$cur
done
