#!/usr/bin/env bash
# Play one contiguous slice of the batch: a whole shard from `match`, a part of a lost one from `repair`.
# Staged in the toolkit rather than inlined twice, so a replay cannot drift from the shard it replaces,
# and because shards deliberately do not check out the repository.
#
# Input is the environment: SHARD (a label), START (1-based opening), ROUNDS, CANDIDATE_TC, REFERENCE_TC,
# MATCH_CONCURRENCY, THREADS, CANDIDATE_UCI_OPTIONS, REFERENCE_UCI_OPTIONS, ARM_LABEL, ARM_OPTIONS,
# CAND_NAME, REF_NAME, BOOK_FILE, RUNNER_TEMP and GITHUB_OUTPUT.
set -euo pipefail
tools="$RUNNER_TEMP/tools"
work="$RUNNER_TEMP/match"
# Artifact download does not preserve the executable bit.
chmod +x "$tools/fastchess" "$tools/candidate" "$tools/reference"
# Separate working directories per engine: the engine writes logs
# relative to its cwd, and spdlog fails silently when logs/ is absent.
mkdir -p "$work/cand/logs" "$work/ref/logs"

echo "Shard $SHARD: $ROUNDS pairs from opening $START"

# In a multi-arm run this shard plays one arm, named in the engine
# name so the log and PGN headers identify it.
if [ -n "$ARM_LABEL" ]; then
  CANDIDATE_UCI_OPTIONS=$ARM_OPTIONS
  CAND_NAME="$CAND_NAME-arm$ARM_LABEL"
  echo "Shard $SHARD: arm $ARM_LABEL, options $ARM_OPTIONS"
fi

# compute_budget() bounds a move by the clock (issue #204), but an
# increment under 100 ms still leaves the engine playing near-instant
# moves at the bottom of its clock. A control run should halve the
# BASE time and leave the increment alone; halving both measures the
# time manager rather than the change under test.
for tc in "$CANDIDATE_TC" "$REFERENCE_TC"; do
  inc=${tc#*+}
  if [ "$inc" = "$tc" ]; then inc=0; fi
  if awk -v i="$inc" 'BEGIN { exit !(i < 0.1) }'; then
    echo "::warning::Time control $tc has an increment below 0.1 s. Expect very fast play at low clock, and check the batch for time losses."
  fi
done

# Per-engine UCI options, already checked against each engine's own
# advertised table in `build`. Built as arrays so an empty request
# expands to nothing at all rather than to an empty argument, and so
# no value is ever re-split by the shell.
cand_opts=()
ref_opts=()
if [ -n "$CANDIDATE_UCI_OPTIONS" ]; then
  read -r -a cand_kv <<< "$CANDIDATE_UCI_OPTIONS"
  for kv in "${cand_kv[@]}"; do cand_opts+=("option.$kv"); done
fi
if [ -n "$REFERENCE_UCI_OPTIONS" ]; then
  read -r -a ref_kv <<< "$REFERENCE_UCI_OPTIONS"
  for kv in "${ref_kv[@]}"; do ref_opts+=("option.$kv"); done
fi

cd "$work"
# Evidence of how busy the runner was; the aggregate summarises it.
# Detached from the step's output, which an orphaned sleep would hold open.
bash "$tools/cpu_sampler.sh" 60 cpu.tsv > /dev/null 2>&1 &
sampler_pid=$!
trap 'kill "$sampler_pid" 2>/dev/null || true' EXIT

# Adjudication settings are copied from Run-EloMatch.ps1 verbatim: the
# two instruments must differ only in toolchain and hardware.
# The same Threads on both sides -- Lazy SMP scales with core count, so
# unequal counts measure the hardware split, not the change.
start_s=$SECONDS
set +e
"$tools/fastchess" \
  -engine cmd="$tools/candidate" name="$CAND_NAME" dir=cand args=uci "option.Threads=$THREADS" ${cand_opts[@]+"${cand_opts[@]}"} "tc=$CANDIDATE_TC" \
  -engine cmd="$tools/reference" name="$REF_NAME" dir=ref args=uci "option.Threads=$THREADS" ${ref_opts[@]+"${ref_opts[@]}"} "tc=$REFERENCE_TC" \
  -each proto=uci \
  -rounds "$ROUNDS" -repeat -concurrency "$MATCH_CONCURRENCY" -recover \
  -openings file="$tools/$BOOK_FILE" format=epd order=sequential start="$START" \
  -draw movenumber=40 movecount=8 score=10 \
  -resign movecount=4 score=800 \
  -pgnout file=match.pgn notation=san \
  2>&1 | tee match.log
fc_exit=${PIPESTATUS[0]}
set -e
echo "fastchess exit: $fc_exit"
echo "elapsed_min=$(( (SECONDS - start_s) / 60 ))" >> "$GITHUB_OUTPUT"

# Both scans below key on fastchess's own wordings rather than on
# keywords like "illegal", which also occur in engine output echoed
# into the log. The alternations are the complete set of PV-compliance
# and engine-failure messages fastchess emits.
#
# A PV-compliance warning says the engine's reported `pv` is
# defective. fastchess plays the game on from `bestmove`, so the
# result stands and the batch stays measurable. Reported rather than
# fatal: a PV that does not replay is worthless as evidence, but that
# is no reason to discard 1116 sound games.
pv_warn_re='Warning; (Illegal PV move|PV continues after (checkmate|stalemate|threefold repetition|fifty-move rule)|Incomplete mating PV|Mating PV does not end|Too long mating PV|Bestmove does not match beginning of last PV|Sign mismatch in mate scores)'
pv_warnings=$(grep -cE "$pv_warn_re" match.log || true)
if [ "$pv_warnings" -gt 0 ]; then
  echo "::warning::Shard $SHARD: $pv_warnings PV-compliance warnings -- reported PV only, games unaffected."
  grep -oE "$pv_warn_re" match.log | sort | uniq -c | sort -rn
fi

# A time loss, an illegal or malformed move PLAYED, or an engine that
# disconnects or stops answering is a harness or engine BUG, not
# strength data. On a shared runner it most likely means the box was
# oversubscribed, which invalidates every game in the shard, not just
# the one that lost. Fail the shard; the aggregator then refuses to
# pool the batch at all.
fatal_re='Illegal move .* played by|Move does not match uci move format|disconnects|connection stalls|Engine .* stalls|loses on time|stalled / disconnected|is not responsive|No output from|respond to uci'
if grep -qE "$fatal_re" match.log; then
  echo "::error::Shard $SHARD: harness/engine failures detected -- batch is not measurable."
  grep -E "$fatal_re" match.log | head -10
  exit 1
fi

# Anything else fastchess flagged is unclassified. Surfaced so that a
# wording added by a future release cannot pass silently as neither
# tolerated nor fatal -- the failure mode this accounting is built to
# avoid.
if grep -E 'Warning;|Error;' match.log | grep -qvE "$pv_warn_re|$fatal_re"; then
  echo "::warning::Shard $SHARD: unclassified fastchess diagnostics:"
  grep -E 'Warning;|Error;' match.log | grep -vE "$pv_warn_re|$fatal_re" \
    | sort | uniq -c | sort -rn | head -10
fi
test "$fc_exit" = "0"
