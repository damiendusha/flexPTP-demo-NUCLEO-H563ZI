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
