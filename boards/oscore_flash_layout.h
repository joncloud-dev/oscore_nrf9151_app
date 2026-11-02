/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * One flash layout for both supported boards, so there is a single image-size
 * budget to reason about and the two soak units behave identically. The nRF9151
 * is the same 1 MiB part on the nRF9151 DK and the Thingy:91 X; only their stock
 * devicetrees differ, so each board has a small .dtsi that deletes what it ships
 * and builds this layout from these numbers.
 *
 * Internal flash (flash0, 1 MiB):
 *   0x00000000  boot_partition            (  64 KB)  MCUboot, first stage
 *   0x00010000  slot0_partition (image-0) ( 896 KB)
 *     0x00010000  slot0_s_partition       ( 192 KB)  TF-M full profile (~127 KB used)
 *     0x00040000  slot0_ns_partition      ( 704 KB)  application
 *   0x000f0000  tfm_ps_partition          (  16 KB)  TF-M Protected Storage
 *   0x000f4000  tfm_its_partition         (   8 KB)  TF-M Internal Trusted Storage
 *   0x000f6000  tfm_otp_partition         (   8 KB)  TF-M OTP / NV counters
 *   0x000f8000  storage_partition         (  32 KB)  settings NVS, 8 x 4 KB sectors
 *
 * External flash (32 MiB SPI NOR on both boards):
 *   0x00000000  slot1_partition (image-1) ( 896 KB)  MUST equal slot0
 *     0x00000000  slot1_s_partition       ( 192 KB)
 *     0x00030000  slot1_ns_partition      ( 704 KB)
 *   0x000e0000  pgps_storage              ( 336 KB)  84 x 4 KB predicted windows = 14 days
 *   0x00134000  external_flash_partition  (~30.8 MB) free space
 *
 * The MCUboot secondary slot lives on external flash. Keeping it internal costs
 * 448 KB of the 1 MiB, which left the application only 256 KB -- enough for the
 * DK build with ~27 KB spare, but not for the Thingy:91 X, which additionally
 * links the nPM1300 charger, I2C, MFD and regulator drivers. Moving it out gives
 * the application 704 KB on both boards and keeps a full-size upgrade slot for
 * when FOTA is re-enabled. MCUboot therefore needs the SPI NOR driver, which
 * sysbuild.cmake supplies.
 *
 * The addresses of slot0_s / slot0_ns and of the TF-M and settings partitions are
 * deliberately unchanged from what the DK ran before, so the application's link
 * address is the same and provisioned OSCORE key material in TF-M storage
 * survives the layout change.
 */

#ifndef OSCORE_FLASH_LAYOUT_H_
#define OSCORE_FLASH_LAYOUT_H_

#define OSCORE_BOOT_OFFSET     0x00000000
#define OSCORE_BOOT_SIZE       0x00010000

#define OSCORE_SLOT_OFFSET     0x00010000	/* internal; slot1 sits at 0 on external */
#define OSCORE_SLOT_SIZE       0x000e0000
#define OSCORE_SLOT_S_SIZE     0x00030000
#define OSCORE_SLOT_NS_OFFSET  0x00030000	/* relative to the slot */
#define OSCORE_SLOT_NS_SIZE    0x000b0000

#define OSCORE_TFM_PS_OFFSET   0x000f0000
#define OSCORE_TFM_PS_SIZE     0x00004000
#define OSCORE_TFM_ITS_OFFSET  0x000f4000
#define OSCORE_TFM_ITS_SIZE    0x00002000
#define OSCORE_TFM_OTP_OFFSET  0x000f6000
#define OSCORE_TFM_OTP_SIZE    0x00002000
#define OSCORE_SETTINGS_OFFSET 0x000f8000
#define OSCORE_SETTINGS_SIZE   0x00008000

#define OSCORE_PGPS_OFFSET     0x000e0000	/* external, after slot1 */
#define OSCORE_PGPS_SIZE       0x00054000	/* 84 x 4 KiB windows */
#define OSCORE_EXTFREE_OFFSET  0x00134000
#define OSCORE_EXTFREE_SIZE    0x01ecc000

#endif /* OSCORE_FLASH_LAYOUT_H_ */
