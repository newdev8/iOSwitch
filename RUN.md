# Running this checkout

Prepared 2026-11-16. HEAD `c1267f5`, module version `0.1.0`.

## Quick start

```sh
./run-image.sh --dry-run                  # the plan, about 30 s
./run-image.sh --ios27 --out out/sd       # iPadOS 27 from .release-ios27/ (fetch-ios27.sh)
./run-image.sh --out out/sd               # the image: downloads the IPSW's members, ~35 GB free
./run-image.sh --out out/sd --to-card disk4 --yes   # when the Switch SD card is attached
```

`disk4` is an example; use the card's disk from `diskutil list`. Without
`--to-card`, merge `out/sd` onto the card's FAT partition with `ditto`:

```sh
ditto --norsrc --noextattr out/sd "/Volumes/SWITCH SD"
```

Then boot hekate, open **Payloads**, run `install-ios.bin`, and choose
**More Configs -> iOS** after it finishes.

On Intel, `run-image.sh` skips building the Metal shader seed (its
`precompile`/`rawpack` tools are arm64-only) unless the release directory
supplies one. This build embeds CI's seed from `.release-ios27/seed/tree`.

## Built

`out/sd` is complete: iPadOS 27.2 (24B119), 21.6 GiB in six parts, with the
Metal shader seed.

```text
switchroot/ios/IOS27.SII.001   4.0G
switchroot/ios/IOS27.SII.002   4.0G
switchroot/ios/IOS27.SII.003   4.0G
switchroot/ios/IOS27.SII.004   4.0G
switchroot/ios/IOS27.SII.005   4.0G
switchroot/ios/IOS27.SII.006   1.7G
bootloader/payloads/install-ios.bin
bootloader/ini/ios.ini
bootloader/res/icon_ios_hue.bmp
```

SHA-256:

```text
dcb3d6753ab89309c9a2db97931b06b7e6ba3940d7c597dd15750f5d888e3ed6  IOS27.SII.001
1db4be55027dc7606492a242321b2f9cfc8fb6d58fcc5572a3b512535fd9f10a  IOS27.SII.002
9d0262e6780737653e21ffa8426bd38507ee6fb526ca0d4d4755dfdb59b07b26  IOS27.SII.003
54df1b4617106d8c773a29f42cd0033fbe2b806c784a694963706200e5e6d1a8  IOS27.SII.004
9e6cd95e5f62f0369e381c0e4ac6817fd6eeb24032be205f49a15be817ac5cd7  IOS27.SII.005
39be313223591a110abac5fd915f4978dcf48e53ff31031fdbd31d3e2180f279  IOS27.SII.006
7586db8a16a429e6bafe5a1a81d8c7c828e3a19a97a6374a4951b6d74e0ad2b9  install-ios.bin
```

The image needs an iOS partition of **27517 MiB (26.9 GiB) or more**.

The kernel is CI's prelinked `.release-ios27/kernel.macho`; the two sealed
cryptexes are `.release-ios27/translated/{app,system}_cryptex/*.dmg`. Packing
was done on this Intel Mac from those inputs, so the seed and the translated
System files are in the image rather than compiled on first boot.

The previous build, iPadOS 17.7.11 (21H461), was 8.7 GB in three parts:
`c18cbde1…`, `d78ca2b3…`, `6279d051…`. Its parts are no longer in this
checkout.

With the card attached:

```sh
diskutil list external
./run-image.sh --out out/sd --to-card disk4          # read-only plan
./run-image.sh --out out/sd --to-card disk4 --yes    # write and eject
```

`--to-card` asks for `sudo` in the terminal, so run it there rather than from
an agent. It only writes the FAT partition and never repartitions the card.

## What is set up

- `.tools/bin/bazelisk` 1.30.0, using Bazel 9.3.1.
- `.tools/bin/gh` 2.103.3.
- `.tools/bin/apfs_tool` and `.tools/bin/lz4`, built x86_64 for this Mac.
- `.release/`: the CI `release-assets` artifact, SHA-256 verified, with its
  arm64 `apfs_tool` and `lz4` replaced by the x86_64 builds and `SHA256SUMS`
  updated to match. Its `shader/` tools stay arm64; Intel builds skip the seed.
  `image-inputs.tar` is extracted at `.release/inputs/` for `--gpu-base`.
- `build-host-tools.sh` rebuilds both host tools.
- `fetch-assets.sh` downloads the CI artifact again and does the same swaps.
- `run-image.sh` wraps the image builder: release directory, lockfile, Xcode
  bypass, disk gate, and `image_to_card`.

The prebuilt kernel, GPU bundle, loader, overlay and host tools come from the
CI artifact because GitHub has no release for `0.1.0`. The iPadOS 17 artifacts
are from run `42730816090`; the iPadOS 27 inputs in `.release-ios27/` are from
run `43663856765` (assets and seed; the translated cryptexes were rebuilt and
sealed locally because that job ran out of runner disk).

## Notes

- Switch **OLED only**. It needs a hekate setup and a microSD with a 32 GB or
  larger partition for iOS. Copy the card's contents somewhere safe first.
- The installation docs mention `bazel run //:image -- --copy-to-card`, but
  this checkout's `tools/install/image.py` does not implement that flag. Use
  `//:image_to_card` through `run-image.sh --to-card`.
- The checked-in `MODULE.bazel.lock` is stale for `install/release.bzl`;
  `run-image.sh` passes `--lockfile_mode=off`.
- A release build runs no Xcode tool, but the repository rule checks the
  selected Xcode. `run-image.sh` sets `SWITCH_ALLOW_UNPINNED_XCODE=1` for
  `--release`; a source build still needs Xcode 27.2 (`--local`).
- Bazel caches the external release repository. After replacing files in
  `.release/`, shut Bazel down and remove
  `<output_base>/external/@+release+switch_release.marker`; `fetch-assets.sh`
  does this itself.
- `run-image.sh` refuses under 35 GiB by default. The builder's own
  `space.check_run` is exact; override with `SWITCH_MIN_FREE_GB=<n>` when the
  disk is known to be large enough.
