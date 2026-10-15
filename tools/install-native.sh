#!/bin/bash
# Install only the native kext, userspace provider and provider configuration.
# No Recovery settings, restarts, firmware changes or driver unloads are automated.
set -euo pipefail
case "${1:-}" in
  --help|-h) printf '%s\n' 'Usage: bash tools/install-native.sh --check | --install'; exit 0 ;;
  --check|--install) [ "$#" = 1 ] ;;
  *) printf '%s\n' 'Use --check for preflight or --install for an authenticated install.' >&2; exit 2 ;;
esac
mode=$1
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_dir"
# Keep in step with include/apple_build_ids.h.
case "$(/usr/bin/sw_vers -buildVersion)" in 26A428|26B5101f) ;; *) exit 1 ;; esac
[ -d local/install/MCDMACX5Native.kext ]
[ -f build/libmcdma-rdmav34.so ]
/usr/bin/codesign --verify --strict local/install/MCDMACX5Native.kext
[ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' local/install/MCDMACX5Native.kext/Contents/Info.plist)" = org.mcdma.cx5.native ]
[ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' local/install/MCDMACX5Native.kext/Contents/Info.plist)" = 0.1.18 ]
for key in MCDMALabEnabled MCDMAUserQueues MCDMAUserBlueFlame; do
  [ "$(/usr/libexec/PlistBuddy -c "Print IOKitPersonalities:MCDMACX5Native:$key" local/install/MCDMACX5Native.kext/Contents/Info.plist)" = true ]
done
/usr/bin/dwarfdump --uuid local/install/MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native
/usr/bin/shasum -a 256 build/libmcdma-rdmav34.so
if [ "$mode" = --check ]; then
  printf '%s\n' 'Preflight passed; nothing installed and no security settings changed.'
  exit 0
fi
[ "$(/usr/bin/id -u)" = 0 ] || { printf '%s\n' 'Run --install with sudo.' >&2; exit 2; }
backup=$(/usr/bin/mktemp -d '/Library/Application Support/MCDMA-backup.XXXXXX')
for item in /Library/Extensions/MCDMACX5Native.kext /usr/local/lib/rdma/libmcdma-rdmav34.so /etc/libibverbs.d/mcdma.driver; do
  if [ -e "$item" ]; then /usr/bin/ditto "$item" "$backup/$(/usr/bin/basename "$item")"; fi
done
printf 'Backup: %s\n' "$backup"
/usr/bin/ditto local/install/MCDMACX5Native.kext /Library/Extensions/MCDMACX5Native.kext
/usr/sbin/chown -R root:wheel /Library/Extensions/MCDMACX5Native.kext
/bin/chmod -R go-w /Library/Extensions/MCDMACX5Native.kext
/usr/bin/install -d -o root -g wheel -m 755 /usr/local/lib/rdma /etc/libibverbs.d
/usr/bin/install -o root -g wheel -m 755 build/libmcdma-rdmav34.so /usr/local/lib/rdma/libmcdma-rdmav34.so
printf 'driver /usr/local/lib/rdma/libmcdma\n' > /etc/libibverbs.d/mcdma.driver
/usr/sbin/chown root:wheel /etc/libibverbs.d/mcdma.driver
/bin/chmod 644 /etc/libibverbs.d/mcdma.driver
/usr/bin/codesign --verify --strict /Library/Extensions/MCDMACX5Native.kext
/usr/bin/kmutil load --load-style start-and-match --bundle-path /Library/Extensions/MCDMACX5Native.kext
