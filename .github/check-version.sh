#!/bin/sh
#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# check-version.sh — fail a pull request that changes the code or the build
# but not VTESSE_VERSION in include/vtesse/version.h.  A new version must be
# MAJOR.MINOR.PATCH without leading zeros, greater than the base one, and
# have its section in CHANGELOG.md ("## <version> — YYYY-MM-DD") that the
# base branch does not have yet.  The sources, android/ (except its unit
# tests), the Makefile, ci/, scripts/ and the build workflow
# (.github/workflows/build.yml) count as code or build, without the Markdown
# files in them (server/README.md, android/README.md); docs, tests/, bench/,
# screenshots and the rest of .github/ (version.yml, which runs this check,
# included) need no new version.  The script does
# not tell the build steps of the workflow from its test or release steps:
# NO_RELEASE=true (the pull request's `no-release` label) lets a change to
# the code or the build through without a new version, for changes that
# leave the release files as they are (a test or release step, a comment).
# The format is checked only when the version changes, so the MAJOR.MM
# versions up to 1.12 stay valid until the next code change.
#
# Usage: .github/check-version.sh [BASE]   (default origin/main; CI passes
# HEAD^1, the base side of the pull request's merge commit)
#

set -eu

base=${1:-origin/main}
header=include/vtesse/version.h
code="include netcore cli vtun port server scripts third_party tools android Makefile ci .github/workflows/build.yml"

# The one VTESSE_VERSION definition (comments and other text do not count).
ver() {
	v=$(sed -n 's/^#define VTESSE_VERSION "\([^"]*\)"[[:space:]]*$/\1/p')
	if [ "$(printf '%s\n' "$v" | grep -c .)" -ne 1 ]; then
		echo "::error::$1:$header: expected one VTESSE_VERSION definition," \
			"found: $(printf '%s' "$v" | tr '\n' ' ')" >&2
		exit 1
	fi
	printf '%s\n' "$v"
}

new=$(ver HEAD < "$header")
old=$(git show "$base:$header" | ver "$base")

# shellcheck disable=SC2086 # $code is a list of paths
changed=$(git diff --name-only "$base"...HEAD -- $code ':(exclude)android/app/src/test' \
	':(exclude,glob)**/*.md')

if [ "$new" = "$old" ]; then
	if [ -n "$changed" ] && [ "${NO_RELEASE:-}" = true ]; then
		echo "::notice::code or build changed, VTESSE_VERSION stays $old: the no-release" \
			"label says the release files do not change"
		echo "$changed"
		exit 0
	fi
	if [ -n "$changed" ]; then
		echo "::error::code or build changed, VTESSE_VERSION is still $old:" \
			"bump PATCH (fix) or MINOR (feature) and add its CHANGELOG.md section," \
			"or label the pull request no-release if the release files do not change"
		echo "$changed"
		exit 1
	fi
	echo "version $new unchanged, no code or build changes"
	exit 0
fi

if ! echo "$new" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
	echo "::error::VTESSE_VERSION \"$new\" is not MAJOR.MINOR.PATCH"
	exit 1
fi

# A MAJOR.MM version from before MAJOR.MINOR.PATCH is MAJOR.MM.0: 1.12.0 is 1.12.
case $old in
*.*.*) oldn=$old ;;
*) oldn=$old.0 ;;
esac
top=$(printf '%s\n%s\n' "$oldn" "$new" | sort -V | tail -n 1)
if [ "$new" = "$oldn" ] || [ "$top" != "$new" ]; then
	echo "::error::VTESSE_VERSION $new is not greater than $old"
	exit 1
fi

# $new is digits and dots only (checked above): escape the dots, require a date.
re=$(printf '%s' "$new" | sed 's/\./\\./g')
if ! grep -Eq "^## $re — [0-9]{4}-[0-9]{2}-[0-9]{2}\$" CHANGELOG.md; then
	echo "::error::CHANGELOG.md has no section \"## $new — YYYY-MM-DD\""
	exit 1
fi

# The section must come with this pull request, not be one the base
# branch already has (a version taken from there brings no notes).
if git show "$base:CHANGELOG.md" 2>/dev/null | grep -Eq "^## $re — "; then
	echo "::error::the section \"## $new\" is already on $base: take a new version"
	exit 1
fi

echo "version $old -> $new, CHANGELOG.md section found"
