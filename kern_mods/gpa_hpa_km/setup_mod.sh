#!/usr/bin/env bash
set -euo pipefail

# print short messages
msg() { printf '%s\n' "$*"; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { msg "fail: missing command '$1'"; exit 1; }
}

for cmd in make nproc sudo lsmod awk grep; do
  need_cmd "$cmd"
done

if ! command -v insmod >/dev/null 2>&1 && [[ ! -x /sbin/insmod && ! -x /usr/sbin/insmod ]]; then
  msg "fail: missing command 'insmod'"
  exit 1
fi

# build
if [[ -f Makefile ]]; then
  make -j"$(nproc)"
else
  msg "fail: no makefile"
  exit 2
fi

# find .ko
shopt -s nullglob
kos=( ./*.ko )
shopt -u nullglob
if (( ${#kos[@]} == 0 )); then
  msg "fail: no .ko"
  exit 3
elif (( ${#kos[@]} > 1 )); then
  msg "fail: multiple .ko"
  printf '%s\n' "${kos[@]}"
  exit 4
fi

ko="${kos[0]}"
modname="$(basename "${ko%.ko}")"

# reload if already loaded
if lsmod | awk '{print $1}' | grep -qx "$modname"; then
  sudo rmmod "$modname" || { msg "fail: rmmod $modname"; exit 5; }
fi

# load and verify
if sudo insmod "$ko"; then
  if lsmod | awk '{print $1}' | grep -qx "$modname"; then
    msg "ok: $modname"
    exit 0
  else
    msg "fail: not in lsmod ($modname)"
    exit 6
  fi
else
  msg "fail: insmod $ko"
  exit 7
fi
