#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds a Switch 2 title's precompiled pipeline file (precompiled.fork-v<N>.nx2prep) from the
# game's own files and the Lichii disk caches recorded while playing it, and its recipe to share.
#
# usage: run_pipeline.sh CACHE.bin [CACHE.bin...]
#
# A CACHE is a title's shader cache, cache/shader/<title id>/lichii-cache.v2.bin, or a pipeline
# cache file of the format before it (vulkan.fork-v<N>.bin); a v2 log is converted first.
#
# Environment:
#   NX2_GAME     the game: a .nup file (base and update), or an unpacked RomFS folder
#   NX2_OUT      output folder (default ./nx2-precompile-out)
#   NX2_RECIPE   the recipe to write (default $NX2_OUT/game.nx2recipe)
#   NX2_TOOL     nx2-precompile executable (default: nx2-precompile on PATH)
#   NX2_K        predicted render states per vertex/pixel pair (default 4)
#   NX2_THREADS  workers for native commands (default 4)
#   NX2_ISOLATED_SCAN  1 bounds scan subprocesses, 0 scans directly (default auto: on for POSIX)
#   NX2_SCAN_WORKERS, NX2_SCAN_MEMORY_MIB, NX2_SCAN_TIMEOUT  isolated scan limits (2, 1536, 20)
#   PYTHON       Python 3.11+ (default python)
set -euo pipefail

: "${NX2_GAME:?set NX2_GAME to the game (.nup or an unpacked RomFS folder)}"
[ $# -ge 1 ] || { echo "usage: $0 CACHE.bin [CACHE.bin...]" >&2; exit 2; }
OUT=${NX2_OUT:-$PWD/nx2-precompile-out}
TOOL=${NX2_TOOL:-nx2-precompile}
K=${NX2_K:-4}
THREADS=${NX2_THREADS:-4}
VERSION=$("$TOOL" cache-version) # the file name's v<N>; the Lichii build reading it must match
PREP=$OUT/precompiled.fork-v$VERSION.nx2prep
RECIPE=${NX2_RECIPE:-$OUT/game.nx2recipe}
PYTHON=${PYTHON:-python}
PY=$(cd "$(dirname "$0")" && pwd)/py
PROGRAMS=$OUT/game.programs
PRIORS=$OUT/priors.json
mkdir -p "$OUT"

# Shader cache v2 logs (magic LXSHCACH) become pipeline cache files of the older format.
CACHES=()
n=0
for cache in "$@"; do
    n=$((n + 1))
    if [ "$(head -c 8 "$cache")" = "LXSHCACH" ]; then
        converted="$OUT/cache$n.fork-v$VERSION.bin"
        "$TOOL" cache-export --cache "$cache" --out "$converted"
        CACHES+=("$converted")
    else
        CACHES+=("$cache")
    fi
done
set -- "${CACHES[@]}"
TRAIN=()
USED=()
for cache in "$@"; do TRAIN+=(--train "$cache"); USED+=(--used "$cache"); done

echo "== 1. the game's shader programs, found in its files by content"
"$TOOL" extract --game "$NX2_GAME" --out "$OUT/game" --threads "$THREADS"

echo "== 2. storage-pointer priors from the recorded caches"
"$PYTHON" -I "$PY/learn_priors.py" "$PRIORS" "$@"

echo "== 3. oracle accuracy against each recorded cache"
for cache in "$@"; do
    "$TOOL" validate --pkg "$PROGRAMS" --cache "$cache" --out "$OUT/validate_$(basename "$cache" .bin)" \
        --priors "$PRIORS" --threads "$THREADS" | grep -v differs:
done

echo "== 4. every program: lowered, hashed and given oracle facts"
ISOLATED=${NX2_ISOLATED_SCAN:-auto}
if [ "$ISOLATED" = auto ]; then
    ISOLATED=$("$PYTHON" -c 'import os; print(int(os.name == "posix"))')
fi
if [ "$ISOLATED" = 1 ]; then
    "$PYTHON" -I "$PY/scan_isolated.py" --tool "$TOOL" --pkg "$PROGRAMS" \
        --out "$OUT/scan" --priors "$PRIORS" --workers "${NX2_SCAN_WORKERS:-2}" \
        --memory-mib "${NX2_SCAN_MEMORY_MIB:-1536}" --timeout "${NX2_SCAN_TIMEOUT:-20}"
else
    "$TOOL" scan --pkg "$PROGRAMS" --out "$OUT/scan" --priors "$PRIORS" --threads "$THREADS"
fi

echo "== 5. inventory, vertex/pixel pairs, mesh combinations"
"$PYTHON" -I "$PY/game_tables.py" "$OUT/game.tsv" "$OUT/game.variations.tsv" "$OUT/scan.tsv" "$OUT" "${USED[@]}"
"$PYTHON" -I "$PY/mesh_draws.py" "$PROGRAMS" "$OUT/inventory.json" "$OUT/mesh_combos.json" "$OUT/mesh_draws.tsv"
"$TOOL" mesh-envs --pkg "$PROGRAMS" --cache "$OUT/mesh_draws.tsv" --out "$OUT/mesh" --priors "$PRIORS" --threads "$THREADS"

if [ $# -ge 2 ]; then
    echo "== 6. held-out render-state accuracy (learn from the first cache, test the second's new pipelines)"
    "$PYTHON" -I "$PY/state_eval2.py" "$OUT/inventory.json" "$OUT/nx_pairs.json" "$1" "$2"
fi

echo "== 7. precompiled file"
"$PYTHON" -I "$PY/build_precompiled.py" --inventory "$OUT/inventory.json" --envs "$OUT/scan.envs" \
    --pairs "$OUT/nx_pairs.json" "${TRAIN[@]}" --k "$K" \
    --mesh-combos "$OUT/mesh_combos.json" --mesh-envs "$OUT/mesh.mesh" --mesh-k 2 \
    --cache-version "$VERSION" --out "$PREP" --report "$OUT/precompiled_report.json"

echo "== 8. replay check"
"$TOOL" check --file "$PREP" --threads "$THREADS" || echo "(replay failures are prepared as failed pipelines; see above)"

echo "== 9. SPIR-V for every environment, one at a time (a compiler abort is listed, not fatal)"
"$PYTHON" -I "$PY/emit_check.py" "$TOOL" "$PREP" "$OUT/emit_crashes.json"
echo "== 10. recipe: the precompiled file without the game's code, to share; and its audit"
"$TOOL" recipe --file "$PREP" --game "$NX2_GAME" --out "$RECIPE" --threads "$THREADS"
"$TOOL" audit --recipe "$RECIPE" --game "$NX2_GAME" --threads "$THREADS"
echo "== 11. assemble the recipe and verify its reconstructed shaders"
"$TOOL" assemble --recipe "$RECIPE" --game "$NX2_GAME" --out "$OUT/native-preparation.bin" --threads "$THREADS"
NX2_CHECK_EMIT=1 "$TOOL" check --file "$OUT/native-preparation.bin" --threads "$THREADS"
echo "done: import $RECIPE with Build Shader Precache from Recipe..., or install $OUT/native-preparation.bin in the title's shader directory"
