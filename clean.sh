#!/bin/sh
set -e

if git status -s | grep -e '.*' -q; then
	echo "tree modified, won't blow it away"
	exit 1
fi

git submodule deinit --all
git clean -fXd
