#!/usr/bin/env python3
"""Check relative links and in-document anchors across the Markdown in this repo.

The docs cross-reference each other heavily and several findings in the review were stale
cross-references. Broken links are cheap to prevent and expensive to notice by eye.

Reports, per link:
  - relative file targets that do not exist
  - "#anchor" fragments that no heading in the target file generates
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
SKIP_PREFIX = ("http://", "https://", "mailto:", "file://")
LINE_FRAG = re.compile(r"L\d+(?:-L\d+)?")

# Source files the ThreadX/NetX Duo port deleted, which historical documents still reference.
#
# docs/reviews/ is an archive of reviews of the FreeRTOS/lwIP firmware, and port_review.md
# records findings against it. Their links to these paths were correct on the day they were
# written; the files were removed later, by S3 of the ThreadX port. Rewriting the archive to
# hide that would falsify the record, and deleting the links would lose the reference.
#
# So they are accepted here instead, by exact path. The checker still catches every other
# broken link, including a typo in one of these names -- which is the property that matters.
# Remove an entry if the file ever comes back.
REMOVED_BY_THREADX_PORT = {
    "Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c",
    "Drivers/CMSIS/CMSIS_RTOS_V2/freertos_os2.h",
    "Drivers/CMSIS/CMSIS_RTOS_V2/freertos_mpool.h",
    "Drivers/EthDrv/eth_drv_lwip.c",
    "Drivers/EthDrv/eth_drv_lwip.h",
    "Src/ethernet/ethernet_lwip.c",
    "Src/ethernet/ethernet_lwip.h",
    "Inc/FreeRTOSConfig.h",
    "Inc/lwipopts.h",
    "Modules/lwip_port/arch/cc.h",
    "Modules/lwip_port/arch/cpu.h",
    "Modules/lwip_port/arch/sys_arch.h",
    "Modules/lwip_port/sys_arch.c",
}

# Source files removed with EtherLib, in the Bazel migration (decision D9 in
# docs/bazel_decisions.md). EtherLib was compile-only and had never been run on hardware.
# Same treatment, and same reasoning, as the set above: the documents that link to these were
# correct when written, and rewriting the archive to hide the removal would falsify the record.
REMOVED_WITH_ETHERLIB = {
    "Drivers/EthDrv/eth_drv_etherlib.c",
    "Drivers/EthDrv/eth_drv_etherlib.h",
    "Inc/etherlib_options.h",
    "Src/etherlib_options.c",
    "Src/ethernet/ethernet_etherlib.c",
    "Src/ethernet/ethernet_etherlib.h",
    "Modules/etherlib",
}

# The CMake build, and the submodule trees, removed in phase 7 of the Bazel migration.
#
# Two groups, and the second is the interesting one:
#
#   - the build files themselves. Documents that describe how the firmware was built were
#     correct when written, and the reasoning in them -- the -Og measurements, finding H-3's
#     --gc-sections, the LINK_DEPENDS trap -- is why several decisions in
#     docs/bazel_decisions.md are defensible. Rewriting those references to point at BUILD.bazel
#     would attribute the wrong file.
#   - paths under Modules/. Those trees were git submodules and are now git repositories
#     fetched by Bazel, so they no longer exist in a checkout at all. A reader can still reach
#     them: `bazel fetch @flexptp` puts the patched tree in the repository cache.
REMOVED_WITH_CMAKE = {
    "build.sh",
    "cmake/gcc-arm-none-eabi.cmake",
    "CMakeLists.txt",
    "CMakePresets.json",
    ".gitmodules",
    "Drivers/CMakeLists.txt",
    "Middlewares/CMakeLists.txt",
    "Middlewares/netxduo.cmake",
    "Middlewares/threadx.cmake",
    "Modules/CMakeLists.txt",
    "projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/CMakeLists.txt",
    "projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/test/CMakeLists.txt",
}


def anchors(path):
    """GitHub-style slugs for every ATX heading in a Markdown file."""
    out = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^#{1,6}\s+(.*?)\s*$", line)
        if not m:
            continue
        s = m.group(1).lower()
        s = re.sub(r"[`*~]", "", s)   # inline markup -- NOT underscore, which GitHub keeps
        s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)  # link text only
        s = re.sub(r"[^\w\s-]", "", s)
        # GitHub replaces EACH space with a hyphen; it does not collapse runs. So a heading
        # like "F3 — No ETH PTP clock mux" loses the em-dash but keeps both surrounding
        # spaces, yielding "f3--no-eth-ptp-clock-mux" with a DOUBLE hyphen. Collapsing here
        # produced 51 false positives on a docs tree whose links were in fact correct.
        out.add(s.strip().replace(" ", "-"))
    return out


def main():
    files = sorted(p for p in ROOT.rglob("*.md")
                   if not any(part in {".git", "toolchains", "Modules", "Drivers", "build"}
                              for part in p.relative_to(ROOT).parts))
    bad = []
    cache = {}
    for f in files:
        for target in LINK.findall(f.read_text(encoding="utf-8", errors="replace")):
            if target.startswith(SKIP_PREFIX):
                continue
            frag = ""
            if "#" in target:
                target, frag = target.split("#", 1)
            dest = f.parent / target if target else f
            dest = dest.resolve()
            rel = f.relative_to(ROOT)
            if not dest.exists():
                try:
                    rel_dest = dest.relative_to(ROOT).as_posix()
                except ValueError:
                    rel_dest = None
                if (rel_dest in REMOVED_BY_THREADX_PORT
                        or rel_dest in REMOVED_WITH_ETHERLIB
                        or rel_dest in REMOVED_WITH_CMAKE
                        # Vendored trees are fetched by Bazel now, so nothing under Modules/
                        # exists in a checkout. `bazel fetch @<name>` reaches them.
                        or rel_dest.startswith("Modules/")):
                    continue
                bad.append(f"{rel}: missing file -> {target}")
                continue
            # "#L12" / "#L12-L34" are GitHub *line* references, not heading anchors. They
            # resolve on the blob view and cannot be validated against headings.
            if LINE_FRAG.fullmatch(frag):
                continue
            if frag and dest.suffix == ".md":
                if dest not in cache:
                    cache[dest] = anchors(dest)
                if frag.lower() not in cache[dest]:
                    bad.append(f"{rel}: no such anchor -> {target or dest.name}#{frag}")

    print(f"checked {len(files)} Markdown files")
    for b in bad:
        print(f"  BROKEN  {b}")
    print(f"{len(bad)} broken link(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
