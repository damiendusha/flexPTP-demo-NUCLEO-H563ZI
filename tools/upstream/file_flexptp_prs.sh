#!/usr/bin/env bash
#
# File the flexPTP fixes upstream, ONE PULL REQUEST AT A TIME.
#
# Each fix is an independent branch off the pinned upstream commit and touches a disjoint set
# of lines, so they can be reviewed, merged, rejected or reordered in any combination. That is
# the whole reason they are separate branches rather than one, and it is why this script asks
# before each one rather than firing every PR at a maintainer in a burst.
#
#   tools/upstream/file_flexptp_prs.sh --dry-run     # show exactly what would be filed
#   tools/upstream/file_flexptp_prs.sh               # ask before each PR, then file it
#   tools/upstream/file_flexptp_prs.sh --only u9     # just one
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
        --only)    ONLY="${2:?--only needs a tag, e.g. u9}"; shift ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

die() { echo "error: $*" >&2; exit 1; }

# --- The fixes, in the order they are worth a maintainer's attention ------------------------
#
# Ordered by consequence, not by number: a maintainer who reads only the first two should have
# read the two that break synchronisation outright -- any foreign Sync, or one malformed
# datagram, stops it; u14 next, which stops it for good after a burst. All are filed -- u13, u12
# and u11 as #37-#39, u14 as #40, u9, u10 and u8 as #34-#36, u1 and u6 from the first batch as
# #20 and #26 -- and listed so a re-run reports them as such. u16 and u15 are not filed yet. u16
# leaves stale CLI commands and removes other modules' when flexPTP stops; u15 is undefined
# behaviour that UBSan found and no board has shown, so it comes after every fix with a measured
# symptom. u6 is a comment and goes last on purpose. U2-U5 and U7 are merged and their branches
# deleted.
#
#   tag | branch | one-line PR title
FIXES=(
  "u13|fix/u13-slave-parent-only|slave: take Sync, Follow_Up and Delay_Resp from the selected master's port only"
  "u12|fix/u12-rx-length-version|receive: drop messages shorter than their header or messageLength, or not PTPv2"
  "u14|fix/u14-notification-fifo|task_ptp: make the notification FIFO as long as the four FIFOs it notifies for"
  "u11|fix/u11-teardown-order|unreg_task_ptp(): stop accepting work first, and stop the task where it holds nothing"
  "u9|fix/u9-teardown-unlock|lwIP NSD: make the shutdown branch reachable, and release the core lock in it"
  "u10|fix/u10-chained-pbufs|lwIP NSD: pass a chained pbuf's whole message, not its first segment"
  "u8|fix/u8-delay-resp-interval|master: Delay_Resp carries logMinDelayReqInterval, not the Delay_Req's 0x7F"
  "u1|fix/u1-gm-clock-identity-byte-order|Announce: stop byte-swapping the grandmaster clock identity on receive"
  "u16|fix/u16-cli-remove-handles|CLI: remove commands by their handles, last registered first"
  "u15|fix/u15-timestamp-unsigned-shifts|Timestamps: shift and byte-swap in unsigned types"
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

    # Already filed? Do not open a second one. `gh pr list --head` takes a bare branch name and
    # silently matches nothing given `owner:branch`, so the owner is filtered here instead. A
    # failed query stops the run: treating "could not ask" as "not filed" is how a duplicate
    # would get opened.
    existing="$(gh pr list --repo "$UPSTREAM" --head "$branch" --state all \
                  --json number,state,url,headRepositoryOwner \
                  --jq "[.[] | select(.headRepositoryOwner.login == \"$FORK_OWNER\")][0] | select(.) | \"#\\(.number) \\(.state) \\(.url)\"")" \
        || die "could not query $UPSTREAM for an existing PR from $FORK_OWNER:$branch"
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
