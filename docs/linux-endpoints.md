# Linux endpoints in MCDMA

CLI 1.2 adds Linux RDMA peers, including a Strix Halo host, to the existing MCDMA fabric. Mac/Linux links use the Mac's MCDMA driver and Linux's stock verbs provider; Linux/Linux links use stock libibverbs on both endpoints. The CPU submits work and observes NIC completions. Adding a Linux endpoint does not require a CUDA NIC-register mapping, a private GPU driver or a security-policy change.

## Register and build

Use your existing key-authenticated SSH aliases. Supply `--ssh-config /absolute/path/to/private/config` when the aliases live outside the default OpenSSH configuration. Credentials stay in SSH; the CLI stores host aliases and tool paths.

```sh
mcdma linux add strix-host --name strix
mcdma linux add spark-host --name spark
mcdma linux install strix
mcdma linux install spark
mcdma linux list
```

The installer compiles the bundled peer source on each Linux machine using its native C compiler and libibverbs development headers. It validates the protocol marker, records the binary hash and architecture, and installs into that SSH user's `~/.local/libexec/mcdma`. It preserves a previous binary before atomic replacement. It does not copy the ARM64 Spark package onto x86-64 Strix, install a kernel driver or create a service. Supported native build architectures are x86-64 and ARM64; other architectures are rejected.

If the Spark is already registered with `mcdma sparks add`, keep that entry and install its native peer with `mcdma linux install EXISTING_ID`. Existing Mac mappings and settings remain usable.

## Strix to Spark

Cable an Ethernet/RoCE port on the Strix ConnectX adapter to the Spark, or use the intended RoCE network. Discovery reads each RDMA interface's actual port and complete GID table, including its network-device association. It requires an observed link-local RoCE v2 GID; it does not substitute an IPv4 GID or an unrelated interface.

An unambiguous cable match involving a registered Linux endpoint appears automatically. Choose explicitly when cable metadata is unavailable:

```sh
mcdma linux link strix/STRIX_INTERFACE spark/SPARK_INTERFACE --name strix-spark
mcdma linux status
mcdma linux test strix-spark --quick
mcdma linux test strix-spark --json
```

Use interface names reported by `mcdma linux list`. `--quick` verifies WRITE and READ from both endpoints without timing. The full test records 1,000 latency samples per operation after 100 warmups, with byte verification, binary identities and both endpoints' cleanup exits. Timing measures CPU submission through application-observed NIC completion, excluding registration, GPU production/consumption and inference. It is not one-way network time or GPU-to-GPU latency.

Normal Linux neighbour discovery is sufficient when both endpoints resolve the selected peer's link-local address to its observed MAC. The test performs a bounded IPv6 probe when required and verifies the kernel neighbour record before creating its RDMA peers. A wrong MAC, unusable neighbour state, missing GID or inactive port fails explicitly. Permanent neighbours are optional for Linux/Linux links:

```sh
mcdma linux configure strix-spark
```

That command requires root SSH or locally authenticated sudo. It preserves existing MCDMA neighbour entries, including Studio/Spark links, and installs the existing MCDMA persistence helper. It returns exit 3 when privileged access is unavailable; it does not change sudo policy or collect passwords.

## Strix to Mac

Register the Strix Linux endpoint and map a connected Mac port to it using the existing MCDMA flow:

```sh
mcdma linux add strix-host --name strix
mcdma linux install strix
mcdma status
mcdma map local:MAC_INTERFACE strix/STRIX_INTERFACE
mcdma configure
mcdma test local:MAC_INTERFACE
```

Use `--studio-host MAC_SSH_ALIAS` to manage a remote Mac. The Mac still needs its approved, loaded MCDMA driver and appropriate setup; Linux peer registration does not change that installation recipe. The physical link must exist before validation. The existing Mac initiator device guard and provider-mode checks remain in force.

## Application integration

The public `mcdma-rpcd` link daemon already builds on Linux and macOS. A Strix application can use the same request/reply transport as the existing Spark/Mac integration; see [the link daemon](link-daemon.md). The Mac buffer ownership contract is in [the helper API](../rpc/mcdma_rpc.h). Plain host-buffer CLI verification does not establish GPU-buffer correctness or integrate an inference engine automatically.

CUDA/Vulkan buffer APIs and the pinned native llama.cpp overlay are proposed separately in [the native GPU integration pull request](https://github.com/ashhart/MCDMA/pull/8). Their allocation, ownership and validation requirements remain distinct from this Linux endpoint feature. Resident-kernel GPU ping-pong measurements are research evidence, not an installed inference latency promise. Use the accepted CPU-submitted transport as the default and validate the real model before claiming an inference speedup.

Raw JSON includes private host/device identities and retained samples. Keep settings and results outside publication candidates. Saved verification expires when endpoint, GID, boot or peer-tool identity changes.

## Software transports (opt-in)

`mcdma linux test [LINK] --soft` sets `MCDMA_SOFT_TRANSPORT=1` for both peers. A soft run skips the Mellanox identity check, so software transports such as Soft-RoCE (`rdma_rxe`) or siw can be verified end to end; the peer labels its `LINUX_PEER_CONFIG` evidence with `soft_transport=1` and its real vendor id, and the CLI refuses a soft run whose peers did not confirm that label. A run without `--soft` is unchanged and still requires Mellanox hardware. Soft transports are for bring-up, correctness and baselines; keep their numbers separate from hardware results.
