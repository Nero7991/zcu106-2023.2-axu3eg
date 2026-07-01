# CLAUDE.md — Alinx AXU3EG PetaLinux Project

Guidance for Claude Code working in this repository. Keep this file updated as the
project evolves.

## What this is

PetaLinux 2023.2 development for the **Alinx AXU3EG** board — a Zynq UltraScale+
MPSoC **XCZU3EG-SFVC784-1-E**. Based on the Xilinx ZCU106 BSP with board-specific
drivers and device-tree changes. The headline feature so far is **temperature-based
fan control** using the Linux thermal framework.

Board connection during development:
- **USB (JTAG):** bitstream / boot-image programming.
- **Ethernet:** SSH into the running Linux for driver iteration and testing.

## Project goals

1. **DONE (on/off):** Control the board fan from the on-die thermal sensors. Working,
   but coarse — the fan is on/off only, with a single 65 °C trip and 10 °C hysteresis.
2. **IN PROGRESS — PWM fan control:** Move from on/off to proportional (PWM) fan
   speed so the fan ramps with temperature instead of slamming fully on/off.
   - **Phase A (software PWM) — implemented and HARDWARE-VALIDATED (2026-07-01).**
     New module `soft-pwm-fan` (`recipes-modules/soft-pwm-fan/`) drives the existing
     1-bit `fan_gpio` line with an `hrtimer` soft-PWM (100 Hz, DT-tunable), exposing a
     10-state cooling device. Device tree switched to `xlnx,soft-pwm-fan` with a
     45->75 C staircase across all three zones (95 C critical kept). No FPGA change.
     `simple-gpio-fan` is left in place as an instant fallback (revert = change the
     `compatible` string back).
     Validated on the board: driver probes ("soft-pwm-fan: 10 levels, min 30%, 100 Hz"),
     registers `cooling_device0` (type `gpio-fan`, max_state 10). Measured GPIO duty at
     `0x80090000` vs commanded cooling state: 0->0%, 1->35%, 3->50%, 5->72%, 10->100%
     (tracks the configured `30 + 70*state/10` curve; endpoints clean off/full).
   - **KEY FINDING (2026-07-01): 100 Hz is too low for this fan.** The board fan is a
     2-wire brushless (BLDC) type. Under low-frequency supply chopping it does NOT
     speed-control: below ~15-20% duty it won't spin (stiction), above that it jumps to
     ~full — i.e. effectively on/off with a threshold, which is what's audible as the
     fan snapping on/off near a trip. The pin PWM is correct; the *fan* ignores 100 Hz.
     Confirmed empirically with a userspace mmap PWM generator (`scratchpad/pwm_test.c`,
     cross-compiled `aarch64-linux-gnu-gcc -static`, drives `/dev/mem` @ `0x80090000`):
     at **20 kHz the fan ramps proportionally** (audibly slows with duty); at 100 Hz it
     does not. Software PWM can't cleanly sustain 20 kHz (40k irq/s, jittery) -> this is
     why Phase B (hardware PWM) is REQUIRED for real proportional control, now de-risked
     since we've proven the fan responds at high frequency.
   - **Phase B (hardware PWM in PL) — DONE and HARDWARE-VALIDATED (2026-07-01).**
     Custom AXI4-Lite **`axi_pwm`** VHDL IP (25 kHz) replaces `fan_gpio` in `design_1`,
     driving `fan_tri_o` (AA11). New kernel driver **`pl-pwm-fan`** (compatible
     `user,axi-pwm-fan`) writes the IP's DUTY register from a device-tree
     `cooling-levels` table; reuses the thermal framework. Tuned by ear to a quiet
     curve: off < 50 C, 7% (self-starting floor) 50-70 C, ramp to 10% by 85 C, then
     30/60/100% by 94 C, 95 C critical, 2 C hysteresis. Verified on hardware: driver
     probes ("period=8000 states=0..7 levels 0 70 80 90 100 300 600 1000 per-mille"),
     DUTY register matches each state, fan off at idle (41 C). 2-wire fan speed-controls
     cleanly at 25 kHz (unlike Phase A's 100 Hz).
     Repos: FPGA design in `~/GitHub/AlinxMigrated` (git "zcu106-axu3eg-hardware",
     branch `hardware-pwm-fan`); reusable IP in `~/GitHub/axu3eg-pwm-ip`; plan in
     `docs/superpowers/plans/2026-07-01-hardware-pwm-fan-phase-b.md`.
3. **FUTURE — llama.c in VHDL:** Implement the [llama.c](https://github.com/karpathy/llama2.c)
   inference project in VHDL for a very small (FPGA-fitting) LLM, running in the PL.

## Current fan-control architecture (as-built)

Data flow: **PS SysMon thermal diodes → Linux thermal zones (DT trip points) →
`simple-gpio-fan` cooling device → 1-bit AXI GPIO in the PL → fan transistor.**

### Temperature sensors
- Driver: `project-spec/meta-user/recipes-modules/zynqmp-pssysmon-temp/files/zynqmp-pssysmon-temp.c`
  - Compatible `xlnx,zynqmp-pssysmon-temp`; registers thermal-zone sensors via
    `thermal_of_zone_register()`. Reads PS-SysMon temperature registers by
    `ioread32()`; conversion `temp_C = (val >> 7) - 280`, returned in milli-C.
    Supports `set_emul_temp` (write register) for emulation/testing.
  - Three sensors (addresses in `system-user.dtsi`): APU/FPD `0xffa50a14`,
    PL `0xffa50c00`, PS/LPD `0xffa50800`.
- An LM75 hwmon sensor exists on `i2c1` (`lm75@48` in `pl.dtsi`) but is NOT wired into
  the thermal zones.

### Fan cooling device
- Driver: `project-spec/meta-user/recipes-modules/simple-gpio-fan/files/simple-gpio-fan.c`
  - Compatible `xlnx,simple-gpio-fan`. **On/off only** (`max_state = 1`, two states).
  - Output is **software active-low**: cooling state ≥1 writes GPIO value **0** (fan ON);
    state ≤0 writes GPIO value **1** (fan OFF). Forces the line to output at probe.
  - Registered via `devm_thermal_of_cooling_device_register()`. No temperature reading
    or thresholds in the C code — that all lives in the device tree.

### Device tree (source of the on/off policy)
- Authoritative overlay: `project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`
  - `gpio-fan` cooling node: `gpios = <&fan_gpio 0 GPIO_ACTIVE_HIGH>`, `#cooling-cells = <2>`.
  - Three thermal zones (`apu_thermal`, `pl_thermal`, `ps_thermal`): `active` trip at
    **65000 mC** with **10000 mC hysteresis**, `critical` at **95000 mC**. Cooling map
    `cooling-device = <&cooling_device 0 1>` — fan on when any zone crosses 65 °C,
    off after dropping 10 °C.
- Generated PL nodes: `components/plnx_workspace/device-tree/device-tree/pl.dtsi`
  - `fan_gpio: gpio@80090000` — `xlnx,axi-gpio-2.0`, **1-bit, all-outputs**, single channel.

### How the fan pin is wired (matters for PWM)
- The fan is on a **pure PL pin**, not a PS MIO pin and not EMIO.
- In the hardware handoff (`design_1.hwh` inside `system.xsa`), AXI GPIO `fan_gpio`
  output `gpio_io_o` maps to top-level port **`fan_tri_o`** (1-bit output), driving the
  fan MOSFET. Only `TRI_O` is mapped (all-outputs).
- The **package pin** for `fan_tri_o` is set in the `design_1` top-level XDC, which is
  **not in the repo** — it is baked into `alinx_axu3eg_stock.bit`. Recovering it needs
  the Alinx reference design or the schematic.

### Module enablement
- `project-spec/meta-user/conf/user-rootfsconfig`: `CONFIG_zynqmp-pssysmon-temp`,
  `CONFIG_simple-gpio-fan`.
- Recipes: `.../recipes-modules/{simple-gpio-fan,zynqmp-pssysmon-temp}/*.bb` (both
  `inherit module`).

## FPGA / hardware source status (important)

- The **deployed** AXU3EG design (`design_1`) exists only as an **exported**
  `system.xsa` / `alinx_axu3eg_stock.bit` / `.mmi` / `.hwh` under
  `project-spec/hw-description/` and `components/plnx_workspace/...`. There is **no
  editable Vivado project** for it in this repo.
- The Vivado `.xpr` projects under `hardware/xilinx-zcu106-2023.2{,-Rev2}/` are
  **unrelated stock ZCU106 VCU/camera reference designs** (`project_1`/`project_2`),
  with no fan IP. Do not assume they drive the fan.
- Consequence: any *hardware* PWM approach requires reconstructing or obtaining the
  Alinx `design_1` Vivado project (including the fan pin XDC). A *software* PWM approach
  on the existing GPIO needs no FPGA rebuild.

## Build & deploy (dev loop)

Tools: PetaLinux at `~/petalinux/2023.2/settings.sh` (project `TMPDIR` on
`/mnt/storage/petalinux`); Vivado/Vitis 2023.2 at `/tools/Xilinx/2023.2/...`
(`xsct`/`hw_server` for JTAG).

- **Build:** `source ~/petalinux/2023.2/settings.sh` then, from the project root,
  `petalinux-build` (full) or `petalinux-build -c <recipe>` (single, e.g.
  `soft-pwm-fan`). Outputs land in `images/linux/` and are auto-copied to
  `/tftpboot` (`CONFIG_SUBSYSTEM_COPY_TO_TFTPBOOT=y`).
- **Boot method (as set up early 2024):** SD-card holds `BOOT.BIN`
  (FSBL/PMU/ATF/U-Boot); **U-Boot then TFTPs `image.ub` from this host's
  `/tftpboot`** and boots it. Rootfs is `CONFIG_SUBSYSTEM_ROOTFS_INITRD=y` — a RAM
  disk baked inside `image.ub` (113 MB), so runtime changes are ephemeral; persist
  by rebuilding `image.ub`. This is the "network boot" flow: rebuild -> reboot board
  -> new OS, no SD reflash. `tftpd-hpa` serves `/tftpboot`; NFS is unused.
- **Kernel module iteration:** the recipe installs the `.ko` into the rootfs
  (auto-loaded via OF modalias since the DT node is present). For a quick runtime
  test without a full image rebuild, `insmod` the freshly built `.ko` over the
  console/scp once the board is networked.
- **JTAG (Phase B / recovery):** onboard **FT232H (USB 0403:6014)** is the board's
  JTAG; `hw_server` + `xsct` reach it when the board is powered and cabled.
- **Board serial console = `/dev/ttyUSB3` (CP2102N, USB `10c4:ea60`) @ 115200 8N1.**
  The board's UART is a Silicon Labs CP2102N (NOT one of the three CH340s — `ttyUSB0`
  is the Kiprim PSU, `ttyUSB1`/`ttyUSB2` are other bench gear incl. QuickVolt). If the
  port number moves, identify by the `10c4:ea60` device. Scripted access helpers live
  in the session scratchpad (`ub.py` for U-Boot, `rootrun.py` for the Linux root
  console) — small pyserial wrappers; open with `dtr=False, rts=False` to avoid
  toggling lines, and `stty -echo` before marker-based command capture.

### Board access (working as of 2026-07-01)

- **Serial:** `/dev/ttyUSB3` @ 115200 (see above). Board debug USB (CP2102N UART +
  FT232H JTAG) and Ethernet are cabled to this PC; the correct Ethernet port is the
  one on **GEM3 / `ethernet@ff0e0000`** (U-Boot `ping` confirmed).
- **U-Boot network + bitstream (persisted via `saveenv` to SPI flash):** board `ipaddr
  192.168.97.47`, `serverip 192.168.97.169` (this host), `netmask 255.255.255.0`. Since
  Phase B the `bootcmd` also TFTP-loads the PL bitstream before the kernel:
  `tftpboot 0x10000000 ${serverip}:system.bit.bin && fpga load 0 0x10000000 ${filesize}
  && tftpboot 0x10000000 ${serverip}:image.ub && bootm 0x10000000`. So a plain power-on
  loads the current `/tftpboot/{system.bit.bin,image.ub}` from this host — PL changes
  deploy over TFTP too, no SD swap. (Bitstream is NOT in BOOT.BIN or image.ub here;
  the FIT `.its` carries only kernel+FDT+ramdisk. `system.bit.bin` = raw bitstream from
  `bootgen -arch zynqmp -image <bif> -process_bitstream bin`; U-Boot uses `fpga load`
  for the raw .bin, `loadb` is for a .bit-with-header. To rebuild the bitstream: build
  in the hardware repo, regenerate `system.bit.bin`, copy to `/tftpboot`, reboot.)
  (The old stale values were board `192.168.0.47`, server hardcoded `192.168.0.46`.)
- **Login:** rootfs now built with `debug-tweaks` + `empty-root-password` +
  `serial-autologin-root` (set in `rootfs_config`), so the serial console
  **auto-logs-in as root** — no password. (Stock creds were unusable: `root:root` from
  `ADD_EXTRA_USERS` is the broken-hash case; `petalinux:petalinux` works but forces a
  password change that a RAM-initrd loses on reboot.)
- **SSH / Linux networking (WORKING):** Linux networking is now **DHCP**
  (`CONFIG_SUBSYSTEM_ETHERNET_PSU_ETHERNET_3_USE_DHCP=y` in `config`). NOTE PetaLinux
  REGENERATES `configs/systemd-conf/wired.network` from `config` at build time, so set
  DHCP/static in `config` (via `petalinux-config` or edit + `petalinux-config
  --silentconfig`), NOT by editing the generated `wired.network`. On boot the board
  DHCPs a LAN address; find it in the router leases by MAC `00:0a:35:00:22:01`
  (`ssh orencollaco@192.168.97.1 'grep 00:0a:35 /var/lib/misc/dnsmasq.leases'`) — it was
  `192.168.97.101`, but DHCP so it can change.
  **SSH login is `root` / password `root`** (works over SSH even though it failed on the
  serial console). Example helper:
  `sshpass -p root ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
  root@<ip> 'CMD'`. SSH is the preferred iteration path (serial CP2102 is flaky).

### Testing the fan

`CONFIG_THERMAL_EMULATION=y` is now built in, so
`/sys/class/thermal/thermal_zoneN/emul_temp` EXISTS — BUT writing it does NOT stick:
the `zynqmp-pssysmon-temp` driver implements `.set_emul_temp` by writing the SysMon
hardware register, which the live SysMon immediately overwrites, so the governor keeps
seeing the real ~40 C. (To make emulation work, patch `zynqmp-pssysmon-temp` to drop
its `.set_emul_temp` op so the thermal core's own `emul_temperature` override applies.)
CPU load barely heats the XCZU3EG die (stays ~43 C), so it won't cross the 45 C trip.

Reliable ways to exercise the fan:
- **Direct cooling state:** disable the governor per zone
  (`echo disabled > thermal_zoneN/mode`), write `cooling_device0/cur_state` (0..10),
  measure real pin duty with `devmem 0x80090000 32` (bit0; active-low, physical 0 = fan
  on). Re-enable with `echo enabled > thermal_zoneN/mode`.
- **Arbitrary frequency/duty (bypasses the driver):** `scratchpad/pwm_test.c`
  (`aarch64-linux-gnu-gcc -static`, mmaps `0x80090000`) — `pwm_test <freq_hz> <duty%>
  <secs>`. Kill cleanly with `pkill -9 -f pwm_test` (overlapping instances corrupt the
  duty). Used to prove the fan needs ~20 kHz (see KEY FINDING under goal 2).

## Working notes / conventions

- Iterate drivers over SSH (Ethernet); reprogram bitstream/boot over USB-JTAG.
- Thermal policy (trips/hysteresis) is device-tree-driven, not hard-coded in C.
- The fan drive is software-inverted (active-low) — preserve that when changing drivers.
