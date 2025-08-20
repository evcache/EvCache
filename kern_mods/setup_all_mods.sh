#!/usr/bin/env bash
set -uo pipefail

# colors
red=$'\033[31m'
grn=$'\033[32m'
ylw=$'\033[33m'
rst=$'\033[0m'

ok_mods=()
fail_mods=()

run_one() {
  local d="$1"
  local out rc
  if ! out="$(cd "$d" && ./setup_mod.sh 2>&1)"; then
    rc=$?
    printf '%s[x]%s %s: %s\n' "$red" "$rst" "$d" "$out"
    local mn="$(basename "$d")"
    fail_mods+=( "$mn" )
    return "$rc"
  fi
  printf '%s[+]%s %s: %s\n' "$grn" "$rst" "$d" "$out"
  local mn
  mn="$(sed -n 's/^ok: \([^[:space:]]\+\)$/\1/p' <<<"$out" | head -n1)"
  [[ -z "$mn" ]] && mn="$(basename "$d")"
  ok_mods+=( "$mn" )
  return 0
}

# run both known module dirs
dirs=( gpa_hpa_km vcolor_km )
any_failed=0
for d in "${dirs[@]}"; do
  [[ -d "$d" ]] && run_one "$d" || printf '%s[x]%s skip: %s (missing)\n' "$red" "$rst" "$d"
done

# summary
ok_count=${#ok_mods[@]}
fail_count=${#fail_mods[@]}
total=$(( ok_count + fail_count ))

if (( ok_count == total && total > 0 )); then
  printf '\n%s[+]%s summary: %d/%d succeeded\n' "$grn" "$rst" "$ok_count" "$total"
elif (( ok_count > 0 )); then
  printf '\n%s[!]%s summary: %d/%d succeeded\n' "$ylw" "$rst" "$ok_count" "$total"
else
  printf '\n%s[x]%s summary: %d/%d succeeded\n' "$red" "$rst" "$ok_count" "$total"
fi

(( ok_count > 0 )) && printf '%s[+]%s ok: %s\n' "$grn" "$rst" "$(IFS=, ; echo "${ok_mods[*]}")"
(( fail_count > 0 )) && printf '%s[x]%s fail: %s\n' "$red" "$rst" "$(IFS=, ; echo "${fail_mods[*]}")"

exit $any_failed
