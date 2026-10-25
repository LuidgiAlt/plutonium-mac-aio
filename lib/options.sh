# launch.sh's options (README.md): checked before anything is changed.
# shellcheck shell=bash

# options_validate: prints "bad option: NAME=value" and returns 1 on the
# first unsupported value.
options_validate() {
  local name value
  bad() { echo "bad option: $1=$2" >&2; return 1; }
  case "${D3D9:-dxvk}" in dxvk|vulkan) ;; *) bad D3D9 "$D3D9"; return 1 ;; esac
  case "${NTDLL:-fixed}" in fixed|legacy) ;; *) bad NTDLL "$NTDLL"; return 1 ;; esac
  case "${XAUDIO:-native}" in native|builtin) ;; *) bad XAUDIO "$XAUDIO"; return 1 ;; esac
  if [ -n "${MAXFPS:-}" ]; then
    case "$MAXFPS" in ''|*[!0-9]*|0*) bad MAXFPS "$MAXFPS"; return 1 ;; esac
  fi
  for name in LAZY_EXEC SOCKFIX HUD WINDOWED FULLSCREEN TWEAKS VERBOSE DXVK_ASYNC; do
    eval "value=\${$name:-}"
    case "$value" in ''|0|1) ;; *) bad "$name" "$value"; return 1 ;; esac
  done
  return 0
}
