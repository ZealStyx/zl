#!/usr/bin/env bash
# Auto-discovering ZL regression runner for tests/zl/valid and tests/zl/invalid.
#
# This script does NOT hardcode any test case paths. It walks
# ../tests/zl/invalid/<category>/<case> and ../tests/zl/valid/<category>/<case>
# (plus flat *.zl files directly inside a category folder) and finds each
# case's entry file automatically. Two kinds of case are recognized:
#
#   - plain: a *.zl file, or a subdirectory containing a *.zl file that
#     declares "func main(" -- run directly via
#     zl_language, same as always.
#   - package-manager: a subdirectory whose root, or conventional
#     <case>/project directory, contains zlpkg.toml -- run via
#     `zlpkg run <main()-file found within that project dir>`, from within
#     the project dir, so dependencies declared in its zlpkg.toml get
#     resolved. Its generated .zlpkg/ cache and zlpkg.lock are removed
#     again after each run, so repeated runs stay deterministic. zlpkg is
#     looked for next to the zl_language binary; if it isn't there, these
#     cases are skipped (not failed) with a note explaining why.
#
# Add a new folder/case under ../tests/zl/valid or ../tests/zl/invalid and it
# shows up here next run -- nothing to edit.
#
# Usage:
#   ./run_examples.sh <path-to-zl_language>                 interactive menu
#   ./run_examples.sh <path-to-zl_language> all              non-interactive, runs everything
#   ./run_examples.sh <path-to-zl_language> invalid          non-interactive, all invalid categories
#   ./run_examples.sh <path-to-zl_language> valid            non-interactive, all valid categories
#   ./run_examples.sh <path-to-zl_language> all 1,3          non-interactive, categories 1 and 3 from that scope
#   ./run_examples.sh <path-to-zl_language> --list           list discovered categories/cases and exit
#
# Interactive mode:
#   1) Test All   2) Invalid   3) Valid   -- pick a scope
#   Categories in that scope are then listed with numbers.
#   Type 0 (or just press enter) to run all of them, or a comma list like
#   1,2,5 to run only those categories.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 /path/to/zl_language[.exe] [all|invalid|valid] [selection]" >&2
    exit 2
fi

ZL="$1"
if [[ ! -x "$ZL" ]]; then
    echo "error: '$ZL' is not an executable file" >&2
    exit 2
fi
# Make absolute: package-manager cases run with their cwd changed to the
# case's own project dir, so a relative $ZL/$ZLPKG would break there.
ZL="$(cd "$(dirname "$ZL")" && pwd)/$(basename "$ZL")"
ZLPKG="$(dirname "$ZL")/zlpkg"
[[ -x "$ZLPKG" ]] || ZLPKG=""
shift

MODE_ARG="${1:-}"
SELECTION_ARG="${2:-}"

VALID_ROOT="../tests/zl/valid"
INVALID_ROOT="../tests/zl/invalid"

# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

# find_entry DIR -- prints the first *.zl file under DIR that declares a
# main() function (works no matter how deeply the case is nested, e.g.
# import-style cases under src/pkg/...).
find_entry() {
    local dir="$1"
    grep -lrE 'func[[:space:]]+main[[:space:]]*\(' \
        --include='*.zl' "$dir" 2>/dev/null | sort | head -n1
}

# find_manifest DIR -- prints a manifest only for DIR's own project root.
# Do not recursively search: ordinary multi-file ZL cases can contain nested
# package fixtures/dependencies and must still run their own main() directly.
find_manifest() {
    local dir="$1"
    if [[ -f "$dir/zlpkg.toml" ]]; then
        echo "$dir/zlpkg.toml"
        return
    fi
    if [[ -f "$dir/project/zlpkg.toml" ]]; then
        echo "$dir/project/zlpkg.toml"
        return
    fi
}

# list_categories ROOT -- prints category folder names (one per line, sorted)
list_categories() {
    local root="$1"
    [[ -d "$root" ]] || return 0
    find "$root" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort | while read -r cat; do
        [[ "$cat" == "demos" ]] && continue
        echo "$cat"
    done
}

# list_cases ROOT/CATEGORY -- prints "case_name|kind|location" for every
# case in a category, where kind is "zl" (location = entry .zl file, run
# directly) or "pkg" (location = project dir containing zlpkg.toml, run via
# `zlpkg run`).
list_cases() {
    local catdir="$1"
    local entry name
    for entry in "$catdir"/*; do
        [[ -e "$entry" ]] || continue
        if [[ -d "$entry" ]]; then
            name="$(basename "$entry")"
            if is_support_fixture "$entry"; then
                continue
            fi
            local manifest
            manifest="$(find_manifest "$entry")"
            if [[ -n "$manifest" ]]; then
                echo "${name}|pkg|$(dirname "$manifest")"
                continue
            fi
            local ef
            ef="$(find_entry "$entry")"
            [[ -n "$ef" ]] && echo "${name}|zl|${ef}"
        elif [[ "$entry" == *.zl ]]; then
            # Root-level helper/library .zl files are not runnable cases unless
            # they declare the program entry point. This keeps support files
            # (for example reflection/type-only declarations) from producing
            # false "program has no main()" failures.
            if grep -Fq "func main(" "$entry"; then
                name="$(basename "$entry" .zl)"
                echo "${name}|zl|${entry}"
            fi
        fi
    done
}

display_name() {
    # "generics_tests" -> "generics tests", "core_tests" -> "core tests"
    echo "${1//_/ }"
}

# ---------------------------------------------------------------------------
# Test execution
# ---------------------------------------------------------------------------

PASS=0
FAIL=0
SKIP=0

# Fixture-only directories that intentionally contain helper ZL declarations
# but no runnable main(). They are imported by another case, not cases themselves.
is_support_fixture() {
    [[ "$(basename "$1")" == "closure_support" ]]
}

run_case() {
    local name="$1" kind="$2" location="$3" expected="$4"
    local out actual

    if [[ "$kind" == "pkg" ]]; then
        if [[ -z "$ZLPKG" ]]; then
            echo "  SKIP  $name  (no zlpkg binary next to $ZL - build the zlpkg CMake target to run package-manager cases)"
            SKIP=$((SKIP + 1))
            return
        fi
        local proj_entry rel_entry
        proj_entry="$(find_entry "$location")"
        if [[ -z "$proj_entry" ]]; then
            if is_support_fixture "$location"; then
                echo "  SKIP  $name  (support fixture; no main() entry point)"
                SKIP=$((SKIP + 1))
                return
            fi
            echo "  FAIL  $name"
            echo "        no main()-declaring .zl file found under $location"
            FAIL=$((FAIL + 1))
            return
        fi
        rel_entry="${proj_entry#"$location"/}"
        rm -rf "$location/.zlpkg" "$location/zlpkg.lock"
        out="$(cd "$location" && timeout 20s "$ZLPKG" run "$rel_entry" </dev/null 2>&1)"
        actual=$?
        rm -rf "$location/.zlpkg" "$location/zlpkg.lock"
    else
        local direct_location="$location"
        if [[ "$direct_location" != /* ]]; then
            direct_location="$SCRIPT_DIR/$direct_location"
            direct_location="$(cd "$(dirname "$direct_location")" && pwd)/$(basename "$direct_location")"
        fi
        local case_timeout="20s"
        if [[ "$direct_location" == *"/native/benchmarks/"* ]]; then
            case_timeout="60s"
        fi
        out="$(cd "$SCRIPT_DIR/.." && timeout "$case_timeout" "$ZL" "$direct_location" </dev/null 2>&1)"
        actual=$?
    fi

    local termination_marker="${location%.zl}.expect-termination"
    if [[ -f "$termination_marker" ]]; then
        if [[ "$actual" -ne 0 && "$actual" -ne 124 ]]; then
            echo "  PASS  $name  (expected process termination: exit $actual)"
            PASS=$((PASS + 1))
            return
        fi
    fi

    if [[ "$actual" -eq "$expected" ]]; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        echo "        location:      $location  ($( [[ "$kind" == "pkg" ]] && echo "via zlpkg run" || echo "direct" ))"
        echo "        expected exit: $expected  (got $actual)"
        echo "        actual output:"
        echo "$out" | sed 's/^/          /'
        FAIL=$((FAIL + 1))
    fi
}

run_category() {
    local root="$1" category="$2" expected="$3"
    local catdir="$root/$category"
    echo
    echo "--- $(display_name "$category") ($( [[ "$expected" -eq 0 ]] && echo valid || echo invalid )) ---"
    while IFS='|' read -r name kind location; do
        [[ -n "$name" ]] || continue
        run_case "$name" "$kind" "$location" "$expected"
    done < <(list_cases "$catdir")
}

# ---------------------------------------------------------------------------
# Permanent compiler regressions that intentionally do not require another
# example-file fixture. These are generated in a temporary directory so the
# example tree stays compact while the regression logic remains versioned.
# ---------------------------------------------------------------------------

run_structural_pattern_regressions() {
    local tmpdir set_file map_file out actual
    tmpdir="$(mktemp -d)"
    set_file="$tmpdir/DuplicateSet.zl"
    map_file="$tmpdir/DuplicateMap.zl"
    trap 'rm -rf "$tmpdir"' RETURN

    cat >"$set_file" <<'EOF'
class DuplicateSet {
    static func main(): void {
        set<int> s = {1, 2}
        var x = match s { set [1, 1] => 1, _ => 0 }
        log(x)
    }
}
EOF
    cat >"$map_file" <<'EOF'
class DuplicateMap {
    static func main(): void {
        map<string,int> m = {"a": 1, "b": 2}
        var x = match m { map { "a": 1, "a": 1 } => 1, _ => 0 }
        log(x)
    }
}
EOF

    for local_file in "$set_file" "$map_file"; do
        out="$(timeout 5s "$ZL" --check "$local_file" 2>&1)"
        actual=$?
        if [[ "$actual" -eq 1 ]]; then
            echo "  PASS  structural pattern duplicate: $(basename "$local_file" .zl)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL  structural pattern duplicate: $(basename "$local_file" .zl)"
            echo "        expected exit: 1 (got $actual)"
            echo "$out" | sed 's/^/          /'
            FAIL=$((FAIL + 1))
        fi
    done
    trap - RETURN
    rm -rf "$tmpdir"
}

# ---------------------------------------------------------------------------
# --list mode
# ---------------------------------------------------------------------------

if [[ "$MODE_ARG" == "--list" || "$MODE_ARG" == "-l" ]]; then
    echo "=== Invalid ==="
    for cat in $(list_categories "$INVALID_ROOT"); do
        echo "$(display_name "$cat"):"
        list_cases "$INVALID_ROOT/$cat" | cut -d'|' -f1 | sed 's/^/  /'
    done
    echo
    echo "=== Valid ==="
    for cat in $(list_categories "$VALID_ROOT"); do
        echo "$(display_name "$cat"):"
        list_cases "$VALID_ROOT/$cat" | cut -d'|' -f1 | sed 's/^/  /'
    done
    exit 0
fi

# ---------------------------------------------------------------------------
# Resolve scope (all / invalid / valid)
# ---------------------------------------------------------------------------

normalize_mode() {
    case "$1" in
        1|[Aa]ll) echo "all" ;;
        2|[Ii]nvalid) echo "invalid" ;;
        3|[Vv]alid) echo "valid" ;;
        *) echo "" ;;
    esac
}

SCOPE="$(normalize_mode "$MODE_ARG")"

if [[ -z "$SCOPE" ]]; then
    echo "1. Test All"
    echo "2. Invalid"
    echo "3. Valid"
    read -r -p "Select mode: " choice
    SCOPE="$(normalize_mode "$choice")"
    if [[ -z "$SCOPE" ]]; then
        echo "error: unrecognized selection '$choice'" >&2
        exit 2
    fi
fi

# ---------------------------------------------------------------------------
# Build the numbered category list for the chosen scope
# ---------------------------------------------------------------------------

IDX=0
declare -a ENTRY_ROOT ENTRY_CAT ENTRY_EXPECTED

print_section() {
    local label="$1" root="$2" expected="$3"
    local cats
    cats="$(list_categories "$root")"
    [[ -z "$cats" ]] && return 0
    echo
    echo "=== $label ==="
    local cat
    for cat in $cats; do
        IDX=$((IDX + 1))
        ENTRY_ROOT[$IDX]="$root"
        ENTRY_CAT[$IDX]="$cat"
        ENTRY_EXPECTED[$IDX]="$expected"
        local n
        n=$(list_cases "$root/$cat" | wc -l | tr -d ' ')
        echo "$IDX. $(display_name "$cat")  ($n case$( [[ "$n" == 1 ]] || echo s ))"
    done
}

if [[ "$SCOPE" == "all" || "$SCOPE" == "invalid" ]]; then
    print_section "Invalid" "$INVALID_ROOT" 1
fi
if [[ "$SCOPE" == "all" || "$SCOPE" == "valid" ]]; then
    print_section "Valid" "$VALID_ROOT" 0
fi

if [[ "$IDX" -eq 0 ]]; then
    echo "no test categories found under ../tests/zl/valid or ../tests/zl/invalid" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Resolve selection (which of the listed categories to actually run)
# ---------------------------------------------------------------------------

if [[ -z "$SELECTION_ARG" ]]; then
    echo
    read -r -p "Run which? (0 = all, or e.g. 1,2,5): " SELECTION_ARG
fi
SELECTION_ARG="${SELECTION_ARG// /}"

declare -a CHOSEN
if [[ -z "$SELECTION_ARG" || "$SELECTION_ARG" == "0" ]]; then
    for ((i = 1; i <= IDX; i++)); do CHOSEN+=("$i"); done
else
    IFS=',' read -r -a CHOSEN <<< "$SELECTION_ARG"
fi

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

for n in "${CHOSEN[@]}"; do
    if [[ ! "$n" =~ ^[0-9]+$ ]] || [[ "$n" -lt 1 ]] || [[ "$n" -gt "$IDX" ]]; then
        echo "warning: skipping invalid selection '$n'" >&2
        continue
    fi
    run_category "${ENTRY_ROOT[$n]}" "${ENTRY_CAT[$n]}" "${ENTRY_EXPECTED[$n]}"
done

if [[ "$SCOPE" == "all" || "$SCOPE" == "invalid" ]]; then
    run_structural_pattern_regressions
fi

echo
echo "$PASS passed, $FAIL failed$( [[ "$SKIP" -gt 0 ]] && echo ", $SKIP skipped" )"
[[ "$FAIL" -eq 0 ]]
