# Vendored Sailfish-specific libraries

The bridge links against two libraries that are **not** in a normal Linux distro and whose ABI is
tied to the Sailfish OS release:

| File | Origin |
|------|--------|
| `lib/libgbinder.so.1`   | `libgbinder` (github.com/mer-hybris/libgbinder), BSD-licensed |
| `lib/libglibutil.so.1`  | `libglibutil` (github.com/sailfishos/libglibutil), BSD-licensed |
| `include/gbinder_*.h`, `include/gbinder.h` | matching `libgbinder` public headers |
| `include/gutil_*.h`     | matching `libglibutil` public headers |

The `.so` files here were taken from **Sailfish OS 5.2.0.22 (aarch64, Jolla Phone 2026 / MT6858)** —
i.e. the exact binaries from `/usr/lib64` on the device. Linking against them makes the built
`appsupport-bridge-bt` ABI-matched to that release. Everything else (glib / gobject / gio) is
standard and comes from the distro at build time.

Built with:

    make MODE=vendored-release CROSS=        # native aarch64 (e.g. the CI arm64 container)

## Refreshing for a new OS release

If you move to a Sailfish release that bumps the **major** soname (e.g. `libgbinder.so.2`), refresh
these from the device:

    scp 'defaultuser@<phone>:/usr/lib64/libgbinder.so.1*'  vendor/lib/libgbinder.so.1
    scp 'defaultuser@<phone>:/usr/lib64/libglibutil.so.1*' vendor/lib/libglibutil.so.1
    # headers: from the matching upstream git tag, or the device's -devel packages

Minor updates generally keep the same soname and stay ABI-compatible, so a refresh is usually only
needed on a major bump or if the build/runtime complains about missing symbols.
