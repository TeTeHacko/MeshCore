/* Linker script to configure memory regions. */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x27000, LENGTH = 0xD4000 - 0x27000

  /* SRAM required by Softdevice depend on
   * - Attribute Table Size (Number of Services and Characteristics)
   * - Vendor UUID count
   * - Max ATT MTU
   * - Concurrent connection peripheral + central + secure links
   * - Event Len, HVN queue, Write CMD queue
   */ 
  /* CUSTOM (TeTeHacko): reserve 16 B at the end of RAM for a hardfault record
   * (PC/LR survive soft reset, printed on next boot) — see HardFault_Handler
   * in the framework and the boot-time printout in main.cpp.
   *
   * MEASURED 7. 8. 2026, AND IT UNDERMINES THE PREMISE: keeping an address out of
   * the linker's region is NOT enough to survive a soft reset on this hardware.
   * A clock record was tried at 0x2003FFE0 (32 B reserved) and came back as
   * `00000000 10001000 00000000` after a `reboot` — not zeroed, OVERWRITTEN, and
   * 0x10001000 is the nRF52 UICR base, i.e. a UICR pointer left on somebody's
   * stack. The Adafruit bootloader runs before the app on every reset and links
   * against the full RAM, so its stack starts at 0x20040000 and grows down
   * through this area; it reached at least 24 B down.
   *
   * The hardfault record at 0x2003FFF0 is HIGHER, i.e. the first thing that stack
   * touches, so it is unlikely to be any safer. Nothing here has ever
   * demonstrated it surviving. Do not build anything new on this mechanism
   * without proving retention first, and treat a missing hardfault report as
   * "unknown" rather than "no fault happened".
   */
  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x2003FFF0 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM
  
  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
