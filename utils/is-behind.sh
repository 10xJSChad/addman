#!/bin/sh
git fetch origin
git status -uno | grep -q 'is behind' && exit 1 || exit 0