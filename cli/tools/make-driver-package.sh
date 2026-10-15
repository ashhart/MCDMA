#!/bin/bash
# Assemble the driver payload mcdma installs (driver/): the installed kernel
# extension, the libibverbs provider, its registration file, the Mac tools and
# the Linux peer test tool, packed as one archive plus manifest.json.
#
# All sources are read over ssh from a Mac that already has the driver
# installed (and a Spark that has the Linux peer tool). Nothing is written on
# those machines. Required environment:
#   MAC=<ssh host of the Mac with the driver>
#   BUILD_DIR=<directory on that Mac holding the built tools: native-verbs-peer, cx5-native-check, mcdma-set, fabric-keepalive, ...>
# Optional:
#   SPARK=<ssh host of a Spark>  SPARK_PEER=<path of verbs-peer on that Spark>
set -euo pipefail
export COPYFILE_DISABLE=1
: "${MAC:?set MAC to the ssh host of the Mac that has the driver installed}"
: "${BUILD_DIR:?set BUILD_DIR to the directory of the built tools on that Mac}"
SPARK=${SPARK:-}
SPARK_PEER=${SPARK_PEER:-/usr/local/libexec/mcdma/verbs-peer}
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/driver"
stage=$(mktemp -d /tmp/mcdma-driver-pkg.XXXXXX)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/payload/tools/linux-arm64" "$out"

echo "· kernel extension from $MAC:/Library/Extensions"
ssh -o BatchMode=yes "$MAC" 'cd /Library/Extensions && tar cf - MCDMACX5Native.kext' | tar xf - -C "$stage/payload"
echo "· provider and registration"
ssh -o BatchMode=yes "$MAC" 'cat /usr/local/lib/rdma/libmcdma-rdmav34.so' > "$stage/payload/libmcdma-rdmav34.so"
ssh -o BatchMode=yes "$MAC" 'cat /etc/libibverbs.d/mcdma.driver' > "$stage/payload/mcdma.driver"
chmod 755 "$stage/payload/libmcdma-rdmav34.so"
echo "· Mac tools from $BUILD_DIR"
for t in native-verbs-peer cx5-native-check mcdma-set fabric-keepalive user-queue-check cq-map-check lifecycle-client mcdma-bw; do
  if ssh -o BatchMode=yes "$MAC" "test -x '$BUILD_DIR/$t'"; then
    ssh -o BatchMode=yes "$MAC" "cat '$BUILD_DIR/$t'" > "$stage/payload/tools/$t"; chmod 755 "$stage/payload/tools/$t"; echo "    $t"
  fi
done
if [ -n "$SPARK" ]; then
  echo "· Spark peer tool from $SPARK:$SPARK_PEER"
  if ssh -o BatchMode=yes "$SPARK" "test -x '$SPARK_PEER'"; then
    ssh -o BatchMode=yes "$SPARK" "cat '$SPARK_PEER'" > "$stage/payload/tools/linux-arm64/verbs-peer"; chmod 755 "$stage/payload/tools/linux-arm64/verbs-peer"
  fi
fi

plist="$stage/payload/MCDMACX5Native.kext/Contents/Info.plist"
version=$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$plist")
kpi=$(/usr/libexec/PlistBuddy -c 'Print OSBundleLibraries:com.apple.kpi.iokit' "$plist" 2>/dev/null || echo 0)
uuid=$(dwarfdump --uuid "$stage/payload/MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native" 2>/dev/null | awk '{print $2}' | head -1)
[ -n "$uuid" ] || uuid=$(otool -l "$stage/payload/MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native" | awk '/uuid/{print $2}' | head -1)
match=$(/usr/libexec/PlistBuddy -c 'Print IOKitPersonalities:MCDMACX5Native:IOPCIMatch' "$plist")
[ "$version" = 0.1.18 ] || { echo "Expected an installed 0.1.18 driver" >&2; exit 3; }
archive="mcdma-driver-$version.tar.gz"

tar czf "$out/$archive" -C "$stage/payload" .
sha() { shasum -a 256 "$1" | cut -d ' ' -f1; }
tools_json=$(cd "$stage/payload/tools" && ls -1 | grep -v linux-arm64 | awk '{printf "%s\"%s\": \"tools/%s\"", (NR>1?", ":""), $1, $1}')
cat > "$out/manifest.json" <<JSON
{
  "name": "MCDMA native ConnectX-5 driver",
  "version": "$version",
  "uuid": "$uuid",
  "archive": "$archive",
  "archive_sha256": "$(sha "$out/$archive")",
  "kext": "MCDMACX5Native.kext",
  "provider": "libmcdma-rdmav34.so",
  "conf": "mcdma.driver",
  "tools": { $tools_json },
  "spark_tools": { "verbs-peer": $( [ -f "$stage/payload/tools/linux-arm64/verbs-peer" ] && echo '"tools/linux-arm64/verbs-peer"' || echo null ) },
  "requires": { "macos_major": ${kpi%%.*}, "arch": "arm64", "pci_match": "$match" },
  "sha256": {
    "kext_executable": "$(sha "$stage/payload/MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native")",
    "kext_plist": "$(sha "$plist")",
    "provider": "$(sha "$stage/payload/libmcdma-rdmav34.so")",
    "conf": "$(sha "$stage/payload/mcdma.driver")"
  },
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
JSON
python3 - "$stage/payload" "$out/manifest.json" <<'PYJSON'
import hashlib,json,sys
from pathlib import Path
root=Path(sys.argv[1]); path=Path(sys.argv[2]); m=json.loads(path.read_text())
m['files']={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file()}
m['requires']['macos_build']='26A428'  # read by CLIs without macos_builds
m['requires']['macos_builds']=['26A428','26B5101f']  # keep in step with include/apple_build_ids.h
path.write_text(json.dumps(m,indent=2)+'\n')
PYJSON
echo "· wrote $out/$archive and manifest.json (driver $version, UUID $uuid)"
