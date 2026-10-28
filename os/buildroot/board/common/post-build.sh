#!/bin/sh
# Runs after the target filesystem is assembled. $1 is the target directory.
set -eu
target=$1

# The Oscairn session starts its own X server (/usr/libexec/oscairn-session); Buildroot's
# X.org package would start a second one.
rm -f "$target/etc/init.d/S40xorg"
