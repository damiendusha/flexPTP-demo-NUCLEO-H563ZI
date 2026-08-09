# flexPTP patches

Patches against the pinned flexPTP submodule (`76aaf67`, `RC1.0-11`).

The submodule is **not** modified in git — the parent repository records only its commit
pointer, so any working-tree change inside `Modules/flexptp` is invisible to a clone and
is destroyed by `git submodule update`. Anything that must survive lives here.

## Applying

```bash
git -C Modules/flexptp apply ../../patches/flexptp/<name>.patch
```

## Reverting

```bash
git -C Modules/flexptp checkout -- src/
```

## Contents

### `0001-temporary-F42-instrumentation.patch` — **temporary, do not ship**

Counters added while investigating F42 (PTP fails to complete the E2E delay exchange).
Adds three volatile debug structs, readable over SWD, with no behavioural change:

| Struct | File | Measures |
|---|---|---|
| `ptpdbg` | `task_ptp.c` | transmit-timestamp callbacks, `PTN_TRANSMIT_DONE` handling, `msgb` uid/tag hits and misses |
| `slvdbg` | `slave.c` | messages entering the slave handler, Delay_Resp arrivals, delay-mechanism and sequence-ID matching |
| `coredbg` | `ptp_core.c` | messages reaching the core dispatcher, Delay_Resp arrivals, domain/`transportSpecific` filtering |
| `nsddbg` | `port/example_netstack_drivers/nsd_lwip.c` | what lwIP hands to the driver: callback count, PCB (event/general), and a per-message-type histogram |

Read them with, e.g.:

```bash
ADDR=$(arm-none-eabi-nm build/debug/flexptp-demo.elf | grep ' coredbg' | cut -d' ' -f1)
toolchains/openocd/bin/openocd -f board/st_nucleo_h5.cfg \
  -c init -c halt -c "mdw 0x$ADDR 8" -c resume -c shutdown
```

**This patch exists to diagnose a defect, not to fix one.** Drop it once F42 is resolved.
