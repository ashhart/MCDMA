# Link daemon for applications

`mcdma-rpcd` moves request and reply bytes between applications on two machines over MCDMA RDMA. Neither
application opens a verbs context. Each one talks to its local daemon through a shared-memory mailbox, and only the
daemons hold queue pairs, so an application that crashes or is killed cannot leave a queue pair behind.
`libmcdma-rpc` gives applications in any language the ordered word loads and stores the mailbox needs.

A link runs over one of two kinds of RDMA:

- **RoCE.** A Mac's ConnectX-5 through MCDMA's provider (`rdma_mcrdmaN`) and a Linux peer, or two Linux hosts with
  rdma-core. RC queue pairs carry RDMA WRITE and READ.
- **Thunderbolt.** Two Macs cabled port to port, using Apple's RDMA over Thunderbolt (`rdma_enN`, macOS 26.2 and
  later; see Apple's [TN3205](https://developer.apple.com/documentation/technotes/tn3205-low-latency-communication-with-rdma-over-thunderbolt)).
  It carries only SENDs on UC queue pairs, three a device on macOS 27.0, so the daemons place each other's writes
  themselves over one queue pair a link; the [fabric guide](fabric.md#thunderbolt-links) describes how.

Either end of a link can be a Mac or a Linux host. The daemons set a link up with a short exchange of IPv6 link-local
datagrams on a Thunderbolt cable. There is no TCP anywhere, and every payload byte moves by RDMA.

Status: compiled and offline-tested with this repository's checks, including a stub verbs library that models
Thunderbolt RDMA as two Studios on macOS 27.0 measured it. Neither link kind has run on hardware in this form. The
RoCE transfer and teardown logic is carried over from an earlier, hardware-tested version; the setup exchange and
Thunderbolt links are new.

## Build and install

On the Mac, `python3 tools/build.py native` builds `build/mcdma-rpcd` and `build/libmcdma-rpc.dylib` with the rest of
the native tools. Both ends can also build with the Makefile, which writes to `build/rpc`:

```bash
make -C rpc
make -C rpc test
sudo make -C rpc install
```

A Linux peer needs `build-essential` and `libibverbs-dev`. `install` copies `mcdma-rpcd` to `/usr/local/bin`,
`libmcdma-rpc` and `libmcdma-fabric` to `/usr/local/lib` and their headers to `/usr/local/include`; set `PREFIX` to
install elsewhere. Everything also runs from `build/rpc` without installing.

## Run a link

Run one listen daemon per link, then one connect daemon with a peer entry for each link. The examples use example
device and interface names; read your own from `ibv_devices`, `ibv_devinfo -v` and `ifconfig`.

### Mac to Mac over Thunderbolt

Enable RDMA in Recovery with `rdma_ctl enable` on both Macs, and make the Thunderbolt Bridge inactive in System
Settings, as TN3205 requires for Macs cabled in loops. Each port `enN` then has its own IPv6 link-local address, and
its RDMA device is `rdma_enN`. A Thunderbolt link must use the device's own port for its exchange.

```bash
# Mac B: link "kv-a" on port en2, the one cabled to Mac A
mcdma-rpcd listen kv-a rdma_en2 1 4096 en2:18620 4 64

# Mac A: its en3 is cabled to Mac B's en2
mcdma-rpcd connect kv-a,en3,18620,rdma_en3,1,4096,4,64
```

The GID index selects one of the port's addresses. TN3205's table for macOS 26 lists the MAC-derived link-local
address at index 0 and the IPv4 link-local one at 1; on macOS 27.0 index 1 was the interface's EUI-64 link-local
address and index 0 the Thunderbolt domain's UUID. Read the table with `ibv_devinfo -v` and use the same index on
both Macs; the qualification used 1. Thunderbolt links reply directly only; `MCDMA_RPC_PULL=1` needs RDMA READ and is
refused.

### Mac to Linux over the ConnectX-5

The RDMA link is the CX5 cable, but the Mac's `mcrdmaN` interface carries no IP traffic, so the two hosts also need a
Thunderbolt or USB4 network cable between them for the exchange: on the Mac a Thunderbolt port `enN`, on Linux the
matching network interface (`thunderbolt0`, or whatever the host names it). That cable carries a few hundred bytes of
datagrams at setup and one small datagram a second afterwards; no payload crosses it.

```bash
# Linux peer: RoCE device rocep1s0f1, exchange on its Thunderbolt network interface
mcdma-rpcd listen worker-a rocep1s0f1 3 4096 thunderbolt0:18620 4 64

# Mac: CX5 device rdma_mcrdma0, exchange on the Thunderbolt port cabled to that peer
mcdma-rpcd connect worker-a,en2,18620,rdma_mcrdma0,0,4096,4,64
```

A peer without Thunderbolt or USB4 networking, such as a DGX Spark, can meet on another shared link, such as the LAN,
when each end pins the other's address there. The datagrams still need a link-local source and hop limit 255, so both
hosts must be on the same layer-2 segment:

```bash
# Linux peer: exchange on its LAN port, admitting only the Mac's link-local address there
mcdma-rpcd listen worker-a rocep1s0f1 1 4096 enP7s7/fe80::aaaa:bbff:fecc:dddd:18620 4 64

# Mac: exchange on its LAN port, pinned to the peer's link-local address there
mcdma-rpcd connect worker-a,en0/fe80::1111:22ff:fe33:4444,18620,rdma_mcrdma0,0,4096,4,64
```

The payload still moves only over the CX5 cable. Anyone on the LAN who forges the pinned address can stop the link
from coming up, but cannot read or write memory, since RDMA packets reach the Mac's CX5 only through that cable.

Mailbox halves are whole multiples of 4 MiB up to 256 MiB, and both ends of a link must use the same sizes. Path MTU
must not exceed the port's active MTU.

Mailboxes and sockets are owner-only, so each daemon must belong to the user whose applications use it. On Linux the
mailbox is registered memory: either run the listen daemon as that user with a locked-memory limit (`ulimit -l`) that
covers both halves, or start it as root with `--owner USER` so the mailbox and socket belong to that user:

```bash
sudo mcdma-rpcd listen --owner worker worker-a rocep1s0f1 3 4096 thunderbolt0:18620 4 64
```

A Mac listen end keeps its mailbox in POSIX shared memory, `/mcdma-rpc.NAME`, the same as a Mac connect end; run it
as the applications' user. `--owner` is for Linux.

## Who can connect

Before this release a listen daemon took control connections on a TCP port, and anyone who reached that port could
write into its mailbox, so it had to be bound to one address and firewalled. The exchange that replaced it admits a
datagram only when all of these hold:

1. It arrived on the named interface.
2. It came from an IPv6 link-local address, which no router forwards.
3. Its hop limit is 255. Both ends send with 255, and any router on the way would have lowered it, the same check
   IPv6 neighbor discovery makes.
4. It names this link.
5. When the interface is given as `IFACE/fe80::ADDR`, it came from that address.

On a point-to-point Thunderbolt cable with the Thunderbolt Bridge inactive, only the machine at the other end of the
cable can pass, so a firewall rule is no longer needed. Each end also picks a random 128-bit session identifier for
every attempt, and messages for any other session are ignored, so a stale or replayed datagram cannot act on a live
link. Ignored datagrams are counted and logged at most every ten seconds.

On a Mac the exchange runs over Thunderbolt: Apple names each Thunderbolt RDMA device after its port, so a
Thunderbolt link must give its device's own port (`rdma_en2` with `en2`), and a CX5 link a port that has an `rdma_`
device, or any interface with the peer's address pinned (`IFACE/fe80::ADDR` or `IFACE/A.B.C.D`). A daemon given any
other interface refuses to start. Linux cannot check this the same way; give it the Thunderbolt or USB4 interface,
or the pinned shared link.

## Metal and GPU memory

On macOS, `libmcdma-rpc` also offers `mcdma_rpc_metal_wrap`, `mcdma_rpc_metal_contents` and `mcdma_rpc_metal_release`.
They make a Metal buffer over mailbox memory without a copy, so a GPU reads what the NIC wrote: oMLX imports the reply
half into MLX through DLPack and copies large frames into its own buffers on the GPU. The buffer is refused unless it is
the caller's memory. Helpers built before these functions simply lack them, and callers fall back to a CPU copy.

On Linux, `rpc/gpu` offers the same for CUDA mapped host memory and Vulkan storage buffers; see
[its guide](../rpc/gpu/README.md).

## Status and shutdown

```bash
printf 'STATUS\n' | nc -U /tmp/mcdma-rpcd.sock
printf 'SHUTDOWN\n' | nc -U /tmp/mcdma-rpcd.sock
```

`STATUS` prints a `VERSION` line, one `PEER` line per link and `END`:

```text
VERSION mcdma-rpcd 1 2.0.0
PEER NAME up|down calls N failures N MiB N via=VIA port=PORT device=DEVICE link=roce|thunderbolt req_mib=R rep_mib=P since=EPOCH
END
```

`since` is the time the link came up, or 0 while it is down. The listen daemon answers the same commands on
`/tmp/mcdma-rpcd.NAME.sock`; its `PEER` line adds `service=attached|none`. `MCDMA_RPCD_SOCKET` replaces either
socket path.

One daemon serves each link name. A daemon takes the lock file `/tmp/mcdma-rpc.NAME.lock` for every link before it
touches a device or mailbox, and refuses to start while another daemon holds it or answers on its socket, so a running
daemon is never orphaned. The lock files stay behind after a daemon stops; they are harmless.

`SHUTDOWN`, `SIGINT` and `SIGTERM` all stop a daemon the same orderly way: every queue pair is drained or flushed and
destroyed before it exits, its peer is told goodbye, and every wait is bounded, so it cannot hang on a silent peer.
`SIGHUP` is ignored, so a closed terminal or SSH session leaves the daemon running. Never use `SIGKILL`, which skips the
teardown.

## Protocol 1

Each link has one mailbox: a request half of `R` bytes followed by a reply half of `P` bytes, each starting with a
4 KiB control page. A word is `seq << 32 | length`; sequence 0 means empty.

| Offset | End | Meaning |
| --- | --- | --- |
| request +0 | both | Request word: staged by the client, landed at the service |
| request +64 | connect | 1 while the daemon's link to the peer is up |
| request +72 | connect | Link generation, bumped each time the link comes up |
| request +256 | both | `R` and `P` as two little-endian u64 values |
| reply +0 | both | Pull mode: the peer's ready word |
| reply +64 | connect | Done word: the service's reply has landed |
| reply +128 | listen | Staged word: a reply is ready for the daemon to send |

The connect end's mailbox is the POSIX shared memory object `/mcdma-rpc.NAME`. The listen end's is the file
`/dev/shm/mcdma-rpc.NAME` on Linux and the POSIX shared memory object `/mcdma-rpc.NAME` on a Mac. Write a payload
before its word, store words with `mcdma_rpc_store_word` and wait on them with `mcdma_rpc_wait_word`.

A client call:

1. Copy the request after the request half's control page and store its word with a new sequence.
2. Wait for the done word (reply +64) to carry that sequence, then read the reply after the reply half's control page.
   A request staged before a reconnect is lost: if the link generation (request +72) changes while you wait, fail
   the call instead of waiting on.

A service:

1. Connect to the listen daemon's socket and send `MODE poll`. The daemon answers `OK`, or `ERR busy` while another
   service holds the link. After `OK` it sends nothing until the registration ends with `BYE` or a closed socket,
   which happens when the link drops: requests in flight are lost with it, so fail the work rather than wait.
2. Wait for the request word to carry a sequence other than the last one served, and read the request.
3. Copy the reply after the reply half's control page and store the staged word (reply +128) with the request's
   sequence.

By default the listen daemon writes each reply and then the client's done word straight into the other end's reply
half. On RoCE that relies on the Mac's NIC not reordering its PCIe writes (`MCDMARelaxedOrdering = No`, the default);
`MCDMA_RPC_PULL=1` on the connect daemon makes the peer send only its ready word and the connect end read the payload
instead. On Thunderbolt the receiving daemon applies the word only after the payload has landed.

## The setup exchange

The daemons exchange IPv6 UDP datagrams of at most 1,232 bytes, so none needs fragments. Each starts with a 64-byte
header: magic `MCDX`, version 1, a kind, a role (connect, listen or fabric peer), flags, the link name, the sender's
session and the session it believes the receiver has. An offer adds the sender's link kind and reply mode, GID, LID,
queue pair numbers, starting PSN, Thunderbolt receive depth, mailbox sizes and a description of its registered
mailbox: base address, length, piece size and, for RoCE, one key per piece. A key is never handed out for a piece the
peer may not use: a connect end gives none for its request half.

1. The connect end creates its queue pairs and offers every 100 ms: to `ff02::1` on the interface, which on a
   Thunderbolt cable reaches only the other end, or to the configured address.
2. The listen end checks the offer against its own mailbox, creates its queue pairs and takes them to RTS, then
   answers with its own offer. A refusal (`mailbox sizes differ`, `the two ends use different link kinds`, `pull mode
   needs RDMA READ, which Thunderbolt lacks`) is sent back as an error, and the connect end tries again two seconds
   later.
3. The connect end takes its queue pairs to RTR and RTS and confirms with its offer again. It posts nothing before its
   own RTS, and nothing before the listen end has answered from RTS.
4. The listen end arms on the confirmation, and offers again until it hears one. It writes no reply before it is
   armed.

A new session from the connect end replaces the old one, which is how a restarted connect end recovers at once. An
exchange that does not finish within ten seconds is dropped on both sides. While a link is up the connect end pings
every second and the listen end answers; either end declares the link down after six seconds without hearing from the
other. A daemon that stops or drops a link says goodbye, so the other end does not wait out that timeout, and a ping
from an unknown session is answered with goodbye.

## Qualifying a link on hardware

`build/rpc/rpc-echo` answers and checks calls through real daemons: run `rpc-echo service NAME` beside the listen
daemon and `rpc-echo client NAME [CALLS [SIZES]]` beside the connect daemon. The client checks every byte of every
reply and prints `rpc-echo: calls=N errors=0 median_us=... p99_us=...`; it exits 0 only when every reply was right.
`build/rpc/fabric-check` qualifies the one-sided path in [the fabric guide](fabric.md).
