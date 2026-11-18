# Running this checkout

Prepared 2026-11-16; the iPadOS 27 image was rebuilt 2026-11-18. Module version `0.1.0`.

## Quick start

```sh
./fetch-ios27.sh                          # .release-ios27/ from the newest "Build iOS 27 inputs" run
./compress-seed.sh                        # the seed shrinks about three times, if it is not already compressed
./run-image.sh --ios27 --dry-run          # the plan, about 30 s
./run-image.sh --ios27 --out out/sd       # iPadOS 27, needs about 35 GB free
./run-image.sh --ios27 --out out/sd --to-card disk4 --yes   # when the Switch card is attached
```

`disk4` is an example; use the card's disk from `diskutil list`. Without
`--to-card`, merge `out/sd` onto the card's FAT partition with `ditto`:

```sh
ditto --norsrc --noextattr out/sd "/Volumes/SWITCH SD"
```

Then boot hekate, open **Payloads**, run `install-ios.bin`, and choose
**More Configs -> iOS** after it finishes.

## Built

`out/sd` is complete: iPadOS 27.2 (24B119), **18.72 GiB in five parts**, with
the Metal shader seed compressed, and a minimum partition of **48 GiB
(49,152 MiB)**.

```text
switchroot/ios/IOS27.SII.001   4.0G
switchroot/ios/IOS27.SII.002   4.0G
switchroot/ios/IOS27.SII.003   4.0G
switchroot/ios/IOS27.SII.004   4.0G
switchroot/ios/IOS27.SII.005   2.7G
bootloader/payloads/install-ios.bin
bootloader/ini/ios.ini
bootloader/res/icon_ios_hue.bmp
```

SHA-256:

```text
e02ad9c9a047a437071ce28b687c146daf0abb4efd914b7a249ea2de2dc5c5ce  IOS27.SII.001
2b69dccd16395d518d76e8a6e363857066a73f89564f9fc4425a9a3a97191e6d  IOS27.SII.002
1ba7662e47c8d087818e0c0389e766c29edf8011408f547b8a2a31976a1dad08  IOS27.SII.003
5e396c8e8dd4456a7a23857e21cd81467a29bdb1ba3f2e178df605091c012f9f  IOS27.SII.004
e9944248a118acbd248b15e08d8fced8bcda8a374f4d7cad1db71cbdc87ae6e0  IOS27.SII.005
7586db8a16a429e6bafe5a1a81d8c7c828e3a19a97a6374a4951b6d74e0ad2b9  install-ios.bin
```

The five parts hold 20,095,401,984 bytes: ten extents, 2,387 hashed chunks,
18.64 GiB of source blocks. The manifest's `container_bytes` is 1 TiB and its
`minimum_bytes` is 51,539,607,552, so `install-ios.bin` accepts a partition of
48 GiB or more and only writes the blocks the image uses. The fitted proof ran
`fsck_apfs` on a 49,152 MiB clone and it was clean; all 2,387 chunk hashes match
the packed stream.

The kernel is CI run `44001779619`'s prelinked `.release-ios27/kernel.macho`
(SHA-256 `052bc522…`); the two sealed cryptexes are
`.release-ios27/translated/{app,system}_cryptex/*.dmg`. Populate takes 94.7 s,
pack 79 s.

### Loader

The release loader's compiled-in window holds the cdhash of the `installd` CI
re-signed, so it refuses a window staged by another `codesign`. This Mac has
Xcode 14.3.1, not the pinned 27.2, and `run-image.sh` compiled the loader here
with `--//boot:local_loader=true` and `--//boot:composition=release`. The local
Xcode's `-O2` output outgrew the U-Boot stamps slot, so the loader builds with
`-Oz` (`boot/defs.bzl`). The link used the x86_64 LLVM tools through
`SWITCH_LLVM_REPO` because the release's `@llvm` archive is arm64-only.

### Seed

CI's seed is 5.24 GiB logical; `compress-seed.sh` writes APFS transparent
compression, leaving about 1.95 GiB on disk, and `populate.merge_tree` copies
with `ditto --rsrc --extattr --acl` so the `compressed` flag survives into Data
and the packed image. The populated container was 18.65 GiB, in line with the
compressed seed rather than its logical size.

`stages.pack` now measures the parts it will write (`pack.planned_bytes`)
instead of charging `minimum_bytes`. With the 48 GiB floor above the
container's used blocks, the old check reserved the floor itself, 38 GiB for
19 GiB of parts.

The previous build, iPadOS 17.7.11 (21H461), was 8.7 GB in three parts:
`c18cbde1…`, `d78ca2b3…`, `6279d051…`. Its source is `.release/`, and rebuilding
it with this checkout also enforces the 48 GiB floor.

With the card attached:

```sh
diskutil list external
./run-image.sh --ios27 --out out/sd --to-card disk4          # read-only plan
./run-image.sh --ios27 --out out/sd --to-card disk4 --yes    # write and eject
```

`--to-card` asks for `sudo` in the terminal, so run it there rather than from
an agent. It only writes the FAT partition and never repartitions the card.

## What is set up

- `.tools/bin/bazelisk` 1.30.0, using Bazel 9.3.1.
- `.tools/bin/gh` 2.103.3.
- `.tools/bin/apfs_tool` and `.tools/bin/lz4`, built x86_64 for this Mac.
- `.release/`: the iPadOS 17 `release-assets` artifact, SHA-256 verified, with
  its arm64 `apfs_tool` and `lz4` replaced by the x86_64 builds and
  `SHA256SUMS` updated to match.
- `.release-ios27/`: the iPadOS 27 inputs from CI run `44001779619` (assets,
  kernel, seed, translated cryptexes) and the x86_64 host tools above.
- `build-host-tools.sh` rebuilds both host tools.
- `fetch-assets.sh` downloads the iPadOS 17 CI artifact and does the same
  swaps; `fetch-ios27.sh` fills `.release-ios27/`.
- `run-image.sh` wraps the image builder: release directory, lockfile, Xcode
  bypass, loader/LLVM selection, disk gate, and `image_to_card`.

## Notes

- Switch **OLED only**. It needs a hekate setup and a microSD of **128 GB or
  more**: the parts are 18.72 GiB and the iOS partition needs 48 GiB, so the
  two together exceed a 64 GB card's usable space while the install runs. Copy
  the card's contents somewhere safe first.
- The installation docs mention `bazel run //:image -- --copy-to-card`, but
  this checkout's `tools/install/image.py` does not implement that flag. Use
  `//:image_to_card` through `run-image.sh --to-card`.
- A release build runs no Xcode tool, but the repository rule checks the
  selected Xcode. `run-image.sh` sets `SWITCH_ALLOW_UNPINNED_XCODE=1` for
  `--release`; a source build still needs Xcode 27.2 (`--local`).
- Bazel caches the external release repository. After replacing files in
  `.release/` or `.release-ios27/`, shut Bazel down and remove
  `<output_base>/external/@+release+switch_release.marker`; the fetch scripts
  do this themselves.
- `run-image.sh` refuses under 35 GiB by default. The measured run writes 22
  GiB on top of Bazel's 8.1 GiB of outputs; the builder's own `space.check_run`
  is exact. Override with `SWITCH_MIN_FREE_GB=<n>` when the disk is known to
  be large enough.
