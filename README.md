# STM32 NUCLEO-H563ZI flexPTP demo

## What's this?

> **This is a [flexPTP](https://github.com/epagris/flexPTP) demo project showcasing the capabilities of the flexPTP [IEEE 1588 Precision Time Protocol](https://ieeexplore.ieee.org/document/9120376) implementation for the [STMicroelectronics NUCLEO-H563ZI](https://www.st.com/en/evaluation-tools/nucleo-h563zi.html) devboard.**

Still not clear what is it useful for? No worries, it's a behind-the-scenes support technology that you use unaware every day if you have a smartphone or when you are connected to the internet. Modern telecommunication and measurement systems often rely on precise time synchronization down to the nanoseconds' scale. Methods got standardized by the IEEE and now it's known by the name of the Precision Time Protocol. This software project is an evaluation environment to showcase the capabilities of our IEEE 1588 PTP implementation named `flexPTP` on the STMicroelectronics NUCLEO-H563ZI board.

This project is a port of [flexPTP-demo-NUCLEO-H743ZI](https://github.com/epagris/flexPTP-demo-NUCLEO-H743ZI)
from the STM32H743 (Cortex-M7) to the STM32H563 (Cortex-M33). It was then ported a second time,
from FreeRTOS and lwIP to **Eclipse ThreadX and Eclipse NetX Duo**, and today it builds **two
firmwares** for the same board, one on each stack (see [Software structure](#software-structure)).

The firmware documentation lives in
[`projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/architecture.md),
beside the ThreadX firmware it was written for; the host setup, profile, performance and testing
documents there cover both firmwares. Repository-level documents — the Bazel migration
[plan](docs/bazel_migration.md) and its [rationale](docs/bazel_decisions.md), and the
[flexPTP upstream review](docs/flexptp_upstream_review.md) — are in `docs/`. The full list is
under [Documentation](#documentation).

Start with
[`architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/architecture.md) for how the
firmware fits together, or
[`testing_architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md)
for how it is verified and how to repeat that — including what each layer of testing *cannot*
tell you.

### Get the sources

```bash
git clone <this-repository> flexPTP-demo-NUCLEO-H563ZI
cd flexPTP-demo-NUCLEO-H563ZI
```

That is the whole of it. There are no submodules to initialise and no patches to apply by hand.

Both used to be required, and getting them wrong was dangerous rather than merely inconvenient:
the vendored trees were git submodules, two of them carried local bug fixes that a submodule
cannot record — the parent repository stores only a commit pointer, so a working-tree change was
invisible to a clone and destroyed by `git submodule update` — and **skipping a patch did not
break the build, it produced a firmware that was silently wrong.** The flexPTP heartbeat patch of
that time was the sharp case: without it the firmware built, linked, got an address and never
synchronised, because every heartbeat was rejected and the BMCA never left INITIALIZING (F80).

The trees are now git repositories declared in
[`bazel/repositories.bzl`](bazel/repositories.bzl), pinned by commit, and the patches in
[`patches/`](patches/) are attached to the fetch. The patched tree is what lands in Bazel's
repository cache, keyed by content, so there is no longer a working tree that can be pristine,
patched, or neither — which is why the old `--check-patches` gate no longer exists.

## Building

The build is Bazel. Nothing to install but Bazel itself: the compiler is fetched and pinned by
`MODULE.bazel`, and the vendored trees come from git rather than submodules, so there is no
`git submodule update` step and no patch-application step.

```bash
bazel build //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo    # ThreadX + NetX Duo
bazel build //projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:flexptp-demo-freertos   # FreeRTOS + lwIP
```

There are two firmwares for the same board. `bazel build //...` builds both. They share most of
their code — the MAC driver, the flexPTP hardware port, the CLI, the console — and differ in the
RTOS, the network stack and the glue between them.

That produces the `.elf`, `.elf.map`, `.hex` and `.bin` in `bazel-bin/projects/<project>/`. No
`--platforms` flag is needed — the target carries a transition onto the board's platform, so it
cannot silently produce a host build.

Three compilation modes, and only two of them are what ships:

| | Command | Flags |
|---|---|---|
| development (fastbuild) | `bazel build …` | `-O0 -g` |
| **release** | `bazel build --config=opt …` | `-O2 -g0` (the CMake build this replaced shipped `-Og`; see D8 in the [decisions](docs/bazel_decisions.md)) |
| **debug** | `bazel build --config=dbg …` | `-O0 -g3`, and `DEBUG`, which turns on ThreadX's stack check and NetX Duo's API error checking |

Release and debug are both verified on the board; the plain `bazel build` image is neither. See
[Testing](#testing).

To flash. ST's OpenOCD fork is built from source the first time (a few minutes); upstream
OpenOCD cannot program an STM32H5 — see [Flashing](#flashing):

```bash
bazel run //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flash
```

If you already have ST's fork on the machine — in `toolchains/openocd`, on `PATH`, or named by
`$OPENOCD` — use it instead of building one:

```bash
bazel run //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flash --//tools/flash:openocd=local
```

## Testing

Two halves. The host half needs nothing but Bazel; the hardware half needs the board. What each
one can and — just as importantly — **cannot** prove, and the procedures that repeat a full
verification, are in
[`testing_architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md).

### Host tests — no board required

```bash
bazel test //...
```

26 targets in one invocation: the Ethernet descriptor rings and the flexPTP network-stack
driver's logic units; the console FIFO, the CLI and the RTOS context rules against real ThreadX
and FreeRTOS kernels; flexPTP's own engine, patched as the firmware fetches it, against the
defects the patches fix; the C++ rules asserted against the linked images; and the HIL tools'
own parsers and the compile-commands generator's path rewrite. It runs in seconds and gates every push in CI. Host and target are two platforms in
one graph, so there is no separate test root to configure.

To run one binary, or the target-image checks alone:

```bash
bazel test //projects/flexptp_demo_stm32h563/lib:mac_drv_tx_ring_test
bazel test //tools/ci:cxx_rules_test
```

> [!IMPORTANT]
> A green host suite says **nothing** about codegen defects, memory ordering, or anything
> downstream of the timestamp capture. Two of this port's findings were an optimisation-level
> HardFault and a bounded wait the compiler deleted above `-Og`; the host tests compile
> different code, with a different compiler, for a different instruction set. Roughly a
> quarter of the findings in [`docs/port_review.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md) are reachable only
> on hardware.

### Hardware in the loop — board required

Drives the board's serial CLI from the host, with OpenOCD for flash and reset. Fourteen checks,
from the build ID through PHY identification, DHCP and ping to a settled PTP offset in `SLAVE`
and a 120-second saturation flood:

```bash
tools/hil/run.py --flash bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf --stage S8
```

For the other firmware, pass `--firmware freertos`. It selects the per-firmware expectations —
the CLI command count and the interrupt priority table — which genuinely differ, because the two
kernels require different things of SysTick and SVCall. It defaults to `threadx`.

```bash
tools/hil/run.py --firmware freertos --flash bazel-bin/projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo/flexptp-demo-freertos.elf --stage S0
```

> [!NOTE]
> The runner checks it is testing the image you gave it. The linker stamps a SHA-1 build ID
> into every image, `osinfo` prints it, and the `build_id` check compares the two — so running
> without `--flash` against a board holding different firmware is a **failure**, not a
> confident pass. Use `--elf` to check a board you did not just flash.

`tools/hil/run.py --list` shows the checks and the per-stage sets; `--compare
tools/hil/baseline.json` diffs against golden values captured from the pre-port firmware.
Exit status is `0` pass · `1` fail · `2` board unreachable · `3` passed but incomplete —
**a skip is not a pass**, and conflating the two is how one finding survived for days.

Then the long tests — twenty scenarios covering each upstream fix, floods as slave and as
master, malformed PTP traffic, stack headroom and the console, about an hour per firmware:

```bash
tools/hil/scenario.py --firmware threadx --elf bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf all
```

And the whole verification — both firmwares, release and debug, host suites, S8 and every
scenario — in one command, about four and a half hours:

```bash
tools/hil/matrix.sh
```

Details, including the one-time host setup for a direct cable, are in
[`tools/hil/README.md`](tools/hil/README.md).

## Deploying

### Flashing

**With OpenOCD** — ST's fork is required. `target/stm32h5x.cfg` declares a `stm32h5x` flash
bank, which names a *compiled-in* driver; upstream OpenOCD 0.12.0 has no such driver and
rejects the declaration outright, so copying ST's `.cfg` files onto a distro build does not
help. `bazel run //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flash` builds the fork from source and
flashes in one step. The tools that run OpenOCD themselves — the HIL harness and the VS Code
OpenOCD configurations — use that same binary, through `//tools/flash:openocd_exec`: a symlink
to whatever the flash targets run, so it follows the pin and `--//tools/flash:openocd` with
nothing to copy. The harness builds it as it starts, and the debug configurations' pre-launch
task does too. By hand:

```bash
bazel build //tools/flash:openocd_exec    # first: bazel-bin/ follows the LAST build, and the ELF is found through it
bazel build //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo
bazel-out/k8-fastbuild/bin/tools/flash/openocd_exec/openocd -f board/st_nucleo_h5.cfg \
    -c init -c halt -c "program bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf verify reset" -c exit
```

Note the board file is **`st_nucleo_h5.cfg`** — generic across H5 Nucleos. There is no
`st_nucleo_h563zi.cfg`.

> [!NOTE]
> On Linux, non-root access to the ST-Link needs udev rules — once per machine, and by hand,
> because it is root-owned system configuration and no build should be running `sudo`:
> ```bash
> sudo cp "$(bazel info output_base)/external/+openocd+openocd_src/contrib/60-openocd.rules" /etc/udev/rules.d/
> sudo udevadm control --reload-rules && sudo udevadm trigger
> ```

**With the ST-Link mass-storage device** — the simplest route, and no tools at all. The board
enumerates as a USB drive; copy the `.bin` onto it and it reprograms and resets itself:

```bash
cp bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.bin /run/media/$USER/NOD_H563ZI/ && sync
```

**With `st-flash`** from [stlink-tools](https://github.com/stlink-org/stlink):

```bash
st-flash write bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.bin 0x08000000
```

**With [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)**, which recognises the MCU once connected.

### Interacting with the firmware

The firmware prints its messages to and expects user input on the board controller's virtual
serial port using the `115200-8-N-1` configuration. Use [Putty](https://www.putty.org/) or any
equivalent (e.g. GtkTerm); on Linux the device shows up as `/dev/ttyACMx`.

> [!NOTE]
> Read the firmware's bootup hints and messages carefully!

### PPS signal

The 1PPS signal is emitted on **`PG8`** (ETH_PPS_OUT, AF11). `ptp pps <freq>` sets it to the
nearest power of two from 1 Hz to 16384 Hz; `ptp pps 0` stops it.

> [!NOTE]
> "Stops it" needed a fix. The MAC's fixed-frequency PPS mode has no *disabled* encoding —
> `PPSCTRL = 0b0000` is the **1 Hz** code — so `ptp pps 0` used to leave the pin toggling once a
> second while the CLI reported it off. Stopping the output means leaving that mode entirely.
> See NX-09 in [`docs/review_v1_opus_5p0.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/review_v1_opus_5p0.md).

## Test environment

To evaluate synchronization accuracy, connect the devboard to another PTP-capable device —
a dedicated master clock, or a Linux host running [linuxptp](https://github.com/richardcochran/linuxptp).
**[`docs/host_ptp_setup.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/host_ptp_setup.md) is a step-by-step guide** to setting the
host up, including a direct-cable configuration and the failure modes worth knowing about.

Two things that cost days here and are worth reading before you start:

- **The board loads the `default` profile at startup** — plain IEEE 1588 over UDP/IPv4 with
  E2E delay measurement — which a stock `ptp4l` with no config file already matches. The
  other presets (`gPTP`, `defp2p`, `PUP`) need the host matched to them, and `gPTP` additionally
  needs a direct cable or 802.1AS-capable bridges throughout. Profiles are not persisted, so
  any runtime `ptp profile preset …` is lost on reset. See
  **[`docs/ptp_profiles.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/ptp_profiles.md)** for the trade-offs.
- **A switch may silently discard PTP `Delay_Resp`** while forwarding `Sync` and `Announce`
  on the same multicast group. Verified on this setup. If sync never completes, try a direct
  cable before suspecting the firmware.

Hardware timestamping on the host side is available out-of-the-box on several Intel NICs, such
as the I210, I219-V, I226 or 82576. With software timestamping expect microsecond-class results —
enough to prove the link works, not enough to characterise it. The bench here now uses an
**I226** (`igc`), so `ptp4l` runs with `-H`; `tools/hil/setup_host_net.sh --with-ptp4l` picks
hardware or software timestamping for the interface by itself, and `--gptp` starts the master
in the 802.1AS profile instead.

Against such a master, on a direct cable, both firmware images hold **sub-microsecond** offset
in the `default` (layer 4) profile — RMS **187 ns** on FreeRTOS/lwIP and **203 ns** on
ThreadX/NetX Duo over a 15-minute window, p99 around 500 ns. The two are not distinguishable
from one 15-minute run each, and
[`docs/l4_performance.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/l4_performance.md)
says so explicitly alongside the method, the distributions and an unexplained 11-second Sync
outage seen on FreeRTOS.

## Development

A [Visual Studio Code](https://code.visualstudio.com/) configuration ships in `.vscode/`. It
needs [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug)
to debug and [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
for IntelliSense; `.vscode/extensions.json` recommends those and the rest, including the
[Bazel](https://marketplace.visualstudio.com/items?itemName=BazelBuild.vscode-bazel) extension.
The compiler and GDB come from the toolchain Bazel fetches, so nothing else is needed to build
or debug through OpenOCD.

**Tasks** (*Terminal → Run Task…*):

| Task | Runs |
|---|---|
| `Bazel: build ThreadX (dbg)` / `(opt)` | `bazel build --config=dbg` / `--config=opt` `//projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo` |
| `Bazel: build FreeRTOS (dbg)` / `(opt)` | the same for `//projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:flexptp-demo-freertos` |
| `Bazel: build OpenOCD` | `bazel build //tools/flash:openocd_exec` — the OpenOCD the flash targets run, for the debug configurations |
| `Bazel: build ThreadX (dbg) + OpenOCD` / `FreeRTOS (dbg) + OpenOCD` | the two above, OpenOCD first — the OpenOCD debug configurations' pre-launch task |
| `Bazel: flash ThreadX (dbg)` / `(opt)`, `Bazel: flash FreeRTOS (dbg)` / `(opt)` | `bazel run --config=… //…:flash` — build and program in one step (see [Flashing](#flashing)) |
| `Bazel: test //...` | the whole test suite |
| `Bazel: refresh compile_commands.json` | `tools/ide/refresh_compile_commands.py` — see *IntelliSense* below |

`dbg` and `opt` are the debug and release modes in [Building](#building) — so a `dbg` image
has `DEBUG` too: ThreadX's stack check and NetX Duo's API error checking.

**Debugging.** Four configurations per firmware: *Launch* (flash, reset, run to `main`) and
*Attach* (to whatever is running), each through OpenOCD or through ST's GDB server. They load
the ELF from `bazel-bin/projects/flexptp_demo_stm32h563/<project>/`, and every one runs that
firmware's `build … (dbg)` task first. That matters more than it looks: `bazel-bin` points at
the **last** build's configuration, so after an `opt` build or a `bazel test //...` the ELF
there is a different image, or missing. An *Attach* therefore assumes the board is running the
`dbg` build of the tree you have checked out.

The OpenOCD configurations run the same OpenOCD as the flash tasks,
`//tools/flash:openocd_exec` ([Flashing](#flashing)), which their `build … (dbg) + OpenOCD`
task builds before the firmware — minutes the first time, since it is ST's fork from source,
and a no-op after that. `--//tools/flash:openocd=local` therefore moves both together. The ST-Link GDB-server
configurations and the register (`.SVD`) view resolve through
[STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html), located by the
[STM32Cube for Visual Studio Code](https://marketplace.visualstudio.com/items?itemName=stmicroelectronics.stm32-vscode-extension)
extension; both are optional, and without them only those two are lost.

> [!NOTE]
> OpenOCD's RTOS awareness lists every thread with its name and state, and the *current*
> thread is exact. The others' backtraces are not: OpenOCD unstacks them with a plain
> Cortex-M frame, and both kernels' Cortex-M33 ports save more than that. Read a blocked
> thread's state from its control block, not from its backtrace. The details are in
> `.vscode/launch.json`.

**IntelliSense.** clangd reads `compile_commands.json` at the workspace root, which Bazel does
not write by itself. Generate it — and again after adding a source file or changing a
`BUILD.bazel`:

```bash
tools/ide/refresh_compile_commands.py
```

It builds both firmwares and every host test with `--config=dbg`, takes their compile commands
from `bazel aquery`, and rewrites Bazel's execroot-relative paths so they resolve from the
workspace — the vendored trees included, so go-to-definition reaches into ThreadX, NetX Duo and
the HAL. A file compiled both for the board and for a host test gets the board's command. Other
flags or targets go on the command line:
`tools/ide/refresh_compile_commands.py --config=opt -- //projects/flexptp_demo_stm32h563/lib:mac_drv_tx_ring_test`.
The usual tool for this, hedronvision/bazel-compile-commands-extractor, is not in BCR and, as
published, uses native rules Bazel 9 removed; the script's docstring has the details.

### Software structure

This is a monorepo with **two firmwares** over one set of shared code. They are not build
variants of each other: each is its own Bazel target with its own RTOS and its own network
stack, and both are built and flashed the same way.

| | `projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo` | `projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo` |
|---|---|---|
| RTOS | [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) | [FreeRTOS](https://github.com/FreeRTOS/FreeRTOS-Kernel), `main` head (`83a45bf`) |
| Network stack | [Eclipse NetX Duo](https://github.com/eclipse-threadx/netxduo), whose link layer carries PTP hardware timestamps natively | [lwIP](https://github.com/lwip-tcpip/lwip) |
| CMSIS-RTOS2 wrapper | [ST's, over ThreadX](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx) | ST's, over FreeRTOS |
| Bazel target | `//projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo` | `//projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:flexptp-demo-freertos` |

Shared by both, in `//projects/flexptp_demo_stm32h563/lib`:

- **The PTP and Ethernet logic units** — timestamp codec, endpoint addressing, the TX registry,
  NSD lifecycle, frame framing, link state. These depend on nothing but `stdint`, which is why
  they were the first thing extracted, and the host tests beside them pin their behaviour.
- **The hardware layer** — `mac_drv.c` and the PHY drivers, and flexPTP's STM32H563 port.
  `mac_drv.h` was written stack-agnostic so the driver could outlive a change of network stack;
  that is exactly what happened.
- **The CMSIS-RTOS2 layer** — the CLI, `serial_io.c`, the console and the blocking FIFO.
  `cmsis_os2.h` is ARM's and names no RTOS, so **neither of these libraries names an RTOS
  either**; `osThreadNew()` resolves at link against whichever wrapper the firmware pulls in.
  That is the property that makes two RTOSes over one application layer possible at all.
- **The runtime and startup** — syscalls, the heap, C++ support, `SystemInit()`, the reset
  vector, the linker script, and the HAL configuration.

Common to both, from outside the tree: ST's
[STM32H5xx HAL](https://github.com/STMicroelectronics/stm32h5xx-hal-driver), and
[embfmt](https://gitea.epagris.com/epagris/embfmt), a `printf()`-like formatted printer for
embedded systems.

What is deliberately *not* shared is `main.c`, the configuration headers, the stack glue
(`ethernet_*.c`, `eth_drv_*.c`, `nsd_*.c`) and the RTOS/stack `BUILD` wiring. Those are the
difference between the two firmwares. See
[`docs/bazel_migration.md` §13.8](docs/bazel_migration.md#138-unification-afterwards) for how the
split was arrived at, and the rule that governs it: a file is unified only once a test pins its
behaviour.

> [!NOTE]
> Two stacks that used to be selectable here are gone. **EtherLib** was removed — it was only
> ever compiled, never run on this port. **lwIP** was removed from the ThreadX firmware in S3 of
> that port and is now the network stack of the FreeRTOS firmware instead, which is a much better
> home for it. There is no `ETH_STACK` switch any more; choosing a stack means choosing a project.

The repository is organized the following way:

```
bazel/              how each vendored tree is fetched and built: repositories.bzl pins them by
                    commit and attaches the patches; one .BUILD file per tree
docs/               repository-level documents: the Bazel migration, its decisions, the flexPTP review
patches/
  flexptp/          ten fixes, filed upstream or ready to be, applied by the fetch, deleted as each merges;
    test/           and the host tests that run flexPTP's real engine against them
  embfmt/           one patch, four formatting defects
projects/flexptp_demo_stm32h563/
  lib/              shared by both firmwares:
    eth/              the MAC driver (mac_drv.c), PHY driver, framer, link state, and their host tests
    ptp/              flexPTP's STM32H563 port and the NSD logic units, with their host tests
    cli/ io/ blocking_io/   the command line, MSG() and the console, the blocking FIFO
    runtime/ startup/ the syscalls, heap, C++ support, SystemInit(), reset vector, linker script, HAL config
    host/             the shims that let the driver compile for the host
  flexptp_threadx_netx_demo/    the ThreadX + NetX Duo firmware
    main.c cmds.c tx_user.h nx_user.h flexptp_options.h
    eth/              the NetX Duo link driver and stack bring-up
    ptp/              the NetX Duo network-stack driver for flexPTP, and the TX-timestamp deferral
    startup/          ThreadX's low-level init
    test/             FIFO, CLI and context-rule tests against real ThreadX
    docs/             the firmware documentation
  flexptp_freertos_lwip_demo/   the FreeRTOS + lwIP firmware
    main.c cmds.c FreeRTOSConfig.h lwipopts.h flexptp_options.h
    eth/              the lwIP link driver and stack bring-up
    lwip_port/        lwIP's sys_arch over FreeRTOS
    test/             context-rule tests against real FreeRTOS
third_party/CMSIS/  ARM's CMSIS Core and RTOS2 headers, ST's device headers (in-tree, not fetched)
toolchain/          the pinned arm-none-eabi toolchain and its flags per compilation mode
tools/
  hil/              hardware-in-the-loop: the checks, the scenarios, the SWD tools, the matrix
  ci/               link checker, C++ rules and seeding checks
  flash/            the flash rule and the OpenOCD it uses
  upstream/         filing the flexPTP fixes
  linuxptp/         the host grandmaster's gPTP configuration
```

> [!NOTE]
> The flexPTP parameters are defined in each firmware's `flexptp_options.h` — the ThreadX one is
> [here](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp_options.h).

The build carries patches to two of the vendored trees, kept in [`patches/`](patches/) and
**applied as part of fetching the dependency** — see [Get the sources](#get-the-sources) for why
that matters:

- **flexPTP, ten**, each a fix filed upstream, or ready to be, and deleted as its pull request
  merges: U1 (#20), U6 (#26), U8–U10 (#34–#36), U11–U14 (#37–#40), and U15, not yet filed. What each fixes and how it was
  verified is in [`patches/flexptp/README.md`](patches/flexptp/README.md), and the review that
  found them in [`docs/flexptp_upstream_review.md`](docs/flexptp_upstream_review.md). The first
  three this project carried — the servo NaN latch (F59), the IGMP extension point and the
  non-blocking heartbeat enqueue (F80) — were taken upstream in `RC1.0-17`.
- **embfmt, one** — carrying **four** defects across **two** findings: `%ll` conversions
  dropped and NaN rendered as `0.0000` ([F60](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md)), plus small `%e` values
  swallowed by a pre-normalisation rounding constant and a `%f`/`%e` rounding carry that
  subtracted 10 instead of 10^precision ([F66](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md)).
  See [`patches/embfmt/README.md`](patches/embfmt/README.md).

### Printing and logging

In this project the memory-heavy `printf()` is replaced by the more embedded-optimized `MSG()` function backed by the `embfmt` library. Parameters and format specifiers are fully `printf()` compatible.

### CLI commands

The software offers you with the following multitude, most flexPTP-related of commands:

```
?                                                        Print this help (26/64)
hist                                                     Print command history
osinfo                                                   Print OS-related information
flexptp [stop]                                           Start flexPTP, or stop it with `stop`
ethinfo                                                  Print Ethernet driver state and counters
ethstat                                                  Print the same counters as key=value, for tooling
ptpcfg {store|show|load}                                 Exercise flexPTP's retained-config round trip
ptp reset                                                Reset PTP subsystem
ptp servo offset [offset_ns]                             Set or query clock offset
ptp log {def|corr|ts|info|locked|bmca|logid} {on|off}    Turn on or off logging
time [ns]                                                Print time
ptp master [[un]prefer] [clockid]                        Master clock settings
ptp info                                                 Print PTP info
ptp domain [domain]                                      Print or set PTP domain
ptp addend [addend]                                      Print or set addend
ptp transport [{ipv4|802.3}]                             Set or get PTP transport layer
ptp delmech [{e2e|p2p}]                                  Set or get PTP delay mechanism
ptp transpec [{def|gPTP}]                                Set or get PTP transportSpecific field (majorSdoId)
ptp profile [preset [<name>]]                            Print or set PTP profile, or list available presets
ptp tlv [preset [name]|unload]                           Print or set TLV-chain, or list available TLV presets
ptp pflags [<flags>]                                     Print or set profile flags
ptp period <delreq|sync|ann> [<lp>|matched]              Print or set log. periods
ptp coarse [threshold]                                   Print or set coarse correction threshold
ptp priority [<p1> <p2>]                                 Print or set clock priority fields
ptp pps {freq}                                           Set or query PPS signal frequency [Hz]
ptp servo st [var|default]                               Set or get sigma_theta^2 (s^2)
ptp servo sg [var|default]                               Set or get sigma_gamma^2 (s^2)
ptp servo stm [var|default]                              Set or get sigma_theta_m^2 (s^2)
```

Listed in the order the firmware emits them, captured from the running ThreadX board on
2026-11-27. The `(26/64)` on the first line is the CLI's own registered/available slot count.
`flexptp stop` stops PTP again (it exercises `unreg_task_ptp()`, which U9 and U11 fixed), and
`ptpcfg` exercises flexPTP's retained-configuration round trip (U2).

`ethinfo` is this project's own, added by the NetX Duo port. It prints the driver's state and
its receive/transmit counters, and it is how four of the five defects in that port were
actually diagnosed — counters, not code-reading.

Both firmwares register the same 26 commands — the FreeRTOS one's `ethstat` also takes `clear` —
but `ethinfo` and `osinfo` report **less** on the FreeRTOS + lwIP one, and the output says where the gaps are rather than leaving them to be
inferred. `ethinfo` there has no driver drop counters — the NetX Duo driver grew those while the
ThreadX port was chasing where frames were going, and adding them to the lwIP driver means
touching a working receive path — and lwIP is built with `LWIP_STATS 0`, so the stack keeps no
counters of its own to print. What both firmwares report identically is what comes from `//projects/flexptp_demo_stm32h563/lib`:
the MAC's receive-timestamp context counters, the newlib heap high-water mark, and flexPTP's own
core state.

`ethstat` prints the same figures as `key=value` lines between `BEGIN ethstat`/`END ethstat`.
It exists for `tools/hil/`, which used to regex-parse the prose form — and had already silently
miscounted once doing so. The prose is for humans and its layout is free to change; the key
names are the contract.

> [!TIP]
> The above hint can be listed by typing '?'.

This project uses the **Kalman** servo, hence `ptp servo st|sg|stm`. Ports configured for the
PID servo expose `ptp servo params` instead.

## Notes

> [!WARNING]
> By default the MCU is clocked from the on-board ST-Link's 8 MHz MCO. That signal is noisy enough to limit how precisely synchronization settles, so a crystal is the better reference — and on this board (MB1404) one is **already fitted**: X3, 25 MHz, 20 ppm. Nothing needs soldering onto empty pads.
>
> Selecting it takes both hardware and firmware changes: SB3 and SB4 ON, SB49 OFF, C70/C71 populated with 5.6 pF; then `HSE_VALUE` = 25000000, `HSEState` = `RCC_HSE_ON`, and a re-derived PLL — `PLLM` 5, `PLLN` 100, `PLLRGE` = `RCC_PLL1_VCIRANGE_2`. Changing `HSE_VALUE` alone is **not** enough and fails silently, because `init_pll()` computes `PLLM` as `HSE_VALUE/2000000` and 25/2 is not an integer. This path is untested on this port.

## Documentation

**The firmware** — in [`projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/architecture.md); written for the ThreadX firmware, and where
marked, covering both:

| Document | Contents |
|---|---|
| [`architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/architecture.md) | Module inventory and versions, layering, boot sequence, threads and priorities, interrupts, Debug vs Release, clocking, the PTP timestamp path, memory layout |
| [`testing_architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md) | **Both firmwares.** The test layers and what each reaches, debug vs release, the bench, the procedures that repeat a full verification, mutation testing, the gaps stated plainly, the latest results |
| [`host_ptp_setup.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/host_ptp_setup.md) | **Both firmwares.** Running a PTP master on a Linux host, and what to check when nothing happens |
| [`ptp_profiles.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/ptp_profiles.md) | **Both firmwares.** The four profile presets, the transport and delay-mechanism trade-offs, and what a switched network costs |
| [`l4_performance.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/l4_performance.md) | **Both firmwares.** Measured layer 4 accuracy against a hardware-timestamping master, and what a single run cannot establish |
| [`ptp_clock.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/ptp_clock.md) | Full derivation of the PTP timestamp clock, and why a 4 ns increment is possible but not adopted |
| [`design_doc.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/design_doc.md) | Porting decisions and their rationale, clock tree, pin map, the TrustZone guide, the custom MAC driver, and the first port's stage log |
| [`port_review.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md) | Every finding of the port reviews, F1–F80 and the NX/TS/AR/CX/GI series, with evidence — including the ones that cost days |

**The repository** — in [`docs/`](docs/), and the READMEs beside what they describe:

| Document | Contents |
|---|---|
| [`docs/flexptp_upstream_review.md`](docs/flexptp_upstream_review.md) | The flexPTP review, U1–U15: each finding, its fix and pull request, and every hardware campaign since, dated |
| [`docs/bazel_decisions.md`](docs/bazel_decisions.md) | Why the build is what it is: D1–D18, with the experiments behind them |
| [`docs/bazel_migration.md`](docs/bazel_migration.md) | The completed CMake → Bazel migration plan, phase by phase, and the two-firmware layout it arrived at |
| [`tools/hil/README.md`](tools/hil/README.md) | The hardware harness: host setup, the checks, the scenarios, the fuzzer, the SWD tools |
| [`projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/test/README.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/test/README.md) | How the driver and a real RTOS run on the host |
| [`patches/flexptp/README.md`](patches/flexptp/README.md), [`patches/embfmt/README.md`](patches/embfmt/README.md) | What each carried patch fixes, how it was verified, how to regenerate and retire them |
| [`tools/upstream/README.md`](tools/upstream/README.md) | The flexPTP fixes' branches, pull requests and status, and how they are filed |

**History** — kept as the record of how the firmware got here, not as a description of it:

| Document | Contents |
|---|---|
| [`implementation_plan.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/implementation_plan.md) | The staged plan for the **first** port (H743 → H563). Closed; ends with a table of what changed afterwards |
| [`threadx_netxduo_feasibility.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_netxduo_feasibility.md) | What porting to Eclipse ThreadX and NetX Duo would cost, written before it was done |
| [`threadx_porting_plan.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_porting_plan.md) | The staged execution plan for that port — hardware checkpoints, the host test tree, and the measured reach of mocks |
| [`threadx_s5e_plan.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_s5e_plan.md) | The one stage left undone: running the real stack on the host, and why it was not |
| [`review_v1_opus_5p0.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/review_v1_opus_5p0.md) | The first comprehensive review of the ThreadX port (August 2026); its findings are tracked in `port_review.md` |

## Related papers and references

[Time Synchronization Extension for the IO-Link Industrial Communication Protocol](https://ieeexplore.ieee.org/document/10747727)

[Distributed Measurement System for Performance Evaluation of Embedded Clock Synchronization Solutions](https://ieeexplore.ieee.org/document/9805958/)

[Portable, PTP-based Clock Synchronization Implementation for Microcontroller-based Systems and its Performance Evaluation](https://ieeexplore.ieee.org/document/9615250)

[Synchronization of Sampling in a Distributed Audio Frequency Range Data Acquisition System Utilizing Microcontrollers](https://ieeexplore.ieee.org/document/9918455/)

[Methods of Peripheral Synchronization in Real-Time Cyber-Physical Systems](https://ieeexplore.ieee.org/document/10178979/)

## License

The project was created by András Wiesner (Epagris) in 2025 and published under the MIT license. This specific port was created by Damien Dusha (also under the MIT license) with lots of help from Claude Opus 5.0 and Gemini Flash 3.6.

Contributions are welcome! :)
