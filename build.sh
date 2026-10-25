#!/bin/sh
# Static armv7 build of cecdump. Run inside the builder container.
set -e
cd "$(dirname "$0")"
CC=arm-linux-musleabi-gcc
$CC -Os -static -no-pie -Wall -Wextra \
    -march=armv7-a -mtune=cortex-a15 -mfloat-abi=soft \
    -ffunction-sections -fdata-sections \
    -Wl,--gc-sections \
    -o cecdump cecdump.c -lpthread
arm-linux-musleabi-strip cecdump
file cecdump
