#!/bin/sh
if [ -d ".release" ]; then
    rm -rf .release
fi

mkdir .release
cp -r ElvUI .release
cp -r ElvUI_Options .release
cp -r ElvUI_Libraries .release
exit 0