#!/usr/bin/env bash
# Copyright 2023, Collabora, Ltd.
#
# SPDX-License-Identifier: BSL-1.0

# Set up the TCP tunnels needed for our WebRTC connection.

set -x
set -e

# adb logcat -c
adb reverse tcp:61943 tcp:61943
adb reverse tcp:8080 tcp:8080
