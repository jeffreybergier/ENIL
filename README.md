# ENIL



Build roots are resolved from the project Makefile and inherited by child makes.
`BUILD_ROOT=custom` is relative to the repository root. Native artifacts live in
`build/<platform>/<configuration>/<product>/`; `BUILD_VARIANT=name` keeps an
additional build variant. Use `make macOS-clean PRODUCT=<name>` for one product,
`make macOS-clean` for a platform, or `make clean-all` for the whole build root.
CI stages release assets in `build/release` (including the release manifest).

Run `make benchmark-render` for the offline Linux 2,000-message rendering
benchmark. See [workload, baseline and comparison instructions](docs/render-benchmark.md).
