# STM32 NUCLEO-H563ZI flexPTP demo

## What's this?

> **This is a [flexPTP](https://github.com/epagris/flexPTP) demo project showcasing the capabilities of the flexPTP [IEEE 1588 Precision Time Protocol](https://ieeexplore.ieee.org/document/9120376) implementation for the [STMicroelectronics NUCLEO-H563ZI](https://www.st.com/en/evaluation-tools/nucleo-h563zi.html) devboard.**

Still not clear what is it useful for? No worries, it's a behind-the-scenes support technology that you use unaware every day if you have a smartphone or when you are connected to the internet. Modern telecommunication and measurement systems often rely on precise time synchronization down to the nanoseconds' scale. Methods got standardized by the IEEE and now it's known by the name of the Precision Time Protocol. This software project is an evaluation environment to showcase the capabilities of our IEEE 1588 PTP implementation named `flexPTP` on the STMicroelectronics NUCLEO-H563ZI board.

This project is a port of [flexPTP-demo-NUCLEO-H743ZI](https://github.com/epagris/flexPTP-demo-NUCLEO-H743ZI)
from the STM32H743 (Cortex-M7) to the STM32H563 (Cortex-M33). It has since been ported a
second time, from FreeRTOS and lwIP to **Eclipse ThreadX and Eclipse NetX Duo**, which is what
it runs today.

Documentation is **per project**: each firmware's design, defect record and testing notes live
beside its sources, in `projects/<name>/docs/`. Only repository-level documents — the Bazel
migration [plan](docs/bazel_migration.md) and its
[rationale](docs/bazel_decisions.md) — are at the top.

For the ThreadX + NetX Duo firmware, start with
[`architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/architecture.md) for how it fits
together, or
[`testing_architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md)
for how it is verified — including what each tier of testing *cannot* tell you.

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
break the build, it produced a firmware that was silently wrong.** Patch 0004 is the sharp case:
without it the firmware builds, links, gets an address and never synchronises, because every
heartbeat is rejected and the BMCA never leaves INITIALIZING.

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

That produces the `.elf`, `.map`, `.hex` and `.bin`. No `--platforms` flag is needed — the
target carries a transition onto the board's platform, so it cannot silently produce a host
build. `--config=opt` gives `-O2` and `--config=dbg` gives `-O0 -g3` (the CMake build this
replaced shipped `-Og`; see D8 in the decisions document for why that changed).

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

Tests — the host suite, the C++ rules asserted against the linked image, and the OpenOCD
capability check, all in one invocation:

```bash
bazel test //...
```

## Testing

Two suites. The host suite needs nothing but a compiler; the hardware suite needs the board.
What each one can and — just as importantly — **cannot** prove is set out in
[`docs/testing_architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md).

### Host unit tests — no board required

73 `ctest` entries (77 GoogleTest cases) covering the Ethernet descriptor rings, the flexPTP
network-stack driver's logic units, and the ThreadX context rules against a real kernel. The
whole suite runs in about a quarter of a second and gates every push in CI.

```bash
bazel test //...
```

That covers the host suite, the C++ rules asserted against the linked image, and the OpenOCD
capability check. Host and target are two platforms in one graph, so there is no separate test
root to configure and no need to choose between them.

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

Drives the board's serial CLI from the host, with OpenOCD for flash and reset. Thirteen checks,
from PHY identification through DHCP and ping to a bounded PTP offset in `SLAVE`.

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

Details, including the network setup for a direct cable, are in
[`tools/hil/README.md`](tools/hil/README.md).

## Deploying

### Flashing

**With OpenOCD** — ST's fork is required. `target/stm32h5x.cfg` declares a `stm32h5x` flash
bank, which names a *compiled-in* driver; upstream OpenOCD 0.12.0 has no such driver and
rejects the declaration outright, so copying ST's `.cfg` files onto a distro build does not
help. `bazel run //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flash` builds the fork from source and
flashes in one step; the raw invocation, against a fork you already have, is:

```bash
toolchains/openocd/bin/openocd -f board/st_nucleo_h5.cfg \
    -c init -c halt -c "program build/flexptp-demo.elf verify reset" -c exit
```

Note the board file is **`st_nucleo_h5.cfg`** — generic across H5 Nucleos. There is no
`st_nucleo_h563zi.cfg`.

> [!NOTE]
> On Linux, non-root access to the ST-Link needs udev rules:
> ```bash
> sudo cp toolchains/openocd-src/contrib/60-openocd.rules /etc/udev/rules.d/
> sudo udevadm control --reload-rules && sudo udevadm trigger
> ```

**With the ST-Link mass-storage device** — the simplest route, and no tools at all. The board
enumerates as a USB drive; copy the `.bin` onto it and it reprograms and resets itself:

```bash
cp build/flexptp-demo.bin /run/media/$USER/NOD_H563ZI/ && sync
```

**With `st-flash`** from [stlink-tools](https://github.com/stlink-org/stlink):

```bash
st-flash write build/flexptp-demo.bin 0x08000000
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
  other presets (`gPTP`, `defp2p`) need the host matched to them, and `gPTP` additionally
  needs a direct cable or 802.1AS-capable bridges throughout. Profiles are not persisted, so
  any runtime `ptp profile preset …` is lost on reset. See
  **[`docs/ptp_profiles.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/ptp_profiles.md)** for the trade-offs.
- **A switch may silently discard PTP `Delay_Resp`** while forwarding `Sync` and `Announce`
  on the same multicast group. Verified on this setup. If sync never completes, try a direct
  cable before suspecting the firmware.

Hardware timestamping on the host side is available out-of-the-box on several Intel NICs, such
as the I210, I219-V or 82576. With software timestamping expect microsecond-class results —
enough to prove the link works, not enough to characterise it.

## Development

An all-around [Visual Studio Code](https://code.visualstudio.com/) project is packaged along
the project. Install the [STM32Cube for Visual Studio Code](https://marketplace.visualstudio.com/items?itemName=stmicroelectronics.stm32-vscode-extension),
[CMakeTools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools),
[Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) and
[Embedded Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.vscode-embedded-tools)
extensions. [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd)
and clang-format are highly recommended.

The project has predefined *Launch* and *Attach* tasks. The OpenOCD ones point at
`toolchains/openocd/bin/openocd`; `--//tools/flash:openocd=local` makes the Bazel flash target
use the same binary, so the two agree. The ST-Link GDB-server configurations and the register (`.SVD`) views still resolve
through [STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html); it is
optional, and without it only the register view is lost.

### Software structure

This is a monorepo with **two firmwares** over one set of shared code. They are not build
variants of each other: each is its own Bazel target with its own RTOS and its own network
stack, and both are built and flashed the same way.

| | `projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo` | `projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo` |
|---|---|---|
| RTOS | [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) | [FreeRTOS](https://github.com/FreeRTOS/FreeRTOS-Kernel) V11.3.0 |
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

The project is organized the following way:

```
ROOT
  Drivers
    CMSIS: ARM CMSIS-related files (CMSIS RTOS V2 and device headers)
    EthDrv: a custom Ethernet driver (with PTP timestamp support), a PHY-interface and network stack interfaces
    stm32h5xx-hal-driver: ST's STM32H5xx HAL driver files (submodule)
  Inc: headers for compile-time library configuration (tx_user.h, nx_user.h, flexptp_options.h)
  Middlewares: CMake glue for ThreadX, the CMSIS-RTOS2 wrapper and NetX Duo
  Modules
    flexptp: our PTP implementation (submodule)
    threadx: the Eclipse ThreadX kernel (submodule)
    netxduo: the Eclipse NetX Duo Ethernet stack (submodule)
    cmsis_rtos_threadx: ST's CMSIS-RTOS2 wrapper for ThreadX (submodule)
    lwip: the LwIP Ethernet stack -- no longer built, pinned so pre-port commits still build (submodule)
    etherlib: our in-house developed Ethernet stack (submodule)
    embfmt: a printf()-like formatted printer implementation for embedded systems (submodule)
    blocking_io: a simple blocking FIFO implementation
  Src
    cliutils: CLI-interface implementation
    ethernet: Ethernet stack initialization
    flexptp_port: the STM32H5 PTP hardware port, supplied by this project
    flexptp_nsd: the NetX Duo network-stack driver for flexPTP, also supplied by this project
    standard_output: MSG() backend and the USART3 serial driver

    cmds.c: custom CLI commands
  Startup: vector table, reset handler and ThreadX low-level init
  test: host unit tests -- no board required
  docs: porting documentation, design decisions and the review log
  patches: changes to submodules, which git cannot record for us
  tools
    hil: hardware-in-the-loop test harness
    ci: documentation link checker
```

> [!NOTE]
> The flexPTP parameters are defined in the [flexptp_options.h](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp_options.h) header.

Two submodules carry local changes, kept in [`patches/`](patches/) because the parent
repository records only submodule commit pointers — **four patch files in total**:

- **flexPTP, three.** A servo NaN-latch fix ([F59](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md)); a generic
  network-stack extension point for the two IGMP group addresses; and a non-blocking heartbeat
  enqueue, without which the BMCA never leaves `INITIALIZING` ([F80](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md)).
- **embfmt, one** — carrying **four** defects across **two** findings: `%ll` conversions
  dropped and NaN rendered as `0.0000` ([F60](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md)), plus small `%e` values
  swallowed by a pre-normalisation rounding constant and a `%f`/`%e` rounding carry that
  subtracted 10 instead of 10^precision ([F66](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md)).

All are described in [`docs/port_review.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md) and all are worth reporting
upstream. **The build applies them as part of fetching the dependency** — see
[Get the sources](#get-the-sources) for why that matters.

### Printing and logging

In this project the memory-heavy `printf()` is replaced by the more embedded-optimized `MSG()` function backed by the `embfmt` library. Parameters and format specifiers are fully `printf()` compatible.

### CLI commands

The software offers you with the following multitude, most flexPTP-related of commands:

```
?                                                  Print this help (25/64)
hist                                               Print command history
osinfo                                             Print OS-related information
flexptp                                            Start flexPTP daemon
ethinfo                                            Print Ethernet driver state and counters
ethstat                                            Print the same counters as key=value, for tooling
ptp reset                                          Reset PTP subsystem
ptp servo offset [offset_ns]                       Set or query clock offset
ptp log {def|corr|ts|info|locked|bmca} {on|off}    Turn on or off logging
time [ns]                                          Print time
ptp master [[un]prefer] [clockid]                  Master clock settings
ptp info                                           Print PTP info
ptp domain [domain]                                Print or set PTP domain
ptp addend [addend]                                Print or set addend
ptp transport [{ipv4|802.3}]                       Set or get PTP transport layer
ptp delmech [{e2e|p2p}]                            Set or get PTP delay mechanism
ptp transpec [{def|gPTP}]                          Set or get PTP transportSpecific field (majorSdoId)
ptp profile [preset [<name>]]                      Print or set PTP profile, or list available presets
ptp tlv [preset [name]|unload]                     Print or set TLV-chain, or list available TLV presets
ptp pflags [<flags>]                               Print or set profile flags
ptp period <delreq|sync|ann> [<lp>|matched]        Print or set log. periods
ptp coarse [threshold]                             Print or set coarse correction threshold
ptp priority [<p1> <p2>]                           Print or set clock priority fields
ptp pps {freq}                                     Set or query PPS signal frequency [Hz]
ptp servo st [var|default]                         Set or get sigma_theta^2 (s^2)
ptp servo sg [var|default]                         Set or get sigma_gamma^2 (s^2)
ptp servo stm [var|default]                        Set or get sigma_theta_m^2 (s^2)
```

Listed in the order the firmware emits them, captured from the running board. The `(25/64)`
on the first line is the CLI's own registered/available slot count.

`ethinfo` is this project's own, added by the NetX Duo port. It prints the driver's state and
its receive/transmit counters, and it is how four of the five defects in that port were
actually diagnosed — counters, not code-reading.

Both firmwares register the same 25 commands, but `ethinfo` and `osinfo` report **less** on the
FreeRTOS + lwIP one, and the output says where the gaps are rather than leaving them to be
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

| Document | Contents |
|---|---|
| [`docs/architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/architecture.md) | Module inventory and versions, layering, boot sequence, threads and priorities, interrupts, clocking, the PTP timestamp path, memory layout |
| [`docs/testing_architecture.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md) | The three test tiers, what each one reaches, the mutation-testing discipline, and the gaps stated plainly |
| [`docs/design_doc.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/design_doc.md) | Porting decisions and their rationale, submodule pinning, clock tree, pin map, the EtherLib and TrustZone guides, stage-by-stage log |
| [`docs/ptp_clock.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/ptp_clock.md) | Full derivation of the PTP timestamp clock, and why a 4 ns increment is possible but not adopted |
| [`docs/ptp_profiles.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/ptp_profiles.md) | The three profile presets, the transport and delay-mechanism trade-offs, and what a switched network costs |
| [`docs/host_ptp_setup.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/host_ptp_setup.md) | Running a PTP master on a Linux host, and what to check when nothing happens |
| [`docs/port_review.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/port_review.md) | Every finding of the port review, with evidence — including the ones that cost days |
| [`docs/implementation_plan.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/implementation_plan.md) | The staged plan for the **first** port (H743 → H563). Closed and historical; ends with a table of what changed afterwards |
| [`docs/threadx_netxduo_feasibility.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_netxduo_feasibility.md) | What porting to Eclipse ThreadX and NetX Duo would cost, and the one flexPTP defect that would break it silently |
| [`docs/threadx_porting_plan.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_porting_plan.md) | The staged execution plan for that port — hardware checkpoints, the host test tree, and progress tracking |
| [`docs/threadx_s5e_plan.md`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_s5e_plan.md) | The one stage left undone: running the real stack on the host, what it would and would not have caught, and a cheaper alternative |
| [`docs/reviews/`](projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/reviews/README.md) | Archive of the independent reviews that fed `port_review.md` — historical, superseded by it where they disagree |

## Related papers and references

[Time Synchronization Extension for the IO-Link Industrial Communication Protocol](https://ieeexplore.ieee.org/document/10747727)

[Distributed Measurement System for Performance Evaluation of Embedded Clock Synchronization Solutions](https://ieeexplore.ieee.org/document/9805958/)

[Portable, PTP-based Clock Synchronization Implementation for Microcontroller-based Systems and its Performance Evaluation](https://ieeexplore.ieee.org/document/9615250)

[Synchronization of Sampling in a Distributed Audio Frequency Range Data Acquisition System Utilizing Microcontrollers](https://ieeexplore.ieee.org/document/9918455/)

[Methods of Peripheral Synchronization in Real-Time Cyber-Physical Systems](https://ieeexplore.ieee.org/document/10178979/)

## License

The project was created by András Wiesner (Epagris) in 2025 and published under the MIT license. This specific port was created by Damien Dusha (also under the MIT license) with lots of help from Claude Opus 5.0 and Gemini Flash 3.6.

Contributions are welcome! :)
