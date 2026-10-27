# Example: TI LAUNCHXL-F280049C (C2000) application + bootloader

A bootloader project and an application project that use the portable core in `src/` with the C2000 flash port in
`ports/c2000/f28004x`. It is the integration that was exercised on hardware (see `docs/TEST_RESULTS.md` for exactly what
is evidenced and what is only maintainer-reported).

**This is a reference integration, not a turnkey project.** The repository does not contain CCS project files (they hold
machine-specific absolute paths), TI template files (`device.c`, `f28004x_codestartbranch.asm`), the TI C2000Ware library, or the
FC41D module driver. You create two CCS projects (application, bootloader) from TI's empty driverlib template, add the files
listed in section 2.1, set the compiler/linker options of section 2, and use the linker command files and `c2000.syscfg` from here.

What is in this directory:

| Path | Purpose |
|---|---|
| `application/main.c` | application entry: device/FC41D setup and main loop (TI header block kept; Wi-Fi credentials are placeholders: set your own) |
| `application/port/` | glue between the core and the hardware: `app_fwu.c` (session setup, reboot), `app_ble.c` (FC41D BLE transport), `app_pull.c` (HTTP pull over FC41D TCP), `app_tick.c` (1 ms tick), `app_sci.c` (UART transport) and `app_fwu_config.h` |
| `application/28004x_app_flash_lnk.cmd` | application linker command file (flash 0x083000.., RAM sections incl. `ramgs2`) |
| `application/flash_port/flash_port_config.h` | which flash ranges the application project may write |
| `application/image_tools/` | `make_image.bat`, `finalize_image.ps1`, `app_bin.hexcfg`: turn the linked `.out` into the raw image the host sends |
| `application/flash_tools/` | DSLite helper scripts (`flash_boot.bat`, `flash_boot_keep.bat`, `flash_app.bat`) |
| `application/c2000.syscfg`, `bootloader/c2000.syscfg` | SysConfig configuration (pins, SCI, LEDs) |
| `bootloader/main.c`, `bootloader/port/boot_fwu_config.h` | bootloader: runs `fwu_boot_start`, jumps to 0x083000 |
| `bootloader/28004x_bootloader_flash_lnk.cmd`, `bootloader/flash_port/flash_port_config.h` | bootloader linker file and write ranges |

Where it matters for reuse:

* The C2000 flash port (`flash_port.c`, `fwu_flash_adapter.c`) is shared by both projects and lives in `ports/c2000/f28004x`. Put that
  directory and the project's own `flash_port/` folder (for its `flash_port_config.h`) on the include path.
* The application and the bootloader must be built with the same profile, board, product id, chunk size and layout; otherwise the
  "contract fingerprint" of `fwu_boot.h` differs.
* Everything in sections 1 to 8 below is a record of what the files contain; "UNVERIFIED" marks what could not be confirmed.

---


# C2000 LaunchPad example (F280049C): verified facts

Scope: `examples/c2000_launchxl_f280049c/{application,bootloader}`. Everything below was read from files in this
repository, from the original CCS projects (`.project`, `.cproject`, generated makefiles, `.map` files, `.d` files)
or from the maintainer's local copy of the FC41D driver. Anything that could not be confirmed is marked
**UNVERIFIED**. Line numbers refer to the current files in the repository (comment translation did not change any line
count). Paths use placeholders:

| Placeholder | Meaning |
|---|---|
| `<fwu_git_repo>` | root of this repository |
| `<example>` | `<fwu_git_repo>/examples/c2000_launchxl_f280049c` |
| `<C2000Ware>` | TI C2000Ware install directory (`C2000Ware_26_01_00_00` in the original projects) |
| `<ti_cgt>` | TI C2000 code generation tools `ti-cgt-c2000_22.6.3.LTS` |
| `<fc41d_driver_root>` | directory holding `FC41D_api.c`, `FC41D_api.h`, `FC41D_bridge.*`, `INC/`, `SRC/` (NOT in this repository) |
| `<fc41d_parent_dir>` | parent of the product folder that contains `<product_dir>/FC41D/...` (used by the driver's own includes, see 2.4) |

Units: C28x addresses are 16-bit WORD addresses. `sizeof`/array sizes below are in 16-bit words (on C28x a `char` and
`fwu_byte_t` are both one 16-bit word; `src/fwu_types.h:36`).

---

## 1. Memory maps

### 1.1 Flash (both projects share one layout)

Sources: `src/config/layout_f280049c.h` (sector counts, lines 53-77; word-address arithmetic), `src/config/fwu_layout.h:46-51`
(region order), both `flash_port/flash_port_config.h`, both linker `.cmd` files.
Flash base 0x080000 (word address), Bank 0 = 0x080000..0x08FFFF (sectors 0-15), Bank 1 = 0x090000..0x09FFFF (sectors 16-31),
sector = 0x1000 words = 8192 octets, 32 sectors = 0x20000 words = 262144 octets
(`layout_f280049c.h:42-49`). Driver offsets are octet offsets from flash start; word address = `0x080000 + offset/2`
(`layout_f280049c.h:82`, `ports/c2000/f28004x/fwu_flash_adapter.c:30`).

| Region | Sectors | Start (word) | End (word, inclusive) | Size (words) | Notes |
|---|---|---|---|---|---|
| Bootloader | 0-2 | 0x080000 | 0x082FFF | 0x3000 | linker: `BEGIN` 0x080000..0x080001 (entry `codestart`), `BOOT_FLASH` 0x080002..0x082FFF (0x2FFE) (`28004x_bootloader_flash_lnk.cmd:7,15`) |
| Application | 3-12 | 0x083000 | 0x08CFFF | 0xA000 | linker: `APP_BEGIN` 0x083000..0x083001 (entry `codestart`, bootloader jumps here), `APP_FLASH` 0x083002..0x08CFFF (0x9FFE) (`28004x_app_flash_lnk.cmd:6,8`) |
| Staging: manifest page | 13 | 0x08D000 | 0x08DFFF | 0x1000 | `FWU_LAYOUT_STAGING_MANIFEST_OFFSET` |
| Staging: image area | 14-22 | 0x08E000 | 0x096FFF | 0x9000 | = 9 sectors = 73728 octets = `FWU_LAYOUT_IMAGE_MAX_SIZE`; crosses the bank boundary (sectors 13-15 in Bank 0 ending 0x08FFFF, sectors 16-22 start at 0x090000) |
| Log/config | 23-26 | 0x097000 | 0x09AFFF | 0x4000 | 4 sectors (`FWU_BOARD_LOG_PAGES` is derived); not writable by either project's `FLASH_PORT_WRITABLE`; no code in the example touches it |
| Counter A | 27 | 0x09B000 | 0x09BFFF | 0x1000 | `FWU_LAYOUT_COUNTER_A_OFFSET` |
| Counter B | 28 | 0x09C000 | 0x09CFFF | 0x1000 | `FWU_LAYOUT_COUNTER_B_OFFSET` |
| Journal A | 29 | 0x09D000 | 0x09DFFF | 0x1000 | `FWU_LAYOUT_JOURNAL_A_OFFSET` (commit record) |
| Journal B | 30 | 0x09E000 | 0x09EFFF | 0x1000 | `FWU_LAYOUT_JOURNAL_B_OFFSET` |
| Scratch (swap only) | 31 | 0x09F000 | 0x09FFFF | 0x1000 | `FWU_LAYOUT_SCRATCH_OFFSET`; used only by SWAP mode; NOT in either `FLASH_PORT_WRITABLE` |

Region sector counts: 3 / 10 / 10 / 4 / 2 / 3 (`layout_f280049c.h:53-74`; spare = 2 journal + 1 scratch).
Start addresses in the table are computed from `FWU_LAYOUT_*_FIRST_PAGE` in `fwu_layout.h:46-51` (boot 0, app 3, staging 13, log 23, counters 27, spare 29).

Write permission enforced by `flash_port` (`FLASH_PORT_WRITABLE(first, last)`, word addresses, inclusive range must lie fully inside one entry):

| Project | Writable word ranges | Source |
|---|---|---|
| Application | 0x08D000..0x096FFF (staging), 0x09D000..0x09EFFF (journal A and B) | `application/flash_port/flash_port_config.h:22-24` |
| Bootloader | 0x083000..0x08CFFF (application), 0x09B000..0x09CFFF (counters A and B), 0x09D000..0x09EFFF (journal A and B) | `bootloader/flash_port/flash_port_config.h:23-26` |

Reads are not restricted (header comments in both files). `flash_port.c` places its erase/write code in `.TI.ramfunc`
(`ports/c2000/f28004x/flash_port.c:37-44`); `FLASH_START_ADDRESS 0x080000`, `FLASH_END_ADDRESS 0x09FFFF` (`flash_port.c:9-10`).

Measured flash use (from the maintainer's local linker map files; **UNVERIFIED against a fresh build**, map dates 2026-10-21 13:20 for the application, 2026-10-19 13:24 for the bootloader; the files compared - `main.c`, `app_fwu.c`, `app_ble.c`, `app_pull.c`, `app_fwu_config.h`, `boot_fwu_config.h`, both linker files, both `flash_port_config.h`, `flash_port.c` - equal the CCS project sources except for the FC41D include path and the removed Wi-Fi credentials, see 2.4):

| Item | Value |
|---|---|
| Application `APP_BEGIN` + `APP_FLASH` used | 0x2 + 0x84A7 words = 0x84A9 words = 67922 octets (`APP_FLASH` unused: 0x1B57 words) |
| Application image file `app_test.bin` | 67936 octets (= 67922 rounded up to the 16-octet granule; matches), 92.1 % of the 73728-octet capacity, margin 5792 octets |
| Application `.ramfunc` (F021 Flash API + flash_port code) load image | 0xB5C words, loaded at 0x089CAC, run at RAMLS03 0x008000 |
| Bootloader `BEGIN` + `BOOT_FLASH` used | 0x2 + 0x296E words (`BOOT_FLASH` unused: 0x690 words = 1680 words) |

### 1.2 RAM (identical MEMORY blocks in both linker files; only the SECTIONS differ)

Source: `28004x_app_flash_lnk.cmd:2-31` and `28004x_bootloader_flash_lnk.cmd:2-32`. PAGE 0 / PAGE 1 are linker pages (program / data); the compiler uses `-mt` (unified memory, 2.2), so all of it is one address space.

| Block | Start | End (inclusive) | Length | Application use (SECTIONS) | Bootloader use (SECTIONS) |
|---|---|---|---|---|---|
| `BOOT_RSVD` (PAGE 1) | 0x000002 | 0x0000F2 | 0xF1 | reserved (boot ROM stack) | same |
| `RAMM0` (PAGE 0) | 0x0000F6 | 0x0003FF | 0x30A | declared, nothing placed | same |
| `RAMM1` (PAGE 1) | 0x000400 | 0x0007F7 | 0x3F8 | `.stack` (0x3F8 words; linker option `--stack_size=0x3F8`) | same |
| `RAMLS03` (PAGE 0) | 0x008000 | 0x009FFF | 0x2000 | `.TI.ramfunc` run area (0xB5C words used); `.bss:output`, `.bss:cio` | `.TI.ramfunc` run area (0xB2C words used) |
| `RAMLS4` (PAGE 0) | 0x00A000 | 0x00A7FF | 0x800 | declared, nothing placed | same |
| `RAMLS5` (PAGE 1) | 0x00A800 | 0x00AFFF | 0x800 | `.data` (0x16 words used), `.sysmem` (none linked: no `.sysmem` in the map) | `.bss` (0x3BC words) + `.data` (0xC words) + `.sysmem` |
| `RAMLS6` (PAGE 1) | 0x00B000 | 0x00B7FF | 0x800 | declared, nothing placed | same |
| `RAMLS7` (PAGE 1) | 0x00B800 | 0x00BFFF | 0x800 | declared, nothing placed | same |
| `RAMGS0` (PAGE 1) | 0x00C000 | 0x00DFFF | 0x2000 | `.bss` (0x1CA8 words used, 0x358 free) and section `ramgs0` | section `ramgs0` (nothing placed) |
| `RAMGS1` (PAGE 1) | 0x00E000 | 0x00FFFF | 0x2000 | section `debug_data` (FC41D driver object, 0x774 words) and `ramgs1` | section `ramgs1` (nothing placed) |
| `RAMGS2` (PAGE 1) | 0x010000 | 0x011FFF | 0x2000 | section `ramgs2` (`g_chunk`, 0x1000 words = 0x010000..0x010FFF) | not declared as a section |
| `RAMGS3` (PAGE 1) | 0x012000 | 0x013FF7 | 0x1FF8 | declared, nothing placed (0x013FF8..0x013FFF excluded per the errata comment in the .cmd) | same |
| `RESET` (PAGE 0) | 0x3FFFC0 | 0x3FFFC1 | 2 | `.reset` is `DSECT` (not used) | same |

Section placement summary (EABI branch is the one used: the projects compile with `--abi=eabi`, so `__TI_EABI__` is defined):

| Section | Application | Bootloader |
|---|---|---|
| `codestart` | `APP_BEGIN` | `BEGIN` |
| `.text`, `.cinit`, `.switch`, `.init_array`, `.const` | `APP_FLASH` | `BOOT_FLASH` |
| `.stack` | `RAMM1` | `RAMM1` |
| `.bss` | `RAMGS0` | `RAMLS5` |
| `.bss:output`, `.bss:cio` | `RAMLS03` | `RAMLS03` |
| `.data`, `.sysmem` | `RAMLS5` | `RAMLS5` |
| `ramgs0` / `ramgs1` / `ramgs2` / `debug_data` | `RAMGS0` / `RAMGS1` / `RAMGS2` / `RAMGS1` | `RAMGS0` / `RAMGS1` / - / - |
| `.TI.ramfunc` group (`-l F021_API_F28004x_FPU32_EABI.lib`) | LOAD `APP_FLASH`, RUN `RAMLS03`, symbols `RamfuncsLoadStart/Size/End`, `RamfuncsRunStart/Size/End` | LOAD `BOOT_FLASH`, RUN `RAMLS03`, same symbols |

The non-EABI branch of both `.cmd` files refers to `RAMLS0`, which is not declared in the MEMORY block; it is dead code with the project's `--abi=eabi`.

---

## 2. Sources, compiler and linker settings of the original CCS projects

Projects: `fw_update_application` and `fw_update_bootloader` (CCS 12.8.0, compiler `ti-cgt-c2000_22.6.3.LTS`, device `TMS320C28XX.TMS320F280049C`,
SysConfig 1.27.0, C2000Ware 26.01.00.00; `.ccsproject`, `.cproject`). Both have four build configurations (`CPU1_RAM`, `CPU1_FLASH`,
`CPU1_LAUNCHXL_RAM`, `CPU1_LAUNCHXL_FLASH`). The tools and scripts in `flash_tools` / `image_tools` use the output folder `CPU1_FLASH`, so
`CPU1_FLASH` is the configuration documented here (command lines taken from the generated makefile of that configuration).

### 2.1 Sources that must be compiled

Linked-file names in the `.project` files map to this repository as follows (`<fwu_git_repo>/src/<module>/<file>` for the driver core).

| Source | Application | Bootloader |
|---|---|---|
| `src/activation/fwu_activate.c` | yes | yes |
| `src/boot/fwu_boot.c` | yes | yes |
| `src/counters/fwu_counters.c` | yes | yes |
| `src/crypto/fwu_crypto.c`, `fwu_crypto_sw.c`, `fwu_sha256.c` | yes | yes |
| `src/flash/fwu_flash.c` | yes | yes |
| `src/journal/fwu_journal.c`, `fwu_journal_ab.c` | yes | yes |
| `src/manifest/fwu_manifest.c` | yes | yes |
| `src/verify/fwu_verify.c` | yes | yes |
| `src/frame/fwu_decode.c`, `fwu_frame.c`, `fwu_parser.c` | yes | no |
| `src/ring_buffer/fwu_ringbuf.c` | yes | no |
| `src/session/fwu_proto.c`, `fwu_session.c` | yes | no |
| `src/transport/fwu_link.c` | yes | no |
| `src/pull/fwu_pull.c`, `fwu_http.c` | yes | no |
| `ports/c2000/f28004x/flash_port.c` (+ `flash_port.h`) | yes | yes |
| `ports/c2000/f28004x/fwu_flash_adapter.c` (+ `.h`) | yes | yes |
| example `main.c` (CCS name `empty_driverlib_main.c`) | `application/main.c` | `bootloader/main.c` |
| example port sources | `application/port/app_ble.c`, `app_fwu.c`, `app_pull.c`, `app_sci.c`, `app_tick.c` | none |
| FC41D driver (NOT in this repo) | `FC41D_api.c`, `FC41D_bridge.c`, `SRC/F2800157_port.c`, `SRC/FC41D_ble.c`, `SRC/FC41D_config.c`, `SRC/FC41D_core.c`, `SRC/FC41D_parser.c`, `SRC/FC41D_tcp.c`, `SRC/FC41D_wi-fi.c` | none |
| TI template files in the project's `device/` folder (NOT in this repo) | `device.c`, `f28004x_codestartbranch.asm` | same |
| SysConfig-generated | `syscfg/board.c`, `syscfg/c2000ware_libraries.c` (from `c2000.syscfg`) | same |
| Linker command file | `application/28004x_app_flash_lnk.cmd` | `bootloader/28004x_bootloader_flash_lnk.cmd` |

Not compiled in either project (present in `src/` but absent from both `.project` link lists and both maps): `fwu_auth.c`, `fwu_client.c`,
`fwu_crypto_sim.c`, `fwu_flash_sim.c`, `fwu_isotp.c`, `fwu_linksim.c`. The application map contains no `fwu_auth` symbol (checked).

`app_sci.c` is compiled in the application project (`CPU1_FLASH/ccsObjs.opt`) but contributes nothing to the linked image in the default
configuration: no `app_sci` entry appears in the map (the function is unreferenced because `APP_FWU_OVER_BLE = 1`).

Libraries (linker): `driverlib` (the map shows objects `cputimer`, `flash`, `gpio`, `interrupt`, `sci`, `sysctl` from `driverlib_eabi.lib`; the application also uses `cputimer`, `sci`; the bootloader map has `flash`, `gpio`, `interrupt`, `sysctl`, no `sci`/`cputimer`),
`F021_API_F28004x_FPU32_EABI.lib` (named in both `.cmd` files; search path `<C2000Ware>/libraries/flash_api/f28004x/lib`; objects `Async`, `BlankCheck`, `FlashStateMachine`, `Init`, `Program`, `Read`, `Utilities`, `Verify`),
`libc.a` (`rts2800_fpu32_eabi.lib`), `c2000ware_libraries.cmd.genlibs`. The project links `driverlib.lib` from `<C2000Ware>/driverlib/f28004x/driverlib/ccs/Debug/`; the map names the archive `driverlib_eabi.lib` (same folder contains both names; which file is the one actually opened was not checked).

### 2.2 Compiler options (generated makefile, `CPU1_FLASH`)

Common: `-v28 -ml -mt --cla_support=cla2 --float_support=fpu32 --tmu_support=tmu0 --vcu_support=vcu0 --c99 --abi=eabi --gen_func_subsections=on --diag_suppress=10063 --diag_warning=225 --diag_wrap=off --display_error_number`. Application adds `--advice:performance=all`.

| Setting | Application | Bootloader |
|---|---|---|
| Optimisation | `-Ooff` (`--opt_level` off) | `-O2` (`.cproject`; generated makefile shows `-O2`) |
| Predefined symbols (`--define`) | `ver_1`, `C2000`, `FWU_BOARD_F280049C`, `FWU_PROFILE_DEV`, `DEBUG`, `_FLASH` | `C2000`, `FWU_BOARD_F280049C`, `FWU_PROFILE_DEV`, `DEBUG`, `_FLASH` |
| SysConfig / C2000Ware symbol macros | expand to nothing in the generated command line | same |

Meaning of the symbols (from the code): `C2000` makes `fwu_byte_t` a 16-bit container (`src/fwu_types.h:34-36`); `FWU_BOARD_F280049C` selects the layout header (`src/config/fwu_layout.h:32-40`, exactly one board must be defined); `FWU_PROFILE_DEV` selects the DEV profile (`src/config/fwu_config.h:61-77`: verification CRC-32, which `fwu_boot_policy_from_profile()` maps to `FWU_BOOT_VERIFY_HASH`, activation COPY unless `FWU_ACTIVATION_SWAP_ROLLBACK` is defined, no anti-rollback; cannot be combined with `FWU_PRODUCTION`); `ver_1` only affects `application/main.c:68-74` and `application/port/app_fwu_config.h:15-19` (LED and reported version, see 3.2); `_FLASH` and `DEBUG` are TI template symbols (their use in sources outside this repo is UNVERIFIED). Neither project defines `FWU_PRODUCTION`, `FWU_BOARD_NUMBERS_VERIFIED` or `FWU_ACTIVATION_SWAP_ROLLBACK`.

The `CPU1_LAUNCHXL_FLASH` configurations use `-Ooff` in both projects and do not define `ver_1` (checked in `.cproject`); their other settings were not compared (UNVERIFIED).

### 2.3 Include paths (`CPU1_FLASH`; absolute machine paths replaced by placeholders)

Application (in this order, after the SysConfig include path):

1. `<fc41d_parent_dir>`
2. `<fwu_git_repo>/src` (the original `fwu_driver` root, contains `fwu_types.h`)
3. `<fwu_git_repo>/src/{flash,error,bytes,crc,ring_buffer,frame,journal,activation,manifest,crypto,verify,counters,boot,config,session,transport,pull}`
4. `<application>/port` (original name `fwu_port`: `app_*.h`, `app_fwu_config.h`, `fwu_flash_adapter.h`)
5. `<C2000Ware>/libraries/flash_api/f28004x/include/FlashAPI`
6. `<application>/flash_port` (`flash_port.h`, `flash_port_config.h`)
7. C2000Ware include path macro, `<application>` (project root), `<C2000Ware>`, `<application>/device`, `<C2000Ware>/driverlib/f28004x/driverlib`, `<ti_cgt>/include`

Bootloader: same, but without `<fc41d_parent_dir>` and without `ring_buffer`, `frame`, `session`, `transport`, `pull` (so `src` plus `{flash,error,bytes,crc,journal,activation,manifest,crypto,verify,counters,boot,config}`); `<bootloader>/port` (original `fwu_port`: `boot_fwu_config.h`, `fwu_flash_adapter.h`) and `<bootloader>/flash_port`.

In the repository `flash_port.c/.h` and `fwu_flash_adapter.c/.h` live in `<fwu_git_repo>/ports/c2000/f28004x`, while the original projects had them in the project's `flash_port/` and `fwu_port/` folders; a new project file must add `<fwu_git_repo>/ports/c2000/f28004x` and the project's own `flash_port` folder (for `flash_port_config.h`) to the include path. The project-specific `flash_port_config.h` must be found before any other copy.

### 2.4 FC41D include style

The pristine copies of `main.c`, `app_ble.c`, `app_pull.c` include `"FC41D_api.h"` (and `"FC41D_bridge.h"` in `main.c`); the original CCS sources include `"<product_dir>/FC41D/FC41D_api.h"` (found through include path 1 above). The FC41D driver's own `SRC/F2800157_port.c` includes `"<product_dir>/FC41D/INC/F2800157_port.h"` (line 15). Consequently the build needs BOTH `<fc41d_parent_dir>` (for the driver's own sources) and `<fc41d_driver_root>` (for the example's short include form). How the original `main.c` found `FC41D_api.h` through the include list above could not be determined from `.cproject` (the `.d` file shows it resolved to `<fc41d_driver_root>/FC41D_api.h`): UNVERIFIED.

The original CCS `main.c` also contains real Wi-Fi credentials in the `fc41d_settings` initializers (`empty_driverlib_main.c` lines 105 and 115); the repository copy has placeholders (`application/main.c:105,115`). Do not copy the original file.

### 2.5 Linker options (`CPU1_FLASH`)

`-z -m"<project>.map" --heap_size=0x200 --stack_size=0x3F8 --warn_sections --reread_libs --define=_FLASH --diag_wrap=off --display_error_number --xml_link_info="<project>_linkInfo.xml" --entry_point=code_start --rom_model -o "<project>.out"`
with `-i` search paths `<C2000Ware>/libraries/flash_api/f28004x/lib`, `<ti_cgt>/lib`, `<C2000Ware>`, `<build>/syscfg`, `<ti_cgt>/include`
(application makefile; the bootloader's `.cproject` lists identical values: heap 0x200, stack 0x3F8, entry `code_start`). Linker command file actually used: the project-root file
`28004x_app_flash_lnk.cmd` / `28004x_bootloader_flash_lnk.cmd` (the TI `28004x_generic_*_lnk.cmd` files are excluded from the build in `sourceEntries`; the
`LINKER_COMMAND_FILE=28004x_generic_flash_lnk.cmd` tag in OPT_TAGS is only a project property). The map files confirm `ENTRY POINT SYMBOL: "code_start"` at 0x083000 (application) and 0x080000 (bootloader).
Entry-point mechanism (`codestart` branch in `f28004x_codestartbranch.asm` jumping to `code_start`) is from the TI template and was not read: UNVERIFIED.
No `.sysmem` section appears in either map, i.e. no dynamic allocation is linked even though `--heap_size=0x200` is given.

### 2.6 SysConfig (`c2000.syscfg`, application)

Header (`c2000.syscfg:4-6`): device `F28004x`, part/package `F28004x_100PZ` (`TMS320F280049C`, `100PZ`), `C2000WARE@26.01.00.00`, SysConfig tool `1.27.0+4565`. The bootloader file has the same header.

| Instance | Application | Bootloader |
|---|---|---|
| `LED4` | GPIO23_VSW, output (lines 22-24) | same |
| `LED5` | GPIO34, output (lines 26-28) | same |
| `FW_SCI` | SCIB, RX GPIO13, TX GPIO40, TX/RX FIFO level `SCI_FIFO_TX1`/`SCI_FIFO_RX1`, FIFO interrupts RXFF+TXFF enabled, error interrupts FE/OE/PE/RXERR, registered interrupt: RX (lines 30-40) | not present |
| `FC41D_SCI` | SCIA (suggested solution), RX GPIO28, TX GPIO29, same FIFO settings (lines 42-58) | not present |

Baud rate and frame format are not set in the file (SysConfig defaults apply); the source comment says "115200 8N1 is set up in Board_init" (`application/main.c:474`): the default value itself is UNVERIFIED.

---

## 3. Initialisation and main-loop order

### 3.1 `application/main.c` `main()` (lines 435-541)

| Step | Call | Line | Condition |
|---|---|---|---|
| 1 | `Device_init()` | 441 | |
| 2 | `Device_initGPIO()` | 446 | |
| 3 | `Interrupt_initModule()` | 451 | |
| 4 | `Interrupt_initVectorTable()` | 457 | |
| 5 | `Board_init()` | 462 | SysConfig-generated (GPIO, SCIA/SCIB, ISR registration) |
| 6 | `C2000Ware_libraries_init()` | 467 | |
| 7 | `app_tick_init()` | 472 | CPU Timer 0 1 ms tick (see below) |
| 8 | `fwu_init_status = app_fwu_init()` | 473 | |
| 9 | `fc41d_api_initialize(&g_fc41d_api, FC41D_UART_BASE, &fc41d_settings)` | 474 | |
| 10 | `app_ble_init(app_tick_ms())` | 476 | only if `APP_FWU_OVER_BLE` |
| 11 | `EINT; ERTM;` | 482-483 | global interrupts enabled here |
| 12 | `GPIO_writePin(LED4, 1); GPIO_writePin(LED5, 1);` | 485-486 | |
| 13 | `Interrupt_enable(INT_FW_SCI_RX)` | 489 | only if `!FC41D_ON_FW_UART` |
| 14 | `fw_rx.tick/head/tail = 0` | 492-494 | |
| 15 | `led_last = app_tick_ms()`; `fc41d_tick_last = led_last` | 507, 509 | the latter only if `!APP_FWU_OVER_BLE` |

The self-tests `flash_selftest()` / `adapter_selftest()` are defined (lines 180, 341) but their call sites are commented out (lines 496-503).

Main loop (`while(1)`, line 512), in order:

1. `now = app_tick_ms()` (514)
2. `app_fwu_poll(now)` (516)
3. If `APP_FWU_OVER_BLE` (518): `app_ble_poll(now)` (519); then, if `APP_FWU_PULL`, `app_pull_poll(now)` (521).
4. Else (`!APP_FWU_OVER_BLE`, lines 524-533): `fc41d_api_1ms_timer()` once per elapsed ms (524-527), `fc41d_api_polling()` (528), then `fc41d_ble_echo()` (530) if `FC41D_BLE_ECHO_TEST`, else `fc41d_bridge_run()` (532).
5. LED toggle every `LED_BLINK_HALF_PERIOD_MS` (536-539): `GPIO_togglePin(APP_BLINK_LED)`.

Interrupt routines defined in `main.c` / `app_tick.c`: `INT_FW_SCI_RX_ISR` (`main.c:544`; pushes bytes with `app_fwu_rx_push`; NOT enabled when `FC41D_ON_FW_UART = 1`, see 3.3), `INT_FC41D_SCI_RX_ISR` (`main.c:567`; only counts and clears flags; its enable call is absent, comment at line 569), `app_tick_isr` (`app_tick.c:12`, registered for `INT_TIMER0` at `app_tick.c:20`, increments `g_tick_ms`, acknowledges PIE group 1).
`app_tick_init()` (`app_tick.c:17-30`): register ISR, stop timer, `CPUTimer_setPeriod(DEVICE_SYSCLK_FREQ / 1000 - 1)`, prescaler 0, reload, run-free emulation mode, enable interrupt, `Interrupt_enable(INT_TIMER0)`, start. The value of `DEVICE_SYSCLK_FREQ` (the comment says 100 MHz) is defined in the TI `device.h`, not in this repo: UNVERIFIED.

### 3.2 `app_fwu_init()` (`application/port/app_fwu.c:56-116`), in order

1. `fwu_flash_adapter_init(&g_flash, APP_FWU_FLASH_BASE_WORD, APP_FWU_FLASH_LENGTH_OCTETS)` (61)
2. `fwu_crypto_sw_init(&g_crypto_sw, &g_crypto)` (63)
3. Fill `g_boot_config` (66-80: policy from profile, layout from board, `keys = NULL`, `key_count = 0`, product id, `max_trial_boots`, work buffers, `app_valid = NULL`, `watchdog_arm = NULL`), then `fwu_boot_init(&g_boot, &g_boot_config)` (81)
4. `app_rx_init(&g_rx)` (84)
5. `fwu_parser_init(&g_parser, &g_rx.base)` (87)
6. `app_fwu_session_setup()` (90): `fwu_boot_activate()`, `fwu_boot_verify_config()` (42-43), `auth = NULL`, `require_auth = false`, `auth_max_failures = 3`, `auth_lockout_ms = 60000`, then `fwu_session_init(&g_session, &config)` (53)
7. Transport selection: `g_transport.send = app_ble_send` if `APP_FWU_OVER_BLE`, else `app_sci_send` (95-99); `poll = NULL`, `context = NULL`; `fwu_link_init(&g_link, &link_config)` (105)
8. If `APP_FWU_PULL`: `app_pull_init(&g_session, &g_link)` (109), result ignored (stored in `g_app_pull_init_status`)
9. `g_status`/`g_ready` set (113-114)

`app_pull_init()` (`app_pull.c:115-139`): `fwu_pull_init(&g_pull, &config)` (132), then `fwu_link_set_extension(link, pull_handles, pull_on_frame, NULL)` (134). It does not call the FC41D driver, so running before `fc41d_api_initialize` (main.c:474) is safe; the credit window is set up lazily in `app_pull_poll` (app_pull.c:192-195).

`app_fwu_poll(now)` (`app_fwu.c:143-164`): if ready: `fwu_link_poll()` (149), then if `fwu_link_reboot_ready()` (150) call `app_fwu_reboot()` (151); if not ready: drain `g_rx` in 16-element steps (156-163).
`app_fwu_reboot()` (`app_fwu.c:130-141`): `app_ble_flush(APP_BLE_FLUSH_TIMEOUT_MS, APP_BLE_FLUSH_SETTLE_MS)` (132, BLE only); busy-wait until the `FW_SCI_BASE` TX FIFO is empty and the transmitter idle (134-135, no timeout); `DEVICE_DELAY_US(20000)` (136); `DINT` (137); `SysCtl_resetDevice()` (138); endless loop (139-140).

`app_ble_init()` (`app_ble.c:54-78`): zero ring indices, set `g_tx_last_ms = now - APP_BLE_TX_GAP_MS`, clear decoder state, set `g_tick_last = now_ms`, clear `g_app_ble_stats`.
`app_ble_poll()` (`app_ble.c:276-285`), in order: 1 ms tick loop calling `fc41d_api_1ms_timer` (277-280); `fc41d_api_polling` (281); `rx_service` (282); `rx_stage_service` (283); `tx_service` (284). `app_ble_flush()` (`app_ble.c:293-318`) loops `app_ble_poll` until `app_ble_tx_idle()` has held for `settle_ms` or `timeout_ms` expires (blocking).
`app_pull_poll()` (`app_pull.c:184-234`), in order: return if not ready (187); enable the credit window if `core.net.tcp_window_on` is false (192-195); connection tracking via `tcp_is_up()` and `fwu_pull_set_link()` (198-210); `grant_credit()` (213); then if `fwu_pull_input_used() > 0`: `fc41d_api_tcp_read_hold(true)`, return if `fc41d_api_busy()`, else `fwu_pull_poll()` and release the hold when the ring is empty (218-229); otherwise release the hold and `fwu_pull_poll()` (231-232).
Receive path: `rx_service` (`app_ble.c:169-210`) takes up to `APP_BLE_RX_ROUNDS` messages from `fc41d_get_message`; BLE messages are hex-decoded into the staging buffer, `FC41D_MESSAGE_TCP_CLIENT` messages go to `app_pull_on_tcp()` when `APP_FWU_PULL` (181-186), anything else increments `rx_other`.

### 3.3 Compile-time switches

| Switch | Defined in | Value | Meaning / effect |
|---|---|---|---|
| `APP_FWU_OVER_BLE` | `app_fwu_config.h:32` | 1 | 1: firmware-update frames travel over FC41D BLE GATT characteristic FFF1 as hex text (`app_ble.c`); main loop calls `app_ble_poll` which also runs the FC41D tick and polling. 0: frames travel over the FW UART (`app_sci_send`; RX bytes pushed by `INT_FW_SCI_RX_ISR`); main loop then runs the FC41D tick/polling itself (`main.c:524-533`) |
| `APP_FWU_PULL` | `app_fwu_config.h:38` | 1 | 1: device pulls the image over TCP/HTTP (Range requests) through the FC41D Wi-Fi STA + TCP client; control frames stay on BLE. Selects `fc41d_settings.type = FC41D_BLE_GATT_WITH_WIFI_STA_AND_TCP_CLIENT` (`main.c:90-108`) instead of `FC41D_BLE_GATT_ONLY` (`main.c:109-119`). `#error` if `APP_FWU_OVER_BLE` is 0 (`app_fwu_config.h:45-47`) |
| `APP_PULL_RANGE_BYTES` | `app_fwu_config.h:39` | `APP_FWU_CHUNK_SIZE * 16u` = 4096 | bytes requested by one HTTP GET; also the length of `g_chunk` (`app_pull.c:21`, section `ramgs2`) and `config.range_bytes`/`chunk_buffer_length` (`app_pull.c:128-129`) |
| `APP_PULL_RESPONSE_TIMEOUT_MS` / `APP_PULL_MAX_RETRIES` | `app_fwu_config.h:40-41` | 8000 / 5 | request renewed after 8 s without bytes; pull fails after 5 retries of a range |
| `FC41D_ON_FW_UART` | `main.c:79` | 1 | 1: the FC41D is attached to the FW UART (`FW_SCI_BASE`, SCIB GPIO13/40); `Interrupt_enable(INT_FW_SCI_RX)` is skipped (`main.c:488-490`), and (per the comment at lines 76-78) UART firmware update cannot be used in this mode. 0: FC41D on `FC41D_SCI_BASE` (SCIA GPIO28/29) and UART update possible |
| `FC41D_BLE_ECHO_TEST` | `main.c:127` | 1 | only meaningful when `APP_FWU_OVER_BLE = 0` (code guarded at `main.c:129`, used at 529): 1 runs `fc41d_ble_echo`, 0 runs `fc41d_bridge_run` |
| `ver_1` | project `-D` (application only) | defined | `main.c:68-74`: defined -> `APP_BLINK_LED = LED4`, 500 ms half period; undefined -> `LED5`, 300 ms. `app_fwu_config.h:15-19`: `APP_FWU_RUNNING_VERSION` = 1 (defined) or 2 (undefined) |
| `APP_FWU_PRODUCT_ID` / `BOOT_FWU_PRODUCT_ID` | `app_fwu_config.h:14`, `boot_fwu_config.h:9` | 2001 | must be equal in both projects (contract fingerprint, `fwu_boot.h`) |
| `APP_FWU_MAX_TRIAL_BOOTS` / `BOOT_FWU_MAX_TRIAL_BOOTS` | `app_fwu_config.h:23`, `boot_fwu_config.h:10` | 3 | used only in swap mode; equal in both projects |
| `APP_FWU_CHUNK_SIZE` | `app_fwu_config.h:20` | 256 | chunk size and `g_session_work` length |
| `APP_FWU_IDLE_TIMEOUT_MS` | `app_fwu_config.h:21` | 30000 | session drops after this idle time |
| `APP_FWU_WORK_SIZE` / `BOOT_FWU_WORK_SIZE` | `app_fwu_config.h:22`, `boot_fwu_config.h:11` | 256 | verify/activate read-back buffers |
| `APP_FWU_RX_RING_SIZE` | `app_fwu_config.h:24` | 1024 | receive ring (`FWU_RINGBUF_DEFINE(app_rx, ...)`, `app_fwu.c:18`) |
| `APP_FWU_FLASH_BASE_WORD`, `APP_FWU_FLASH_LENGTH_OCTETS` | `app_fwu_config.h:27-28` | 0x080000, 32 * 8192 | flash object covers all of flash (same for `BOOT_FWU_*`) |
| `APP_BLE_TX_RING_SIZE` | `app_fwu_config.h:50` | 640 | device -> PC ring |
| `APP_BLE_TX_GAP_MS` | `app_fwu_config.h:51` | 150 | minimum gap between hex pieces (comment: 100 ms lossless, 50 ms lossy in measurement) |
| `APP_BLE_RX_STAGE_SIZE` | `app_fwu_config.h:52` | 576 | PC -> device staging buffer |
| `APP_BLE_RX_IDLE_MS` | `app_fwu_config.h:53` | 250 | staging buffer is flushed to the parser after this silence |
| `APP_BLE_RX_ROUNDS` | `app_fwu_config.h:54` | 8 | max FC41D messages per `app_ble_poll` |
| `APP_BLE_FLUSH_TIMEOUT_MS` / `APP_BLE_FLUSH_SETTLE_MS` | `app_fwu_config.h:55-56` | 3000 / 300 | reboot flush limits |

Combinations the CODE explicitly handles:

| `APP_FWU_OVER_BLE` | `APP_FWU_PULL` | `FC41D_ON_FW_UART` | Status in code |
|---|---|---|---|
| 1 | 1 | 1 | shipped defaults (`app_fwu_config.h`, `main.c:79`); the only combination whose map/image artifacts exist |
| 1 | 0 | any | code paths exist (`main.c:109-119`, `app_ble.c:8,181`); build not verified |
| 0 | 0 | 0 | UART transport with FC41D on SCIA; code paths exist (`main.c:121-165,508-533`); build not verified |
| 0 | 0 | 1 | compiles by the preprocessor rules but the comment at `main.c:76-78` says UART update is unusable (no `#error` enforces it) |
| 0 | 1 | any | rejected by `#error` (`app_fwu_config.h:45-47`) |

Whether combinations other than the shipped defaults build and run was not tested here (UNVERIFIED). With `FC41D_ON_FW_UART = 0` and `APP_FWU_OVER_BLE = 1` both transports would feed `app_fwu_rx_push` (`main.c:557`, `app_ble.c:110`) into the same ring; nothing in the code prevents this.

---

## 4. RAM resources allocated by the example

Sizes in 16-bit words. "Source" = derived from the declaration; "Map" = from the maintainer's local `.map` files (UNVERIFIED against a rebuild; dates in 1.1). Struct sizes of driver types are not in the sources and are only available through the map.

### 4.1 Application

| Object | File:line | Size | Section / block |
|---|---|---|---|
| `g_chunk[APP_PULL_RANGE_BYTES]` | `app_pull.c:20-21` | 4096 (source; map 0x1000) | `ramgs2` -> `RAMGS2` 0x010000..0x010FFF |
| FC41D driver object `g_fc41d_api` | driver (`FC41D_api.c`) | map 0x774 = 1908 | `debug_data` -> `RAMGS1` 0xE000.. |
| `app_fwu.c` statics: `g_rx` ring (storage 1024), `g_parser`, `g_link`, `g_session`, `g_transport`, `g_flash`, `g_crypto_sw`, `g_crypto`, `g_boot`, `g_boot_config` | `app_fwu.c:18-29` | map: `app_fwu.obj .bss` = 0xBF4 = 3060 (the three work arrays below are listed separately in the map and are not part of this figure) | `.bss` -> `RAMGS0` |
| `g_verify_work`, `g_activate_work`, `g_session_work` | `app_fwu.c:30-32` | 256 each (source; map 0x100 each) | `.bss` -> `RAMGS0` |
| `app_pull.c` statics (incl. `g_pull` and stats) | `app_pull.c:13-27` | map: `app_pull.obj .bss` = 0x560 = 1376 (excludes `g_request` and `g_feed`, listed separately) | `.bss` -> `RAMGS0` |
| `g_request[FWU_HTTP_REQUEST_MAX]` | `app_pull.c:23` | map 0x100 = 256 | `.bss` |
| `g_feed[FC41D_MESSAGE_MAX]` | `app_pull.c:22` | 96 (`FC41D_MESSAGE_MAX` = `FC41D_LINE_MAX` = 96 in the maintainer's driver copy; map 0x60) | `.bss` |
| `g_tx_ring[APP_BLE_TX_RING_SIZE]` | `app_ble.c:20` | 640 | `.bss` |
| `g_rx_stage[APP_BLE_RX_STAGE_SIZE]` | `app_ble.c:28` | 576 | `.bss` |
| `g_rx_text[FC41D_MESSAGE_MAX]` | `app_ble.c:33` | 96 | `.bss` |
| `g_hex_out[APP_BLE_PIECE_HEX + 1]` | `app_ble.c:25` | 95 (47*2+1) | `.bss` |
| other `app_ble.c` scalars + `g_app_ble_stats` (13 `uint32_t`) | `app_ble.c:17-39` | map 0x28 = 40 | `.bss` |
| `g_tick_ms` | `app_tick.c:8` | 2 (map: `app_tick.obj .bss`) | `.bss` |
| `main.c` statics: `fw_rx` (`rx_ring_buff_t`: `buf[256]` + 4 x `uint16_t` + `uint32_t`) and others | `main.c:275-284` | `fw_rx` = 262 (source); map: `main` `.bss` = 0x10A = 266 | `.bss` |
| `fc41d_echo_in/out`, `g_fc41d_echo_*` | `main.c:129-134` | only compiled if `!APP_FWU_OVER_BLE` (96 + 104 words) | not allocated in the default build |
| `.data` (all objects) | map | 0x16 = 22 | `RAMLS5` 0xA800.. |
| `.stack` | linker option | 0x3F8 = 1016 | `RAMM1` 0x400..0x7F7 (full) |
| `.TI.ramfunc` run image | map | 0xB5C = 2908 | `RAMLS03` 0x8000..0x8B5B |

`RAMGS0` totals (map): 0x1CA8 used of 0x2000. `RAMGS1`: 0x774 used. `RAMGS2`: 0x1000 used. `rx_data[RX_BUF_SIZE]` (`main.c:285`) and the `sci_fw_ring_buffer_*` helpers (`main.c:287-329`) are defined but not referenced anywhere else in `main.c`.

### 4.2 Bootloader

| Object | File:line | Size | Section / block |
|---|---|---|---|
| `g_flash`, `g_crypto_sw`, `g_crypto`, `g_boot`, `g_boot_config`, `g_boot_result`, `g_boot_setup_status` | `bootloader/main.c:67-75` | map: `main .bss` = 0x1BC = 444 (together) | `.bss` -> `RAMLS5` |
| `g_verify_work[BOOT_FWU_WORK_SIZE]`, `g_activate_work[BOOT_FWU_WORK_SIZE]` | `bootloader/main.c:72-73` | 256 each | `.bss` -> `RAMLS5` |
| `.bss` total / `.data` total | map | 0x3BC / 0xC | `RAMLS5` 0xA800.. (0x3C8 used of 0x800) |
| `.stack` | linker option | 0x3F8 | `RAMM1` |
| `.TI.ramfunc` run image | map | 0xB2C = 2860 | `RAMLS03` |

---

## 5. Bootloader flow (`bootloader/main.c`)

1. `main()` (136-189): `Device_init` (142), `Device_initGPIO` (147), `Interrupt_initModule` (152), `Interrupt_initVectorTable` (158), `Board_init` (163), `C2000Ware_libraries_init` (168), `EINT; ERTM;` (173-174), `LED4`/`LED5` set to 1 (176-177), `DEVICE_DELAY_US(100000)` (178).
2. `boot_update_and_decide()` (89-118), called at line 180:
   1. `fwu_flash_adapter_init(&g_flash, BOOT_FWU_FLASH_BASE_WORD, BOOT_FWU_FLASH_LENGTH_OCTETS)` (92), then `fwu_crypto_sw_init` (94); any failure sets `g_boot_setup_status` and returns `FWU_BOOT_STAY` (96-99).
   2. Fill `g_boot_config` (101-115): `policy = fwu_boot_policy_from_profile()` (DEV: hash verification, COPY mode, no anti-rollback), `layout = fwu_boot_layout_from_board()`, `keys = NULL`, `key_count = 0`, `product_id = BOOT_FWU_PRODUCT_ID`, `max_trial_boots`, work buffers (256 words each), `app_valid = app_valid_hook`, `watchdog_arm = NULL`.
   3. `fwu_boot_start(&g_boot, &g_boot_config, &g_boot_result)` (117). Documented order (`src/boot/fwu_boot.h:17-30`): validate configuration/policy; read counters if anti-rollback; build verification and activation configuration; `fwu_activate_run` (verify, copy/swap, trial, rollback pending work); call `app_valid`; arm watchdog if trial. Any failure in the first five steps gives `FWU_BOOT_STAY` (reasons `FWU_BOOT_REASON_CONFIG`, `COUNTERS`, `ACTIVATE_INIT`, `ACTIVATE_RUN`, `NO_APP`; `fwu_boot.h:137-149`).
   4. `app_valid_hook` -> `app_is_valid()` (77-85): true when the first word at `APP_ENTRY_ADDRESS` (0x083000) is not 0xFFFF.
3. If the decision is `FWU_BOOT_JUMP_APP` (180): `app_jump()` (121-126): `DINT`, `IER = 0`, `IFR = 0`, call the function at 0x083000 (`APP_ENTRY_ADDRESS`, line 63; equals `APP_BEGIN` in the application linker file).
4. Otherwise `while(1) { bootloader_fail_animation(); }` (184-187): `DEVICE_DELAY_US(30000)` then `GPIO_togglePin(LED5)` (128-131). There is no recovery path or watchdog in the example (only the LED).

The application, not the bootloader, writes the commit record: `app_fwu.c:36-37` (session uses the same `fwu_boot` activation configuration); the bootloader consumes it at the next boot (the application resets after `fwu_link_reboot_ready()`, 3.2).
The bootloader image's `BEGIN` at 0x080000 is the boot-to-flash entry (comment `28004x_bootloader_flash_lnk.cmd:5`); the boot-mode/OTP configuration that makes the ROM jump there is not part of this repository: UNVERIFIED.

---

## 6. Dependencies that are not in this repository

| Dependency | Evidence |
|---|---|
| TI C2000Ware **26.01.00.00** (`C2000WARE@26.01.00.00`), SysConfig **1.27.0+4565**, CCS 12.8.0, TI C2000 CGT 22.6.3.LTS | `c2000.syscfg:4-6` (both projects), `.ccsproject`, makefiles |
| `layout_f280049c.h:6` cites the TI linker file in C2000Ware "6.00.01.00" for the flash numbers | header comment; differs from the version string above; which is correct is UNVERIFIED |
| TI sources/headers: `driverlib.h`, `device.h`, `board.h`/`board.c` (generated), `c2000ware_libraries.h/.c` (generated), `device.c`, `f28004x_codestartbranch.asm`, F021 Flash API library and `<C2000Ware>/libraries/flash_api/f28004x/include/FlashAPI` | includes in `main.c:50-56`; makefiles |
| Used driverlib/device symbols (partial, from the example): `Device_init`, `Device_initGPIO`, `Interrupt_initModule`, `Interrupt_initVectorTable`, `Interrupt_register/enable/clearACKGroup`, `Board_init`, `C2000Ware_libraries_init`, `CPUTimer_*`, `SCI_*`, `GPIO_*`, `SysCtl_resetDevice`, `DEVICE_DELAY_US`, `DEVICE_SYSCLK_FREQ`, `LED4`, `LED5`, `FW_SCI_BASE`, `FC41D_SCI_BASE`, `INT_FW_SCI_RX`, `INT_FW_SCI_RX_INTERRUPT_ACK_GROUP`, `INT_FC41D_SCI_RX_INTERRUPT_ACK_GROUP` | `main.c`, `app_tick.c`, `app_fwu.c` |
| FC41D module driver (Quectel FC41D Wi-Fi/BLE module), **not published here**; the maintainer's copy: `FC41D_api.h` revisions R1-R5 (2026-09-29..2026-10-05), README titled for TMS320F2800157 | driver headers |
| The example's own headers `app_*.h`, `app_fwu_config.h`, `boot_fwu_config.h` ARE in the repository | `application/port/`, `bootloader/port/` |

FC41D API surface used by the example (function, call sites):

| Function / macro | Call sites |
|---|---|
| `fc41d_api_initialize(fc41d_api_t *, uint32_t sci_base, const fc41d_api_settings_t *)` | `main.c:474` (comment `app_pull.c:191` relies on it resetting the instance) |
| `fc41d_api_1ms_timer()` | `main.c:525`, `app_ble.c:278` |
| `fc41d_api_polling()` | `main.c:528`, `app_ble.c:281` |
| `fc41d_get_message(self, char *buffer, uint16_t *length)` returning `fc41d_message_source_e` (`FC41D_MESSAGE_NONE`, `FC41D_MESSAGE_BLE`, `FC41D_MESSAGE_TCP_CLIENT`) | `main.c:140`, `app_ble.c:175` |
| `fc41d_api_ble_send(self, const char *, uint16_t)` returning sent length | `main.c:158`, `app_ble.c:252` |
| `fc41d_api_tcp_send(self, socket_id, const char *, uint16_t)` | `app_pull.c:66` |
| `fc41d_api_enable_features(self, features)` with `FEAT_WIFI_STA | FEAT_TCP_CLIENT` | `app_pull.c:93` |
| `fc41d_customize_tcp_settings(self, const uint8_t ip[4], uint16_t port)` returning `fc41d_api_return_e` (`FC41D_API_OK`) | `app_pull.c:94` |
| `fc41d_api_tcp_read_window_enable(self, bool)` | `app_pull.c:193, 206` |
| `fc41d_api_tcp_read_credit(self, uint16_t)` | `app_pull.c:179` |
| `fc41d_api_tcp_read_hold(self, bool)` | `app_pull.c:219, 227, 231` |
| `fc41d_api_busy(const self)` | `app_pull.c:220` |
| `fc41d_queue_space(&g_fc41d_api.core.transmit_queue)`, `fc41d_queue_count(...)` (macros for `queue_available_space_char_256`, `queue_count_char_256`) | `app_ble.c:235, 289`, `app_pull.c:59` |
| `fc41d_bridge_run(self)` (from `FC41D_bridge.h`) | `main.c:532` (only when `!APP_FWU_OVER_BLE && !FC41D_BLE_ECHO_TEST`) |
| Types/objects: `fc41d_api_t g_fc41d_api`, `fc41d_api_settings_t` (fields `.type`, `.setup.config.{is_dhcp_enabled,tcp_hex_send}`, `.setup.ble_gatt_config.{device_name,service_uuid,characteristic_uuid}`, `.setup.wi_fi_sta_config.{ssid,password}`, `.setup.wi_fi_ap_config.{ssid,password}`, `.setup.tcp_client_config.{ip_address[4],port}`, `.setup.tcp_server_config.port`), enumerators `FC41D_BLE_GATT_ONLY`, `FC41D_BLE_GATT_WITH_WIFI_STA_AND_TCP_CLIENT` | `main.c:100-118` |
| Macros: `FC41D_MESSAGE_MAX` (96 in the maintainer's copy), `FC41D_TX_CHUNK` (64), `FC41D_TCP_CLIENT_SOCKET_ID` (0), `FEAT_WIFI_STA`, `FEAT_TCP_CLIENT` | `app_ble.c:33,234`, `app_pull.c:22,36,51,93` |
| Members of `g_fc41d_api.core` read by the example: `is_module_ready`, `communication_type.tcp_client`, `net.tcp_changed`, `net.tcp_open_mask`, `net.tcp_window_on`, `transmit_queue`, `tx_job.active` | `app_pull.c:31-37,83,192`, `app_ble.c:235,289-290` |

The driver's own port file `SRC/F2800157_port.c` includes `sci.h` (driverlib) and is compiled into the F280049C application; its compatibility with the F280049C beyond "it links and the image runs" is UNVERIFIED.

---

## 7. Image generation and flash helper scripts

All paths relative to `examples/c2000_launchxl_f280049c/application`.

### 7.1 `image_tools/make_image.bat`

Current line numbers: 1 `@echo off`; 2-7 `rem` comments; 8 `if not defined HEX2000 set "HEX2000=<default path>"`; 9 `cd /d "%~dp0"`; 10 delete old `app_test.bin`; 11 copy `..\CPU1_FLASH\fw_update_application.out` to `fw_update_application.out`; 12 run `"%HEX2000%" "app_bin.hexcfg"`; 13 delete the copied `.out`; 14 run `powershell -NoProfile -ExecutionPolicy Bypass -File "finalize_image.ps1"`; 15-18 on error level print a message and `exit /b 1`.
Default `HEX2000` is `C:\ti\ccs1281\ccs\tools\compiler\ti-cgt-c2000_22.6.3.LTS\bin\hex2000.exe` (the original hard-coded value; behaviour unchanged unless the environment variable is set).

### 7.2 `image_tools/app_bin.hexcfg`

Contents (12 lines): input `fw_update_application.out`; options `--binary --image --memwidth=16 --romwidth=16 --fill=0xFFFF -o app_test.bin`; `ROMS { APPIMG: origin = 0x106000, length = 0x20000 }`.
Address doubling: `0x106000 = 2 * 0x083000` (the application's word address, `APP_BEGIN`); the script comments state hex2000 counts byte addresses. The `--binary` output stores each 16-bit word as two octets, low octet first (`layout_f280049c.h:13`, `ports/c2000/f28004x/fwu_flash_adapter.c`). Evidence from the artifacts: the existing `app_test.bin` is 67936 octets = 2 x 33968 words, consistent with 0x84A9 linked words + granule rounding. The ROMS length (0x20000 octets = 131072) is deliberately larger than the capacity (73728) so that an oversized application is visible to the next step instead of being silently truncated by hex2000 (`finalize_image.ps1:3-6`). That hex2000 interprets `length` in octets here was not independently checked against TI documentation: UNVERIFIED (artifact sizes are consistent with it).

### 7.3 `image_tools/finalize_image.ps1`

`param($Name = "app_test.bin")`; `$capacity = 0x12000` (line 9) = 73728 octets = 9 sectors x 8192 = `FWU_LAYOUT_IMAGE_MAX_SIZE` for this board (`layout_f280049c.h:31-32`: staging 10 sectors minus 1 manifest sector; check: 0x12000 = 73728 = 9 x 8192 ok); `$granule = 16` (line 10) = the flash write granule (`fwu_flash_adapter.c:7`, `layout_f280049c.h:17`). Steps: fail if the file is missing (13); read bytes (14); trim trailing 0xFF (15-16); fail and delete the file if the image is empty (17); round the length up to a multiple of 16 (19); if the length exceeds the capacity, delete the file and exit 1 (20-25); write the trimmed file (26-28); print size, percentage and margin (29); `exit 0` (30). The two numbers (capacity 73728, granule 16) are hard-coded in the script and are NOT read from the layout header; they match it today.
Console messages of the script were translated to English in this change (behaviour otherwise identical).

### 7.4 `flash_tools/*.bat` (Texas Instruments DSLite command line)

| Script | Erase selection argument | What the script comment says | Output file flashed |
|---|---|---|---|
| `flash_app.bat` | `-s FlashEraseSelection=1` | writes the application, erases only the needed sectors, bootloader stays; bootloader must be flashed first; no CCS debug session open | `..\CPU1_FLASH\fw_update_application.out` |
| `flash_boot.bat` | `-s FlashEraseSelection=0` | writes the bootloader, ALL flash is erased (application lost) | `..\..\fw_update_bootloader\CPU1_FLASH\fw_update_bootloader.out` |
| `flash_boot_keep.bat` | `-s FlashEraseSelection=1` | writes the bootloader, erases only the bootloader sectors 0..2; application, staging and journal stay | `..\..\fw_update_bootloader\CPU1_FLASH\fw_update_bootloader.out` |

Common command: `%DSLITE% flash --config=%CCXML% -s FlashEraseSelection=<n> -f -v -r 1 -u -e %OUT%` with `CCXML = %~dp0..\targetConfigs\TMS320F280049C_LaunchPad.ccxml`. The meaning of `-f -v -r 1 -u -e` and of the `FlashEraseSelection` values is taken from the scripts' comments only; the DSLite documentation was not consulted: UNVERIFIED. Each script ends with `pause`.
The scripts read these environment variables when they are set: `DSLITE` (DSLite executable), `CCXML` (target configuration of YOUR debug probe; a `.ccxml` file is NOT part of this repository, create one in CCS) and `OUT` (the `.out` file to flash). The defaults assume a CCS project rooted in `application/` (`application/CPU1_FLASH/fw_update_application.out`) and one rooted in `bootloader/` (`bootloader/CPU1_FLASH/fw_update_bootloader.out`), and a `.ccxml` in `application/targetConfigs/`. The default tool locations (`C:\ti\ccs1281\...`) are the versions used by the maintainer; change them for your installation. Messages are in English.

Path assumptions: the build folder is `CPU1_FLASH` inside the project root (the CCS configuration documented in section 2); `make_image.bat` reads `..\CPU1_FLASH\fw_update_application.out` relative to `image_tools/`, so it expects the application CCS project to be rooted in `application/`. If your projects live elsewhere, edit `make_image.bat` or set `OUT`/`CCXML` for the flash scripts.

---

## 8. Verified inconsistencies noticed while collecting the facts

1. `application/main.c:192-193,255,331`: the self-tests call the 0x08C000 range "staging" (also "Bank 0 sector 12"), but 0x08C000 is the last application sector; staging starts at 0x08D000 (`flash_port_config.h`, layout). The application project may not erase 0x08C000 (`FLASH_PORT_WRITABLE`). Self-tests are disabled (lines 496-503).
2. `application/main.c:73`: the comment for the non-`ver_1` branch still says LED4 / 500 ms but the code is `LED5` / 300 ms.
3. `application/flash_port/flash_port_config.h:15` and the bootloader copy: "spare 0x09D000 .. 0x09FFEF"; sector 31 ends at 0x09FFFF (`flash_port.c:10`).
4. `28004x_app_flash_lnk.cmd:8`: `APP_FLASH` spans all 10 application sectors (to 0x08CFFF), but the updatable image capacity is 9 sectors (to 0x08BFFF when placed from 0x083000); only `finalize_image.ps1` enforces the limit.
5. `bootloader/flash_port/flash_port_config.h:23-26` does not allow writes to staging or to the scratch sector 0x09F000..0x09FFFF, which a SWAP-mode activation would need; with `FWU_PROFILE_DEV` and no `FWU_ACTIVATION_SWAP_ROLLBACK` (COPY mode) this is not exercised.
6. `app_fwu.c:134-135`: unbounded wait on the SCI TX FIFO before reset (no timeout, no watchdog).
7. Scripts' path assumptions (7.4) do not match the repository folder names.
