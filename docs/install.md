# Install the native driver

This is a developer beta for supervised hardware testing, not a signed consumer installer. These instructions target the 0.1.18 development driver and replace machine-specific install paths with repository-relative commands. The portable restore helper has offline tests; a clean installation following this guide has not yet been repeated on a fresh Mac.

Read the removal section before starting. An experimental kernel driver can crash the Mac, and the development security settings reduce system protection. Have a backup, physical access and a separate management connection. Do not use the CX5 link for the SSH or Screen Sharing session that manages installation.

## Hardware and software used

| Component | Lab configuration |
|---|---|
| Mac | Mac Studio, M3 Ultra, 256 GB unified memory |
| macOS | macOS 27, build `26A428`, used for the 0.1.18 functional checks |
| Enclosure | OWC Mercury Helios 5S ([Amazon affiliate link](https://link.amazon/B04jjctoS)), Thunderbolt 5 PCIe enclosure |
| Mac NIC | Mellanox ConnectX-5 Ex EN, MCX516A-CDAT, dual QSFP28; PCI ID `15b3:1019` |
| Mac cable | Thunderbolt 5 cable from the Studio to the powered enclosure |
| Network cable | Mellanox MCP1600-C001E30N ([Amazon affiliate link](https://link.amazon/B0hdC6Du8)), 1 m passive QSFP28-to-QSFP28 DAC |
| Network link | 100GBASE-CR4 with RS-FEC on those cables, validated 2026-09-15 with driver 0.1.17; the 0.1.16 campaign and the pooled headline latency table ran at 40 Gb/s on an earlier cable |
| Peer | One NVIDIA DGX Spark with its ConnectX-7 Ethernet interface |
| Link settings | Ethernet MTU 9000, RC path MTU 4096 |
| Build tools | Xcode with the macOS 27 SDK, Apple command-line tools and Python 3 |

Affiliate disclosure: I may earn a commission from purchases made through these Amazon links.

The wider lab has two Sparks, but the published latency figures cover one directly connected Spark and the Studio. A second Spark is not needed for this setup. QSFP28 is the cable connector, not SFP28. The NIC's nominal port rate is not a claim of measured Thunderbolt throughput or two-port scaling: the driver's PCIe-path readout shows the Thunderbolt 5 tunnel as PCIe Gen4 x4 with a 128-byte maximum payload on every hop, and both ports share that host path. The [17 September report](validation-2026-09-17.md) records about 50.6 Gbit/s into Studio memory and 29.4 Gbit/s out with Studio-initiated READ/WRITE, plus bounded concurrent-port checks.

The source accepts exact builds `26A428` and the earlier inspected beta `26A5425a`; this guide and restore helper target `26A428`. An arbitrary macOS 27 build is not sufficient. Check `sw_vers -buildVersion`, and do not remove the build guard to force an unsupported version to load.

Apple's built-in AppleEthernetMLX5 Ethernet driver and Apple's Thunderbolt RDMA enablement are separate from this native CX5 RDMA provider. MCDMA takes ownership of the selected CX5 PCI functions. Its `mcrdmaN` interfaces hold RDMA addressing; they do not provide ordinary IP packet transmission, ping or TCP networking. Use Wi-Fi or another Ethernet interface for management.

## 1. Enable RDMA and developer kernel extensions in Recovery

Shut down the Mac, hold its power button until startup options appear, then choose Options → Continue and authenticate. These changes require physical access to Recovery.

Open Utilities → Startup Security Utility, select the startup disk, open Security Policy, choose **Reduced Security**, and tick **Allow user management of kernel extensions from identified developers**. Apply the change. Select Reduced Security; do not disable that option. The remote-management checkbox is not a substitute for user-managed kernel extensions.

In Utilities → Terminal, run:

```sh
rdma_ctl enable
```

That enables Apple's RDMA feature; it does not install MCDMA. For this project's current ad-hoc-signed development build, the tested lab setup also disabled SIP:

```sh
csrutil disable
```

SIP disablement is a separate development exception, not a general requirement for Apple's built-in RDMA or properly signed production kernel extensions. This can leave an Apple Silicon development system reporting Permissive policy. Do not disable authenticated-root protection, add unrelated boot arguments or disable other protections to imitate historical troubleshooting. Restart into normal macOS after applying the settings.

Apple documents [RDMA enablement in Recovery](https://developer.apple.com/documentation/technotes/tn3205-low-latency-communication-with-rdma-over-thunderbolt), [custom kernel extension installation](https://developer.apple.com/documentation/apple-silicon/installing-a-custom-kernel-extension) and [temporary SIP changes for development](https://developer.apple.com/documentation/security/disabling-and-enabling-system-integrity-protection).

In normal macOS, inspect the resulting state:

```sh
sw_vers -buildVersion
rdma_ctl status
csrutil status
csrutil authenticated-root status
```

## 2. Build and prepare a local copy

Run these commands from the repository root on the target Mac. The build needs Apple's SDK RDMA headers and `librdma`; Linux `libibverbs` is not a replacement for the Mac SDK library.

```sh
xcode-select -p
xcrun --sdk macosx --show-sdk-version
python3 tools/build.py test
python3 tools/build.py native
```

SDK major version 27 is required. If `kmutil` later explicitly requires a Kernel Debug Kit, obtain the kit matching the running OS build from [Apple Developer Downloads](https://developer.apple.com/download/all/); do not substitute a nearby beta or bypass the missing-kit check. SDKs and KDKs are not included in this repository.

The build creates a disabled kernel extension. Make a separate enabled copy, then ad-hoc sign it:

```sh
mkdir -p local/install
# Stop if this destination already exists; preserve an earlier candidate.
test ! -e local/install/MCDMACX5Native.kext && \
  ditto build/MCDMACX5Native.kext local/install/MCDMACX5Native.kext
```

Run the following only after that copy succeeds:

```sh
/usr/libexec/PlistBuddy -c 'Set :IOKitPersonalities:MCDMACX5Native:MCDMALabEnabled true' local/install/MCDMACX5Native.kext/Contents/Info.plist
/usr/libexec/PlistBuddy -c 'Set :IOKitPersonalities:MCDMACX5Native:MCDMAUserQueues true' local/install/MCDMACX5Native.kext/Contents/Info.plist
/usr/libexec/PlistBuddy -c 'Set :IOKitPersonalities:MCDMACX5Native:MCDMAUserBlueFlame true' local/install/MCDMACX5Native.kext/Contents/Info.plist
codesign --force --deep --sign - local/install/MCDMACX5Native.kext
codesign --verify --strict local/install/MCDMACX5Native.kext
dwarfdump --uuid local/install/MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native
shasum -a 256 build/libmcdma-rdmav34.so
```

Keep the enabled kext and provider from the same build together; if you rebuild, prepare and sign a new enabled copy before installing. Record the UUID and provider hash locally for the post-reboot checks. Rebuilding can change these values; do not compare a new build to an old published UUID as if they must match. These personality flags grant the tested features; userspace still opts into direct posting and BlueFlame separately.

## 3. Install, approve and restart

Finish any RDMA jobs and shut down the Mac before initially disconnecting the enclosure. Boot with the enclosure disconnected for installation. Existing experimental DriverKit versions of MCDMA must be deactivated through their original app/uninstaller so they do not compete for the PCI function. Do not delete or modify Apple's built-in drivers.

The installer checks the build, saves any existing MCDMA install and installs only the kernel extension, provider and provider configuration. It does not restart the Mac or alter security settings. Run it from the repository root:

```sh
bash tools/install-native.sh --check
sudo /bin/bash tools/install-native.sh --install
```

Keep the printed backup directory. If `kmutil` requests approval, open System Settings → Privacy & Security, approve `org.mcdma.cx5.native` or the displayed MCDMA entry, authenticate and restart when macOS requests it. A load exit of zero alone does not establish that the new version owns the card. Do not repeatedly run the install block to dismiss an approval or dependency failure; read the actual error first.

After approval/restart, shut down, connect the powered Helios over Thunderbolt 5 and attach the QSFP28 cable to the intended Spark port, then boot and sign in. This avoids relying on unvalidated removal of a running driver with mapped DMA pages. If both Studio ports are cabled, confirm which Spark port each cable actually reaches before configuring addresses and neighbors. A crossed pair observed on 2026-09-15 reported the port active on both sides while Studio-initiated transfers failed with retry-exceeded completions; swapping the two cables at the Studio fixed it without any software change.

On Mac laptops with Apple silicon, macOS can hold a newly connected accessory until the user approves it ([Apple: allow accessories to connect](https://support.apple.com/en-ie/102282)); the default policy, *Ask for new accessories*, prompts the first time a device connects. After the Helios is connected, macOS asks whether to **Allow accessory to connect**; until that is accepted, System Information lists the Helios on Thunderbolt at its full link rate but no PCI device behind it, so no driver can match the card. Confirm that `system_profiler SPPCIDataType` lists vendor `0x15b3`, device `0x1019` before checking which driver owns it. This was observed on 2026-09-23 on a MacBook Pro with macOS 27 build `26A428` and Apple's built-in Ethernet driver, before MCDMA was installed.

```sh
kmutil showloaded --list-only --variant-suffix release | grep org.mcdma.cx5.native
ioreg -r -c MCDMACX5Native -l
ifconfig -a
ibv_devices
```

Check version 0.1.18 and the UUID recorded from your own signed build. A mismatched UUID means the expected build is not loaded. `MCDMAQuarantined` or a nonzero startup error requires diagnosis, not repeated transfer attempts. The old `rdma_enN` Thunderbolt ports may remain down when no Mac-to-Mac Thunderbolt RDMA link exists; the CX5 devices are named `rdma_mcrdmaN`.

## 4. Configure the Linux peer and restore the Mac GID

On the Spark, inspect `rdma link`, `ip -br link` and `ibv_devinfo`, select the CX7 port physically connected to the Mac, and record its netdev, RDMA device, port, MAC address and RoCE v2 GID index. Do not change the separate inter-Spark link. The Linux interface must be in Ethernet/RoCE mode with its normal `mlx5_core` and `mlx5_ib` drivers.

On GB10-based peers, each QSFP port appears as two netdevs and two RDMA devices on separate PCIe functions, for example `enp1s0f1np1` / `rocep1s0f1` and `enP2p1s0f1np1` / `roceP2p1s0f1`, and both report link on the one cable. Pick one pair and use it consistently for `PEER_IF`, the RDMA device and the GID index. On an ASUS Ascent GX10 (GB10, ConnectX-7 firmware 28.45.4028, Ubuntu 24.04) the `enp1s0…` / `rocep1s0…` function was used.

Leave the peer port at its default autonegotiation and FEC settings. On the same GX10, a port that had earlier been forced with `ethtool -s "$PEER_IF" speed 100000 autoneg off` and `ethtool --set-fec "$PEER_IF" encoding rs` stayed down with a working cable; `ethtool -s "$PEER_IF" autoneg on` and `ethtool --set-fec "$PEER_IF" encoding auto` brought it up at 100000 Mb/s with RS-FEC active.

If the port reports `Link detected: no (Cable issue, Unsupported cable)`, read the cable's identification with `sudo ethtool -m "$PEER_IF"` before suspecting the NIC. Two generic third-party 1 m QSFP28 DACs whose `Transceiver codes` were all `0x00`, with no extended 100GBASE-CR4 compliance code, never linked on the ConnectX-7 ports of two GX10s, with autonegotiation or forced to 100 Gb/s, and a CX5 port-to-port loopback under Apple's Ethernet driver stayed inactive with the same cable. As a control, the NVIDIA DAC already used between the two GX10s linked the same CX5 and ConnectX-7 ports at 100GBASE-CR4 immediately; that is an observation about that one cable, not a recommended part. Use the MCP1600-C001E30N listed above, and check that `ethtool -m` names a 100GBASE-CR4 transceiver type for any other DAC.

Clone this repository on the Spark too and check out the same commit as on the Mac before building the peer client. You can obtain that commit with `git rev-parse HEAD` in the Mac checkout.

For an Ubuntu-based peer with missing development tools, install `build-essential`, `libibverbs-dev`, `ibverbs-utils`, `rdma-core` and `iproute2` using its package manager. Build the supplied peer client:

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror peer/verbs_peer.c -libverbs -o build/verbs-peer
```

Set `PEER_IF` to the selected physical port. If NetworkManager manages it, as on a default DGX OS or Ubuntu desktop install, it keeps retrying DHCP on this unaddressed link; on the GX10 above it was still "connecting (getting IP configuration)" after the link came up. Release the port before making the manual MTU, address and neighbor changes so that a NetworkManager reconnect cannot replace them. This setting lasts only until the peer reboots ([NetworkManager: unmanaging devices](https://networkmanager.dev/docs/admins/#unmanaging-devices)), so repeat it after every peer reboot before restoring the MTU, address and neighbor:

```sh
sudo nmcli dev set "$PEER_IF" managed no
```

Then inspect its GIDs and addresses:

```sh
sudo ip link set dev "$PEER_IF" mtu 9000 up
ip -6 addr show dev "$PEER_IF" scope link
cat "/sys/class/net/$PEER_IF/address"
rdma link
ibv_devinfo
```

The current connector uses a MAC-derived IPv6 link-local GID. If the port uses a stable/privacy address instead, set `PEER_HWADDR` to its observed MAC and calculate the compatible address from the repository root on the peer:

```sh
PEER_GID=$(python3 -c 'import runpy,sys; print(runpy.run_path("tools/restore-rdma.py")["link_local"](sys.argv[1]))' "$PEER_HWADDR")
sudo ip -6 addr add "$PEER_GID/64" dev "$PEER_IF"
```

Only add the address if it is absent. Choose its actual **RoCE v2** GID index from the RDMA device's `ports/1/gids` and `ports/1/gid_attrs/types` sysfs entries. Do not assume index 1 on every Linux installation.

On the Mac, inspect `ifconfig -a` and select the wired `mcrdmaN` interface by its hardware MAC address. Set `MAC_IF`, `MAC_HWADDR`, `PEER_GID` and `PEER_HWADDR` to your observed values. Use the full peer GID without a `%zone` suffix. From the Mac repository root:

```sh
python3 tools/restore-rdma.py --dry-run \
  --interface "$MAC_IF" --expected-mac "$MAC_HWADDR" \
  --peer-gid "$PEER_GID" --peer-mac "$PEER_HWADDR"

sudo python3 tools/restore-rdma.py \
  --interface "$MAC_IF" --expected-mac "$MAC_HWADDR" \
  --peer-gid "$PEER_GID" --peer-mac "$PEER_HWADDR"
```

This is the portable address-restore command. It checks the loaded version and selected interface MAC, adds the Mac's MAC-derived link-local address, brings the interface up, installs the static peer neighbor and runs the native GID checker. It does not change management interfaces or hide a failed command. Address and neighbor configuration is temporary, so rerun it after a reboot, resolving the interface name again. A successful discovery check is not yet a successful memory transfer.

On the Spark, also add the reciprocal static neighbor. Set `MAC_GID` to the Mac link-local address printed by the restore plan, without a zone suffix, and use the same observed `MAC_HWADDR`:

```sh
sudo ip -6 neigh replace "$MAC_GID" lladdr "$MAC_HWADDR" nud permanent dev "$PEER_IF"
ip -6 neigh show to "$MAC_GID" dev "$PEER_IF"
```

This peer entry is also temporary and must be restored after a peer reboot, after releasing `$PEER_IF` from NetworkManager again as described above. Both static neighbors are required because the Mac's address-only interface cannot answer ordinary neighbor discovery.

The driver configures and reads back its Ethernet MTU; a software-only `ifconfig mtu` change is rejected. Inspect the actual Mac MTU and use the matching RC path MTU. The native address interface does not send ordinary NDP packets, which is why the static neighbor is explicit.

## 5. Verify RDMA before using it

```sh
build/cx5-native-check --provider /usr/local/lib/rdma/libmcdma-rdmav34.so --require-gid
ibv_devinfo
```

Require a native CX5 port with `PORT_ACTIVE`, a nonzero RoCE v2 GID and a network-interface association. An unplugged second port can remain down. The checker requires at least one active CX5 port; inspect its printed device identity to confirm it is the one you wired.

Read [hardware validation](hardware-validation.md) and run the four-way byte-verifying check with `tools/native_cross_host.py`. Its required arguments deliberately have no personal paths, SSH aliases or hardware addresses built in. Set the following variables in the shell where you run the test, using values from your own machines:

| Variable | Value to supply |
|---|---|
| `MAC_SSH` | SSH destination for the target Mac over its management connection |
| `PEER_SSH` | SSH destination for the Spark over its management connection |
| `MAC_CLIENT` | Absolute path to `build/native-verbs-peer` on the Mac |
| `MAC_CHECKER` | Absolute path to `build/cx5-native-check` on the Mac |
| `PEER_CLIENT` | Absolute path to `build/verbs-peer` on the Spark |
| `MAC_IF` | Wired `mcrdmaN` interface selected by its MAC address |
| `PEER_IF` | Spark Ethernet interface physically connected to that CX5 port |
| `MAC_RDMA_DEVICE` | Corresponding CX5 device reported by Mac verbs discovery |
| `PEER_RDMA_DEVICE` | Corresponding CX7 device reported by Linux verbs discovery |
| `PEER_GID_INDEX` | Index of the selected MAC-derived link-local RoCE v2 GID on the Spark |

The earlier restore commands also use `MAC_HWADDR`, `PEER_HWADDR`, `MAC_GID` and `PEER_GID`, taken from those same selected interfaces. Shell variables set on one machine are not automatically available in a terminal on the other. Keep any saved assignments in ignored `local/` files.

The example below assumes those variables are set and that both machines are reachable by noninteractive SSH over the management network:

```sh
mkdir -p results
python3 tools/native_cross_host.py \
  --mac-host "$MAC_SSH" --peer-host "$PEER_SSH" \
  --mac-client "$MAC_CLIENT" --peer-client "$PEER_CLIENT" \
  --mac-provider /usr/local/lib/rdma/libmcdma-rdmav34.so \
  --mac-checker "$MAC_CHECKER" \
  --mac-interface "$MAC_IF" --peer-interface "$PEER_IF" \
  --mac-device "$MAC_RDMA_DEVICE" --peer-device "$PEER_RDMA_DEVICE" \
  --peer-gid-index "$PEER_GID_INDEX" --path-mtu 4096 \
  --payload-bytes 4096 --mac-cq-map 0 --mac-user-post 0 --mac-user-bf 0 \
  --output results/functional-kernel.json
```

`MAC_CLIENT` is the absolute path to `build/native-verbs-peer` on the Mac, `MAC_CHECKER` to `build/cx5-native-check`, and `PEER_CLIENT` to `build/verbs-peer` on the Spark. Device names must come from live discovery, not from this guide's examples. The runner uses noninteractive SSH on the management network and checks both preconfigured static neighbors; it does not install them or run sudo for you. Confirm key-based SSH access to both hosts before starting it.

Require successful WRITE and READ in both initiation directions with verified bytes. Then repeat into separate result files using `--mac-cq-map 2 --mac-user-post 1 --mac-user-bf 0`, followed by `--mac-cq-map 2 --mac-user-post 1 --mac-user-bf 64`. These settings select the direct path and the BlueFlame-64 path respectively. Discovery, a green link or `rdma_ctl status` alone does not prove DMA transfer. Logs can include memory-region access keys and machine details; keep `results/` private.

## Removal and recovery

Stop all RDMA applications and shut down before disconnecting the enclosure. Boot without it attached. Do not test live unload or cable removal while work queues or memory mappings exist; those hardware lifetime checks remain outstanding.

To remove this installation, move its three installed components out of the loader's directories and ask macOS to rebuild its collections:

```sh
sudo /bin/bash <<'SH'
set -euo pipefail
saved=$(/usr/bin/mktemp -d '/Library/Application Support/MCDMA-removed.XXXXXX')
for item in /Library/Extensions/MCDMACX5Native.kext /usr/local/lib/rdma/libmcdma-rdmav34.so /etc/libibverbs.d/mcdma.driver; do
  if [ -e "$item" ]; then /bin/mv "$item" "$saved/$(/usr/bin/basename "$item")"; fi
done
printf 'Removed files saved in: %s\n' "$saved"
/usr/bin/kmutil install --update-all
SH
```

Follow any approval/restart request, and verify the MCDMA bundle identifier is absent from `kmutil showloaded` after restarting. If a matching KDK is required, install the exact matching kit instead of forcing a collection rebuild. To roll back an older MCDMA version, use the backup created at installation and restore its kernel extension, provider and configuration together through the same approval/restart process.

Once the experimental extension is removed, return to Recovery, run `csrutil enable`, and restore your previous startup security policy; choose Full Security if no other third-party kernel extension needs Reduced Security. If you also want to undo the separate Apple RDMA opt-in, run `rdma_ctl disable` there, then restart. Leave that opt-in enabled if other RDMA software still uses it.

If normal startup fails, power down, disconnect the enclosure and use Recovery or Safe Mode to remove the experimental installation. Avoid unverified live-unload commands or repeated power cycling with active RDMA work.
