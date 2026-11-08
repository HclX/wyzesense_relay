# Writing a unit's CCFG (flash page 31)

The user's rule since 2026-11-07, replacing "never write a unit's CCFG except enabling `BL_CONFIG`". A CCFG write is
not dangerous as such; a few fields, and the moment page 31 is erased, are. The rule names those.

## What the ROM does with the CCFG (from `ground_truth/firmware/cc13x0_rom/ROM.bin`, read 2026-11-07)

The ROM reads the CCFG through its window at `0x50003FA8` (= flash `0x1FFA8`):

| ROM code | What it does |
|---|---|
| `0x10003B4E`, `0x10003C00`-`0x10003C16` | each debug port (CPU DAP, PRCM, TEST, PBIST, WUC) opened only if its byte in `CCFG_TAP_DAP_0/1` is `0xC5`, ANDed with FCFG1's factory setting, into `AON_WUC:JTAGCFG` |
| `0x10003B2A` | the flash image is started only if `IMAGE_VALID_CONF == 0` and the backdoor is not active |
| `0x10003A90` | the backdoor: only if `BL_CONFIG[7:0] == 0xC5`; the pin `BL_CONFIG[15:8]` set to input **with the internal pull-up** (`IOCFG = 0x20004000`), active when it equals `BL_CONFIG[16]` (so a floating pin never triggers it) |
| `0x10002DB4` | the bootloader acts on a command only if `BL_CONFIG[31:24] == 0xC5` (and FCFG1 `0x2A0[15:8] == 0xC5`); otherwise it drops every command |

So an **erased CCFG** (all `FF`) closes both ordinary ways in: the image counts as invalid, the bootloader ignores
commands, and the ROM opens no debug port. What remains is TI's forced mass erase through ICEPick (UniFlash / an
XDS110; not in `cjtag.c`), which wipes the whole flash, the MAC with it (hypothesis: not tried). A page-31 erase is
therefore the one real risk: a power cut between the erase and the rewrite (a few milliseconds).

## The rule

1. **Never, on any unit:** turning a debug port off (`CCFG_TAP_DAP_0/1` keep their bytes), setting a protection bit
   (`CCFG_PROT_*`) or an erase lock (`ERASE_CONF`), changing `IMAGE_VALID_CONF`, or a bootloader the relay cannot reach
   (`BL_CONFIG` disabled where it was on, or its backdoor on a pin nothing drives). And **never page 31 of a dongle**,
   in any way: its bootloader through the CH554 is its only way back.
2. **Free (with the guards): program-only writes** into blank (`FF`) bytes, no erase: they cannot change anything
   else. Today: filling a blank CCFG MAC slot with the unit's page-29 MAC (`cjtag_fill_ccfg_mac`).
3. **With the user's standing OK, under conditions: an erase-and-rewrite of page 31** that keeps every field of 1:
   the backdoor moved to a pin the relay drives (`cjtag_set_backdoor`), a disabled bootloader turned on, page 31's
   code area (`cjtag_write_page31_code`, the stock-firmware work), or a unit's own backed-up page 31 put back.
   Conditions: the unit has a backup with its CCFG; `M`/`C` are wired; stable power (a relay on USB, never a
   battery); the guards (page 31 read twice and matched with the connect's CRC, the whole page verified after, the
   original written back on a failure); and the write logged in the unit's backup folder (what, when, before/after).

Every page-31 writer in `esp32/contact/cjtag.c` runs the same check (`ccfg_safe`) on the page it is about to write:
1's fields as on the chip (or as the operation allows), the bootloader on. Prefer 2 to 3 wherever both work.
