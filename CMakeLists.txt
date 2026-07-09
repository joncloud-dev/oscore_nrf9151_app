# SPDX-License-Identifier: Apache-2.0
#
# Minimal OSCORE cloud sample for the nRF9151 DK. The oscore_cloud library is
# pulled in as a Zephyr module by the west manifest (west.yml), so it is built
# and linked automatically when CONFIG_OSCORE_CLOUD=y; this app only compiles
# its own main().

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(oscore_nrf9151_app)

target_sources(app PRIVATE
	src/main.c
	src/network.c
)
