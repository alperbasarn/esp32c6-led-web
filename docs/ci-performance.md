# CI performance & scaling options

How long the firmware pipeline takes, why, and the levers for making it faster —
including the self-hosted-runner (PC / Raspberry Pi) option, captured here so the
trade-offs aren't lost.

## Current pipeline

The `Publish Firmware` workflow builds inside a prebuilt GHCR container image
(`ghcr.io/alperbasarn/esp32c6-matter-builder`) that bakes ESP-IDF v5.4.1 +
esp-matter, so no per-build clone/install is needed. See
[`.github/workflows/publish-firmware.yml`](../.github/workflows/publish-firmware.yml)
and [`docker/builder.Dockerfile`](../docker/builder.Dockerfile).

### Measured timings (GitHub-hosted `ubuntu-latest`, 4 vCPU)

| Stage | Old (clone-based) | Current (container) |
|-------|-------------------|---------------------|
| esp-matter checkout + install | ~11 min | — (baked in image) |
| Image pull ("Initialize containers") | — | **~7.6 min** |
| Compile ("Build firmware") | ~7 min | **~6 min** |
| **Total release build** | **~18 min** | **~14 min** |

The container migration removed the fragile per-build clone but **moved** the cost
into the image pull — the image is multi-GB and GitHub runners are ephemeral, so
it is re-downloaded and re-decompressed every run. So the two remaining costs are:

1. **Image pull (~7.6 min)** — dominated by gzip decompression (effectively
   single-threaded) of a multi-GB toolchain image.
2. **Compile (~6 min)** — almost entirely esp-matter/CHIP C++, which rarely changes.

## Optimizations already applied

- **Prebuilt builder image** — no per-build esp-matter clone/install.
- **Registry layer cache** (`cache-to/from type=registry`) — image *rebuilds*
  reuse the expensive esp-matter layer instead of re-fetching (~15 min → ~2 min),
  stored in GHCR so the 10 GB Actions-cache limit doesn't apply. Requires BuildKit's
  `docker-container` driver (`docker/setup-buildx-action`).
- **Single build per release** — dropped the `push: main` trigger, which used to
  build the same commit twice (version-bump push + tag).

## In-progress experiment: `ci/builder-image-speedup` branch

Two changes under validation on that branch (isolated behind a `:dev` image tag so
releases are untouched):

- **ccache** — `ccache` installed in the image, `IDF_CCACHE_ENABLE=1`, and
  `CCACHE_DIR` persisted across runs via `actions/cache`. esp-matter/CHIP objects
  rarely change, so a warm cache should cut the ~6 min compile to **~1–2 min**.
  *(Warm-run measurement pending.)*
- **Image prune** — remove `__pycache__` / pip caches from the install layer.
  Preliminary: pull dropped to **~1.6 min** (from ~7.6 min) — larger than expected;
  being confirmed.

If both hold up, a release build would land around **~4–6 min** on GitHub-hosted
runners. Merge to `main` restores the production image tags in these two files.

## Reducing the image fetch further

On GitHub-hosted runners the pull is structural (fresh machine every run):

- **zstd layer compression** (free, not yet applied) — `compression=zstd` in the
  image build; zstd decompresses far faster than gzip → ~30–50% faster pull.
- **Deeper prune / multi-stage** — diminishing returns and some risk (the CHIP
  build occasionally queries git metadata, so dropping `.git` is not free).
- **Floor** on hosted runners with all of the above: ~3–4 min pull. Not zero.

## Self-hosted runner (PC now, Raspberry Pi later)

The only way to eliminate the pull entirely, because a self-hosted runner keeps
state between jobs.

### Would it speed things up? Yes — substantially

| Cost | GitHub-hosted | Self-hosted (your PC) |
|------|---------------|------------------------|
| Image pull | ~7.6 min every run | pulled **once, ever** → ~0 s after |
| ccache | `actions/cache`, evicts in 7 days | **persistent on disk**, always warm |
| Compile | 4 vCPU | your cores (typically 2–4× faster) |

Realistic outcome: **~14 min → ~2–4 min**, most of which is just the compile.

### What you compromise

1. **Security — the decisive factor, because this repo is public.** A self-hosted
   runner executes whatever a workflow runs. With the `pull_request` build gate,
   **any fork PR can run code on your machine.** Worse, the release job holds
   `MANIFEST_SIGNING_KEY` — the root of OTA trust; a machine that ever runs
   untrusted code could **leak the signing key**, letting an attacker sign firmware
   your devices will install. Mitigations:
   - GitHub → Settings → Actions → **require approval for all outside contributors**.
   - Keep the **release/signing job on GitHub-hosted**; use self-hosted only for the
     PR/build fast-feedback loop (split model).
2. **Uptime** — jobs only run when the machine is on; a release fired while it's off
   just queues.
3. **Maintenance** — you own Docker, disk cleanup (image ~5–6 GB + ccache + build
   dirs accumulate), and runner-agent updates.
4. **Resource use** — builds peg CPU/RAM/disk on the machine while running.

### Raspberry Pi — not recommended for this build

- **Architecture:** the Pi is arm64; esp-matter/CHIP's toolchain (gn, clang,
  pigweed CIPD packages) has patchy arm64-host support and may not build. A separate
  arm64 builder image would be required.
- **Speed:** compiling all of CHIP/Matter on a Pi is ~30–60+ min — *slower* than
  GitHub even with a free pull. Pis are fine for light jobs; a Matter compile is not.

### Paid alternatives (small fee)

| Option | Effect | Cost (verify current) | Caveat |
|--------|--------|-----------------------|--------|
| Cache-persistent CI (Depot, Namespace, Blacksmith) | Warm image/layer cache next to the runner → pull in seconds; fast machines. One-line `runs-on:` change | ~$10–20/mo entry | 3rd-party runs CI; minor lock-in |
| Dedicated VPS as self-hosted runner (~€5/mo) | Persistent image + ccache; isolated/disposable, always on | ~€5/mo | Same public-repo security rules as above |
| GitHub larger runners (8–16 core) | Faster decompress + compile; still re-pulls every run | per-minute premium | Least effective per € — doesn't fix the re-pull |

## Recommendation / decision log

- **Now:** land the `ccache` + `prune` experiment (biggest free wins) and add zstd.
- **Self-hosted PC / RPi migration:** **parked** (2026-07). If revisited, prefer a
  cheap dedicated VPS over a personal machine, with fork-PR approval on and the
  signing job kept on GitHub-hosted. Skip the RPi for this pipeline (arm64 +
  slow compile).
