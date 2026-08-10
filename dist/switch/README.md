# Nintendo Switch build

The launcher and runtime use the lowercase directory `sdmc:/switch/cemu/`.
Install the final `cemu.nro` as `sdmc:/switch/cemu/cemu.nro`.

## Vulkan SDK

Extract the complete Mesa 26.2.0 Switch Vulkan SDK into
`dependencies/switch_mesa_vulkan/`, then verify it with:

```sh
./dist/switch/deps/prepare_nvk.sh --check
```

The private driver payload is ignored by Git. The build localizes its archives
into one link object without adding those archives to the repository.

## LSFG-VK

LSFG support is built from `third_party/lsfg-vk`. Supply your own compatible
`Lossless.dll` at `sdmc:/switch/cemu/lsfg/Lossless.dll`. Enable LSFG preparation
in **Settings > Frame Generation**, then enable frame generation at runtime from
the in-game quick menu. It always starts disabled for each game.

The vendored LSFG-VK code is GPL-3.0-or-later; see
`third_party/lsfg-vk/LICENSE.md`.

## Build

```sh
BUILD_JOBS=18 ./build_switch_all.sh
```

The finished all-in-one launcher is `switch_launcher/cemu.nro`.

## Launcher updates

The SDL launcher checks the latest published release from
`NaGaa95/Cemu-nx`. Upload the all-in-one launcher as a `.nro` release asset;
`cemu.nro` is preferred when a release contains more than one NRO.

Set `RELEASE_VERSION` to the GitHub tag when making a release:

```sh
RELEASE_VERSION=1.1.2 BUILD_JOBS=18 ./build_switch_all.sh
```

The updater requires GitHub's SHA-256 asset digest and validates both the
digest and NRO structure before replacing the installed launcher.
