# SPDX-License-Identifier: Apache-2.0
#
# Give MCUboot the same flash layout as the application, on both boards.
#
# Board .overlay / .conf files under boards/ apply only to the application
# image. This app grows slot0 and moves slot1 to the external SPI NOR chip (see
# boards/oscore_flash_layout.h), and MCUboot has to agree: it needs the driver to
# reach the secondary slot, and a bootloader that disagreed about slot0's bounds
# would write its image trailer outside the slot. On the Thingy:91 X it also
# needs boot_partition to exist at all, since its own app.overlay chooses it and
# the factory layout has no such node.
#
# Sysbuild includes this file for the main application image, so it is the place
# to reach the other images.

if(SB_CONFIG_BOARD_NRF9151DK_NRF9151_NS)
  set(bl_partitions ${CMAKE_CURRENT_LIST_DIR}/boards/nrf9151dk_bl_partitions.overlay)
elseif(SB_CONFIG_BOARD_THINGY91X_NRF9151_NS)
  set(bl_partitions ${CMAKE_CURRENT_LIST_DIR}/boards/thingy91x_bl_partitions.overlay)
endif()

if(DEFINED bl_partitions)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${bl_partitions}
      CACHE INTERNAL "MCUboot flash layout" FORCE
  )
  set(mcuboot_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/oscore_bl_extflash.conf
      CACHE INTERNAL "MCUboot external-flash access" FORCE
  )
endif()
