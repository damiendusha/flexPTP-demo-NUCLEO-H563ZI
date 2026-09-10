#!/usr/bin/env bash
#
# File the flexPTP fixes upstream, ONE PULL REQUEST AT A TIME.
#
# Each fix is an independent branch off the pinned upstream commit and touches a disjoint set
# of lines, so they can be reviewed, merged, rejected or reordered in any combination. That is
# the whole reason they are separate branches rather than one, and it is why this script asks
# before each one rather than firing seven PRs at a maintainer in a burst.
#
#   tools/upstream/file_flexptp_prs.sh --dry-run     # show exactly what would be filed
#   tools/upstream/file_flexptp_prs.sh               # ask before each PR, then file it
#   tools/upstream/file_flexptp_prs.sh --only u5     # just one
#
# Nothing is pushed and no PR is opened until you answer `y` to that specific PR. Answer `q`
# to stop; branches already filed stay filed.
#
# Requirements: `gh` authenticated with `repo` scope, and a fork you can push to.
#
set -euo pipefail

CLONE="${FLEXPTP_CLONE:-$HOME/git/flexPTP-fixes}"
UPSTREAM="${FLEXPTP_UPSTREAM:-epagris/flexPTP}"
BASE="${FLEXPTP_BASE:-master}"
FORK_REMOTE="${FLEXPTP_FORK_REMOTE:-fork}"

DRY_RUN=0
ONLY=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1 ;;
        --only)    ONLY="${2:?--only needs a tag, e.g. u5}"; shift ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

die() { echo "error: $*" >&2; exit 1; }

# --- The fixes, in the order they are worth a maintainer's attention ------------------------
#
# Ordered by consequence, not by number: a maintainer who reads only the first two should have
# read the two that lose data. u6 is a comment and goes last on purpose.
#
#   tag | branch | one-line PR title
FIXES=(
  "u1|fix/u1-gm-clock-identity-byte-order|Announce: stop byte-swapping the grandmaster clock identity on receive"
  "u2|fix/u2-logid-config-bit|config: store the logid flag, and derive CONFIG_LOG_ALL from the flags"
  "u5|fix/u5-stale-pbuf-tx-cb|lwIP NSD: untag the pbuf before releasing it"
  "u7|fix/u7-transport-type-reset|settings: reset the engine when the transport type changes"
  "u4|fix/u4-p2p-8023-dest-sticky|lwIP NSD: recompute the custom P2P 802.3 destinations on every init"
  "u3|fix/u3-locked-log-prefix|stats: print the [LOG-LCKD] prefix only when the line behind it prints"
  "u6|fix/u6-pdelay-pcb-comment|lwIP NSD: say that the PDelay pcbs are receive-only"
)

# --- Preflight -------------------------------------------------------------------------------

[[ -d "$CLONE/.git" ]] || die "no git clone at $CLONE (set FLEXPTP_CLONE)"
command -v gh >/dev/null || die "gh is not installed"
gh auth status >/dev/null 2>&1 || die "gh is not authenticated (run: gh auth login)"

cd "$CLONE"

git remote get-url "$FORK_REMOTE" >/dev/null 2>&1 \
    || die "no '$FORK_REMOTE' remote in $CLONE. Add your fork:
    git -C $CLONE remote add $FORK_REMOTE https://github.com/<you>/flexPTP.git"

FORK_URL="$(git remote get-url "$FORK_REMOTE")"
FORK_OWNER="$(sed -E 's#.*[:/]([^/]+)/[^/]+(\.git)?$#\1#' <<<"$FORK_URL")"
[[ -n "$FORK_OWNER" ]] || die "could not read the fork owner from $FORK_URL"

if [[ -n "$(git status --porcelain)" ]]; then
    die "$CLONE has uncommitted changes; commit or stash them first"
fi

echo "clone     $CLONE"
echo "fork      $FORK_OWNER ($FORK_URL)"
echo "upstream  $UPSTREAM, base branch $BASE"
echo

# --- One PR at a time ------------------------------------------------------------------------

filed=0
skipped=0

for entry in "${FIXES[@]}"; do
    IFS='|' read -r tag branch title <<<"$entry"
    [[ -n "$ONLY" && "$ONLY" != "$tag" ]] && continue

    git rev-parse --verify "$branch" >/dev/null 2>&1 || { echo "!! no branch $branch, skipping"; continue; }

    # The commit message body IS the PR body. It was written to explain the defect, the
    # mechanism and the measurement, which is exactly what the PR has to say. The
    # Co-Authored-By trailer belongs on the commit, not in the prose a maintainer reads, so it
    # is stripped and the attribution footer added in its place.
    body="$(git log --format=%b -1 "$branch" | sed '/^Co-Authored-By:/d' | sed -e :a -e '/^\n*$/{$d;N;};/\n$/ba')"
    body="$body

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
    # Each branch is exactly one commit off the pinned upstream revision, so the branch tip's
    # own diffstat is the whole change.
    files="$(git show --stat --format= "$branch")"

    echo "────────────────────────────────────────────────────────────────────────"
    echo "[$tag] $title"
    echo "     branch: $branch"
    echo "$files" | sed 's/^/     /'
    echo

    # Already filed? Do not open a second one.
    existing="$(gh pr list --repo "$UPSTREAM" --head "$FORK_OWNER:$branch" --state all \
                  --json number,state,url --jq '.[0] | select(.) | "#\(.number) \(.state) \(.url)"' 2>/dev/null || true)"
    if [[ -n "$existing" ]]; then
        echo "     already filed: $existing"
        echo "     skipping."
        skipped=$((skipped + 1))
        continue
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        echo "     --- PR body ---"
        echo "$body" | sed 's/^/     /'
        echo "     --- would run ---"
        echo "     git push $FORK_REMOTE $branch"
        echo "     gh pr create --repo $UPSTREAM --base $BASE --head $FORK_OWNER:$branch ..."
        echo
        continue
    fi

    read -r -p "     File this pull request? [y/N/q] " answer
    case "$answer" in
        [Yy]*) ;;
        [Qq]*) echo "     stopping."; break ;;
        *)     echo "     skipped."; skipped=$((skipped + 1)); continue ;;
    esac

    echo "     pushing $branch to $FORK_REMOTE..."
    git push --quiet "$FORK_REMOTE" "$branch":"$branch"

    echo "     opening pull request..."
    url="$(gh pr create \
             --repo "$UPSTREAM" \
             --base "$BASE" \
             --head "$FORK_OWNER:$branch" \
             --title "$title" \
             --body "$body")"
    echo "     filed: $url"
    filed=$((filed + 1))
    echo
done

echo "────────────────────────────────────────────────────────────────────────"
if [[ $DRY_RUN -eq 1 ]]; then
    echo "dry run: nothing pushed, nothing filed."
else
    echo "filed $filed pull request(s), skipped $skipped."
fi
