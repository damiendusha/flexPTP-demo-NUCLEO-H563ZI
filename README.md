# STM32 NUCLEO-H563ZI flexPTP demo

## What's this?

> **This is a [flexPTP](https://github.com/epagris/flexPTP) demo project showcasing the capabilities of the flexPTP [IEEE 1588 Precision Time Protocol](https://ieeexplore.ieee.org/document/9120376) implementation for the [STMicroelectronics NUCLEO-H563ZI](https://www.st.com/en/evaluation-tools/nucleo-h563zi.html) devboard.**

Still not clear what is it useful for? No worries, it's a behind-the-scenes support technology that you use unaware every day if you have a smartphone or when you are connected to the internet. Modern telecommunication and measurement systems often rely on precise time synchronization down to the nanoseconds' scale. Methods got standardized by the IEEE and now it's known by the name of the Precision Time Protocol. This software project is an evaluation environment to showcase the capabilities of our IEEE 1588 PTP implementation named `flexPTP` on the STMicroelectronics NUCLEO-H563ZI board.

This project is a port of [flexPTP-demo-NUCLEO-H743ZI](https://github.com/epagris/flexPTP-demo-NUCLEO-H743ZI)
from the STM32H743 (Cortex-M7) to the STM32H563 (Cortex-M33). The porting work, the decisions
behind it and every defect found along the way are documented in [`docs/`](docs/) — start with
[`docs/architecture.md`](docs/architecture.md).

### Get the sources

> [!NOTE]
> To acquire the full source tree after cloning the repo, please fetch the linked *submodules* as well:

```bash
git clone <this-repository> flexPTP-demo-NUCLEO-H563ZI
cd flexPTP-demo-NUCLEO-H563ZI
git submodule update --init --recursive
git -C Modules/embfmt  apply "$PWD/patches/embfmt/0001-formatting-fixes.patch"
git -C Modules/flexptp apply "$PWD/patches/flexptp/0002-servo-nan-latch-fix.patch"
git -C Modules/flexptp apply "$PWD/patches/flexptp/0003-generic-netstack-igmp-addresses.patch"
git -C Modules/flexptp apply "$PWD/patches/flexptp/0004-cmsis-os2-event-enqueue-from-timer-thread.patch"
```

`./build.sh` does **both** steps for you, and refuses to build if the patches can be
neither applied nor confirmed already applied. Use it, or run the six commands above.

> [!IMPORTANT]
> The patch commands are not optional. Two submodules carry local bug fixes that git
> cannot record for us — the parent repository stores only a submodule's *commit pointer*,
> so a working-tree change inside `Modules/` is invisible to a clone and is discarded by
> `git submodule update`. They therefore live in [`patches/`](patches/) and must be applied.
>
> Skipping them does not break the build. It produces a firmware that is **silently wrong**:
>
> | Missing patch | Consequence |
> |---|---|
> | `flexptp/0002-servo-nan-latch-fix` | One duplicated `Sync` makes the measured Sync period zero; the Kalman servo computes `0.0/0.0` and NaN latches into its state **permanently**. The clock free-runs at ~18.7 ppm while every visible indicator stays green ([F59](docs/port_review.md)) |
> | `embfmt/0001-formatting-fixes` | `%ll` conversions dropped, `NaN` printed as a plausible `0.0000`, small `%e` values and any rounding carry misreported ([F60](docs/port_review.md), [F66](docs/port_review.md)) — this is what disguised F59 as a working servo |
> | `flexptp/0004-cmsis-os2-event-enqueue-from-timer-thread` | The heartbeat is enqueued with `osWaitForever` from ThreadX's timer thread, which that thread may not block in. Every heartbeat is rejected, the BMCA never leaves `INITIALIZING`, and **no master is ever selected** — the clock simply never syncs ([F80](docs/port_review.md)) |
> | `flexptp/0003-generic-netstack-igmp-addresses` | Not a defect: an extension point so a port supplying its own network stack can provide the two IGMP group addresses. Without it flexPTP `#error`s unless it recognises lwIP, EtherLib or Linux |
>
> All are permanent fixes rather than instrumentation, and all are worth reporting
> upstream. See [`patches/flexptp/README.md`](patches/flexptp/README.md) and
> [`patches/embfmt/README.md`](patches/embfmt/README.md).

## Building

### Prerequisites

| Tool | Needed for | Note |
|---|---|---|
| `arm-none-eabi-gcc` (v12+) | compiling | `build.sh` downloads ARM GNU Toolchain 15.3.rel1 into `toolchains/` if absent |
| `cmake` (v3.22+) | build management | |
| **`ninja`** | the CMake **presets** only | `CMakePresets.json` pins the Ninja generator. `build.sh` bypasses presets and uses CMake's default generator, so the two paths have different requirements — a preset build fails without Ninja while `./build.sh` succeeds |
| `openocd` (ST's fork) | flashing and debugging | Built by `./build.sh --with-openocd`. **Upstream OpenOCD 0.12.0 cannot program an STM32H5** — see [Flashing](#flashing) |

> [!NOTE]
> On Windows, [STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html) bundles `arm-none-eabi-{gcc,gdb}`, `openocd` and ST's STLink tools. [CMake](https://cmake.org/) and Ninja must be installed separately.

### Compiling

The build script downloads the toolchain if needed, initialises submodules and builds:

```bash
./build.sh
```

Add `--with-openocd` the first time if you intend to flash or debug:

```bash
./build.sh --with-openocd
```

Or configure and build manually. Via presets (needs Ninja):

```bash
cmake --preset debug && cmake --build build/debug
```

or without them:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target flexptp-demo
```

Outputs land in the chosen build directory: `flexptp-demo.elf`, `.bin`, `.hex` and `.map`.
Both the `debug` and `release` presets are tested on hardware.

## Deploying

### Flashing

**With OpenOCD** — ST's fork is required. `target/stm32h5x.cfg` declares a `stm32h5x` flash
bank, which names a *compiled-in* driver; upstream OpenOCD 0.12.0 has no such driver and
rejects the declaration outright, so copying ST's `.cfg` files onto a distro build does not
help. `./build.sh --with-openocd` builds the fork into `toolchains/openocd`:

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

The 1PPS signal is emitted on **`PG8`** (ETH_PPS_OUT, AF11).

## Test environment

To evaluate synchronization accuracy, connect the devboard to another PTP-capable device —
a dedicated master clock, or a Linux host running [linuxptp](https://github.com/richardcochran/linuxptp).
**[`docs/host_ptp_setup.md`](docs/host_ptp_setup.md) is a step-by-step guide** to setting the
host up, including a direct-cable configuration and the failure modes worth knowing about.

Two things that cost days here and are worth reading before you start:

- **The board loads the `default` profile at startup** — plain IEEE 1588 over UDP/IPv4 with
  E2E delay measurement — which a stock `ptp4l` with no config file already matches. The
  other presets (`gPTP`, `defp2p`) need the host matched to them, and `gPTP` additionally
  needs a direct cable or 802.1AS-capable bridges throughout. Profiles are not persisted, so
  any runtime `ptp profile preset …` is lost on reset. See
  **[`docs/ptp_profiles.md`](docs/ptp_profiles.md)** for the trade-offs.
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
`toolchains/openocd/bin/openocd` from `./build.sh --with-openocd`, so they need no system
OpenOCD. The ST-Link GDB-server configurations and the register (`.SVD`) views still resolve
through [STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html); it is
optional, and without it only the register view is lost.

### Software structure

The project relies on the following large software building blocks:
- the [Eclipse ThreadX](https://github.com/eclipse-threadx/threadx) embedded operating system,
- ST's [CMSIS-RTOS2 wrapper for ThreadX](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx), which is what lets the application and flexPTP stay RTOS-agnostic,
- [STM32H5xx Hardware Abstraction Layer library](https://github.com/STMicroelectronics/stm32h5xx-hal-driver) (HAL) providing MCU specific functionality,
- the [Eclipse NetX Duo](https://github.com/eclipse-threadx/netxduo) Ethernet stack, whose link layer carries PTP hardware timestamps natively,
- the [EtherLib](https://gitea.epagris.com/epagris/EtherLib), our in-house developed Ethernet stack,
- the [embfmt](https://gitea.epagris.com/epagris/embfmt) a printf()-like formatted printer implementation for embedded systems.

> [!TIP]
> The project can either use NetX Duo or EtherLib Ethernet stacks. This can be easily changed by setting the `ETH_STACK` CMake variable in the top `CMakeLists.txt`:
> ```
>set(ETH_STACK "NETXDUO") # select "NETXDUO" or "ETHERLIB"
>```
> Only the NetX Duo path is exercised on this port; EtherLib is not compiled. See [`docs/design_doc.md`](docs/design_doc.md) for what porting it would involve.
>
> `ETH_STACK="LWIP"` is a hard build error: lwIP was removed in S3 of the ThreadX port. The submodule is still pinned, so a pre-S3 commit builds it.

The project is organized the following way:

```
ROOT
  Drivers
    CMSIS: ARM CMSIS-related files (CMSIS RTOS V2 and device headers)
    EthDrv: a custom Ethernet driver (with PTP timestamp support), a PHY-interface and network stack interfaces
    stm32h5xx-hal-driver: ST's STM32H5xx HAL driver files (submodule)
  Inc: headers for compile-time library configuration
  Middlewares: FreeRTOS CMake configuration
  Modules
    flexptp: our PTP implementation (submodule)
    lwip and lwip_port: the LwIP Ethernet stack and its system dependent module (submodule)
    etherlib: our in-house developed Ethernet stack (submodule)
    embfmt: a printf()-like formatted printer implementation for embedded systems (submodule)
    blocking_io: a simple blocking FIFO implementation
  Src
    cliutils: CLI-interface implementation
    ethernet: Ethernet stack initialization
    flexptp_port: the STM32H5 PTP hardware port, supplied by this project
    standard_output: MSG() backend and the USART3 serial driver

    cmds.c: custom CLI commands
  Startup: vector table and reset handler
  docs: porting documentation, design decisions and the review log
  patches: changes to submodules, which git cannot record for us
  tools: host-side helpers (linuxptp configuration)
```

> [!NOTE]
> The flexPTP parameters are defined in the [flexptp_options.h](Inc/flexptp_options.h) header.

Two submodules carry local changes, kept in [`patches/`](patches/) because the parent
repository records only submodule commit pointers: a servo fix in flexPTP, and **four** defects across **two** findings, in **one** patch file: `%ll` conversions dropped ([F60](docs/port_review.md)), NaN rendered as `0.0000` (F60), small `%e` values swallowed by a pre-normalisation rounding constant ([F66](docs/port_review.md)), and a `%f`/`%e` rounding carry that subtracted 10 instead of 10^precision (F66)
in embfmt. Both are described in [`docs/port_review.md`](docs/port_review.md) and are
worth reporting upstream. **`./build.sh` applies them and will not build without them** —
see [Get the sources](#get-the-sources) for why that matters.

### Printing and logging

In this project the memory-heavy `printf()` is replaced by the more embedded-optimized `MSG()` function backed by the `embfmt` library. Parameters and format specifiers are fully `printf()` compatible.

### CLI commands

The software offers you with the following multitude, most flexPTP-related of commands:

```
?                                                  Print this help (23/64)
hist                                               Print command history
osinfo                                             Print OS-related information
flexptp                                            Start flexPTP daemon
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

Listed in the order the firmware emits them, captured from the running board. The `(23/64)`
on the first line is the CLI's own registered/available slot count.

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
| [`docs/architecture.md`](docs/architecture.md) | Module inventory and versions, layering, boot sequence, threads and priorities, interrupts, clocking, the PTP timestamp path, memory layout |
| [`docs/design_doc.md`](docs/design_doc.md) | Porting decisions and their rationale, submodule pinning, clock tree, pin map, stage-by-stage log |
| [`docs/ptp_clock.md`](docs/ptp_clock.md) | Full derivation of the PTP timestamp clock, and why a 4 ns increment is possible but not adopted |
| [`docs/ptp_profiles.md`](docs/ptp_profiles.md) | The three profile presets, the transport and delay-mechanism trade-offs, and what a switched network costs |
| [`docs/host_ptp_setup.md`](docs/host_ptp_setup.md) | Running a PTP master on a Linux host, and what to check when nothing happens |
| [`docs/port_review.md`](docs/port_review.md) | Every finding of the port review, with evidence — including the ones that cost days |
| [`docs/implementation_plan.md`](docs/implementation_plan.md) | The staged work plan and what remains |
| [`docs/threadx_netxduo_feasibility.md`](docs/threadx_netxduo_feasibility.md) | What porting to Eclipse ThreadX and NetX Duo would cost, and the one flexPTP defect that would break it silently |
| [`docs/threadx_porting_plan.md`](docs/threadx_porting_plan.md) | The staged execution plan for that port — hardware checkpoints, the host test tree, and progress tracking |
| [`docs/threadx_s5e_plan.md`](docs/threadx_s5e_plan.md) | The one stage left undone: running the real stack on the host, what it would and would not have caught, and a cheaper alternative |
| [`docs/reviews/`](docs/reviews/README.md) | Archive of the independent reviews that fed `port_review.md` — historical, superseded by it where they disagree |

## Related papers and references

[Time Synchronization Extension for the IO-Link Industrial Communication Protocol](https://ieeexplore.ieee.org/document/10747727)

[Distributed Measurement System for Performance Evaluation of Embedded Clock Synchronization Solutions](https://ieeexplore.ieee.org/document/9805958/)

[Portable, PTP-based Clock Synchronization Implementation for Microcontroller-based Systems and its Performance Evaluation](https://ieeexplore.ieee.org/document/9615250)

[Synchronization of Sampling in a Distributed Audio Frequency Range Data Acquisition System Utilizing Microcontrollers](https://ieeexplore.ieee.org/document/9918455/)

[Methods of Peripheral Synchronization in Real-Time Cyber-Physical Systems](https://ieeexplore.ieee.org/document/10178979/)

## License

The project was created by András Wiesner (Epagris) in 2025 and published under the MIT license. This specific port was created by Damien Dusha (also under the MIT license) with lots of help from Claude Opus 5.0 and Gemini Flash 3.6.

Contributions are welcome! :)
