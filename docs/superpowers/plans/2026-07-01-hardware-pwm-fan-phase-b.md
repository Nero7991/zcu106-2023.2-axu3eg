# Hardware PWM Fan Control (Phase B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the on/off `fan_gpio` in the FPGA with a custom ~25 kHz VHDL PWM peripheral driving the fan pin (AA11), so the 2-wire brushless fan can be speed-controlled proportionally by the Linux thermal framework.

**Architecture:** A small custom AXI4-Lite PWM IP (`axi_pwm`) is added to the Alinx `design_1` block design, its output routed to `fan_tri_o` (package pin AA11) in place of the `fan_gpio` bit. The bitstream is rebuilt and exported as a new XSA, re-imported into PetaLinux. A kernel driver (`pl-pwm-fan`) writes the IP's duty register and registers as a thermal cooling device, reusing the existing 45→75 °C device-tree staircase from Phase A.

**Tech Stack:** VHDL (custom IP + testbench, GHDL/Vivado xsim for simulation), Vivado 2023.2 (headless Tcl), PetaLinux 2023.2, Linux kernel 6.1 platform driver, device tree.

## Global Constraints

- Target part: `xczu3eg-sfvc784-1-e` (Alinx AXU3EG). Exact value — do not change.
- Fan pin constraint (verbatim): `set_property PACKAGE_PIN AA11 [get_ports {fan_tri_o[0]}]`, `set_property IOSTANDARD LVCMOS33 [get_ports {fan_tri_o[0]}]`.
- Fan wiring is active-low: physical `0` on AA11 = fan powered ON. The PWM "on" (duty) phase must drive AA11 LOW.
- PWM carrier frequency target: 25 kHz (validated: the fan speed-controls at ~20 kHz, not at 100 Hz).
- Vivado source project: `/home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xpr` (block design `design_1`, contains `fan_gpio` = `xilinx.com:ip:axi_gpio:2.0`, 1-bit all-outputs, at AXI `0x80090000`). A second copy is `/home/orencollaco/GitHub/Alinx/vivado/axu3eg_trd.xpr` (fallback).
- Tools: Vivado at `/tools/Xilinx/2023.2/Vivado/2023.2/settings64.sh`; PetaLinux at `~/petalinux/2023.2/settings.sh`; PetaLinux project at `/home/orencollaco/GitHub/zcu106-2023.2-axu3eg`.
- Board access: TFTP boot from this host (`/tftpboot`), SSH `root@<dhcp-ip>` password `root` (find IP via router lease MAC `00:0a:35:00:22:01`), serial `/dev/ttyUSB3` @115200 (flaky — prefer SSH). Fan GPIO/PWM register read on board via `devmem 0x80090000 32` (bit0).
- Keep the rest of `design_1` (rs485, leds, keys, video, ethernet) intact — modify only the fan path.
- PWM register map (custom IP `axi_pwm`, base stays `0x80090000`, 64 KB segment):
  - `0x00 CTRL` — bit0 `ENABLE` (1 = run), bit1 `POLARITY` (1 = invert output; set to 1 for active-low fan). Reset = 0 (output holds the off level).
  - `0x04 PERIOD` — carrier period in PL clock cycles. For 25 kHz at 100 MHz = `4000`.
  - `0x08 DUTY` — high-time in PL clock cycles, `0..PERIOD` (before polarity inversion).
  - `0x0C STATUS` — read-only, bit0 = current raw PWM level (for debug).

---

### Task 1: Custom PWM core (VHDL) with simulation

**Files:**
- Create: `/home/orencollaco/GitHub/axu3eg-pwm-ip/hdl/pwm_core.vhd`
- Test: `/home/orencollaco/GitHub/axu3eg-pwm-ip/sim/tb_pwm_core.vhd`
- Create: `/home/orencollaco/GitHub/axu3eg-pwm-ip/sim/run_ghdl.sh`

(A standalone repo dir keeps the reusable IP separate from the PetaLinux project. If GHDL is unavailable, Task 1 Step 2/4 fall back to Vivado `xsim` — see the run script.)

**Interfaces:**
- Consumes: nothing.
- Produces: `pwm_core` entity — `generic ()`; ports `clk : in std_logic; rst_n : in std_logic; enable : in std_logic; polarity : in std_logic; period : in unsigned(31 downto 0); duty : in unsigned(31 downto 0); pwm_out : out std_logic; raw_level : out std_logic`. `pwm_out = raw XOR polarity`, where `raw = '1' while counter < duty`. When `enable='0'`, counter resets and `pwm_out = polarity` (off level).

- [ ] **Step 1: Write the failing testbench**

```vhdl
-- sim/tb_pwm_core.vhd
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity tb_pwm_core is end;
architecture sim of tb_pwm_core is
  signal clk, rst_n, enable, polarity, pwm_out, raw_level : std_logic := '0';
  signal period, duty : unsigned(31 downto 0) := (others=>'0');
  constant PER : integer := 100;   -- short period for sim
begin
  uut: entity work.pwm_core port map(clk,rst_n,enable,polarity,period,duty,pwm_out,raw_level);
  clk <= not clk after 5 ns;       -- 100 MHz
  process
    variable high : integer;
  begin
    rst_n <= '0'; enable <= '0'; polarity <= '0';
    period <= to_unsigned(PER,32); duty <= to_unsigned(25,32);  -- 25% duty
    wait for 20 ns; rst_n <= '1'; wait for 20 ns;
    -- disabled: output must sit at off level (polarity=0 -> '0')
    assert pwm_out = '0' report "disabled output wrong" severity failure;
    enable <= '1';
    -- measure high-time over exactly one period
    high := 0;
    for i in 0 to PER-1 loop
      wait until rising_edge(clk);
      if pwm_out = '1' then high := high + 1; end if;
    end loop;
    assert (high >= 24 and high <= 26)
      report "duty not ~25% (got " & integer'image(high) & ")" severity failure;
    -- active-low: polarity=1 must invert
    polarity <= '1'; wait until rising_edge(clk); wait for 1 ns;
    assert pwm_out = not raw_level report "polarity invert wrong" severity failure;
    report "PWM_CORE_TEST_PASS" severity note;
    std.env.finish;
  end process;
end;
```

- [ ] **Step 2: Run the testbench to verify it fails (no core yet)**

```bash
# sim/run_ghdl.sh
set -e
cd "$(dirname "$0")/.."
if command -v ghdl >/dev/null; then
  ghdl -a --std=08 hdl/pwm_core.vhd sim/tb_pwm_core.vhd
  ghdl -r --std=08 tb_pwm_core
else   # Vivado xsim fallback
  source /tools/Xilinx/2023.2/Vivado/2023.2/settings64.sh
  xvhdl --2008 hdl/pwm_core.vhd sim/tb_pwm_core.vhd
  xelab -debug typical tb_pwm_core -s tbsim && xsim tbsim -runall
fi
```
Run: `bash sim/run_ghdl.sh`
Expected: FAIL — `pwm_core.vhd` missing / analysis error (unit `pwm_core` not found).

- [ ] **Step 3: Implement `pwm_core.vhd`**

```vhdl
-- hdl/pwm_core.vhd
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity pwm_core is
  port (
    clk       : in  std_logic;
    rst_n     : in  std_logic;
    enable    : in  std_logic;
    polarity  : in  std_logic;                    -- 1 = invert (active-low fan)
    period    : in  unsigned(31 downto 0);        -- cycles per PWM period
    duty      : in  unsigned(31 downto 0);        -- high cycles (0..period)
    pwm_out   : out std_logic;
    raw_level : out std_logic
  );
end;
architecture rtl of pwm_core is
  signal cnt : unsigned(31 downto 0) := (others=>'0');
  signal raw : std_logic := '0';
begin
  process(clk)
  begin
    if rising_edge(clk) then
      if rst_n = '0' or enable = '0' then
        cnt <= (others=>'0');
        raw <= '0';
      else
        if cnt + 1 >= period then
          cnt <= (others=>'0');
        else
          cnt <= cnt + 1;
        end if;
        if cnt < duty then raw <= '1'; else raw <= '0'; end if;
      end if;
    end if;
  end process;
  raw_level <= raw;
  pwm_out   <= raw xor polarity;
end;
```

- [ ] **Step 4: Run the testbench to verify it passes**

Run: `bash sim/run_ghdl.sh`
Expected: PASS — output contains `PWM_CORE_TEST_PASS`, no assertion failures.

- [ ] **Step 5: Commit**

```bash
cd /home/orencollaco/GitHub/axu3eg-pwm-ip && git init -q && git add -A
git commit -q -m "pwm_core: 25kHz PWM core with polarity + sim"
```

---

### Task 2: Package `axi_pwm` AXI4-Lite IP and wire the register map

**Files:**
- Create (generated by Vivado): `/home/orencollaco/GitHub/axu3eg-pwm-ip/ip_repo/axi_pwm_1.0/` (peripheral)
- Modify: the generated `hdl/axi_pwm_v1_0_S00_AXI.vhd` (register→core wiring)
- Create: `/home/orencollaco/GitHub/axu3eg-pwm-ip/tcl/package_ip.tcl`

**Interfaces:**
- Consumes: `pwm_core` (Task 1).
- Produces: Vivado IP `user.org:user:axi_pwm:1.0` with an AXI4-Lite slave (4 regs per Global Constraints) and one output port `pwm_out`. `slv_reg0=CTRL, slv_reg1=PERIOD, slv_reg2=DUTY`; `slv_reg3`/STATUS returns `raw_level`. (VLNV must match Task 3's `create_bd_cell -vlnv user.org:user:axi_pwm:1.0`.)

- [ ] **Step 1: Generate the AXI4-Lite peripheral template**

```tcl
# tcl/package_ip.tcl  (run: vivado -mode batch -source tcl/package_ip.tcl)
set root /home/orencollaco/GitHub/axu3eg-pwm-ip
create_project -force tmp_ip $root/ip_proj -part xczu3eg-sfvc784-1-e
create_peripheral user.org user axi_pwm 1.0 -dir $root/ip_repo
add_peripheral_interface S00_AXI -interface_mode slave -axi_type lite [ipx::find_open_core user.org:user:axi_pwm:1.0]
generate_peripheral -driver [ipx::find_open_core user.org:user:axi_pwm:1.0]
write_peripheral [ipx::find_open_core user.org:user:axi_pwm:1.0]
```
Run: `source /tools/Xilinx/2023.2/Vivado/2023.2/settings64.sh && vivado -mode batch -source /home/orencollaco/GitHub/axu3eg-pwm-ip/tcl/package_ip.tcl`
Expected: `ip_repo/axi_pwm_1.0/hdl/axi_pwm_v1_0_S00_AXI.vhd` and `axi_pwm_v1_0.vhd` exist. (Template gives 4 x 32-bit `slv_reg0..3`.)

- [ ] **Step 2: Add `pwm_out` port to the top wrapper**

In `ip_repo/axi_pwm_1.0/hdl/axi_pwm_v1_0.vhd`, add port `pwm_out : out std_logic;` to the entity and pass it through to the `axi_pwm_v1_0_S00_AXI` instance (add matching port on that component + instance).

- [ ] **Step 3: Wire registers to `pwm_core` in `axi_pwm_v1_0_S00_AXI.vhd`**

Copy `hdl/pwm_core.vhd` into `ip_repo/axi_pwm_1.0/hdl/` and add it to the IP (`ipx` file group) — or add via `package_ip.tcl` `ipx::add_file`. Add `pwm_out : out std_logic;` to this entity, and before `end architecture` instantiate the core:

```vhdl
-- reg0=CTRL(bit0 enable,bit1 polarity), reg1=PERIOD, reg2=DUTY
u_pwm : entity work.pwm_core
  port map (
    clk       => S_AXI_ACLK,
    rst_n     => S_AXI_ARESETN,
    enable    => slv_reg0(0),
    polarity  => slv_reg0(1),
    period    => unsigned(slv_reg1),
    duty      => unsigned(slv_reg2),
    pwm_out   => pwm_out,
    raw_level => open
  );
```
Also make register-read of address 3 return status: in the read mux, set `reg_data_out <= (0 => raw_level_sig, others => '0')` for `slv_reg3` (optional debug; requires exposing `raw_level` to a signal).

- [ ] **Step 4: Re-package and verify the IP has `pwm_out`**

```tcl
# append to package_ip.tcl or a repackage.tcl
ipx::open_core /home/orencollaco/GitHub/axu3eg-pwm-ip/ip_repo/axi_pwm_1.0/component.xml
ipx::merge_project_changes ports [ipx::current_core]
ipx::save_core [ipx::current_core]
```
Run the tcl. Expected: `component.xml` lists an output port `pwm_out`. Grep: `grep -c pwm_out ip_repo/axi_pwm_1.0/component.xml` ≥ 1.

- [ ] **Step 5: Commit**

```bash
cd /home/orencollaco/GitHub/axu3eg-pwm-ip
git add -A && git commit -q -m "axi_pwm: package AXI4-Lite PWM IP (CTRL/PERIOD/DUTY regs)"
```

---

## Synthesis speed strategy (single-track: OOC cache + incremental impl)

The TRD has 31 cells incl. slow blocks (PL `axi_ethernet`+DMA, PL `ddr4`,
`mipi_csi2_rx`, `v_proc_ss`, `v_frmbuf_wr`). A *from-scratch* build is 30–60 min, but a
**one-IP change is not from-scratch**:

- **OOC IP synth is cached per-IP.** In the BD flow each IP synthesizes out-of-context to
  its own checkpoint. Changing only `axi_pwm` re-runs just that IP's (tiny) OOC synth; the
  heavy IPs stay "Complete" and are reused. The AlinxMigrated project already has these
  runs cached, so we never re-synthesize video/ddr4/ethernet.
- **Incremental implementation** handles the remaining long pole (place & route). With the
  stock impl as a reference checkpoint, unchanged logic keeps its placement/routing and
  only the swapped fan logic is re-placed/routed. `axi_pwm` ≈ `axi_gpio` footprint (one
  AXI-Lite slave, one output pin) → a tiny perturbation.

Net: iterate directly on the **real full TRD** — no separate minimal design to build or
keep in sync (rejected as convoluted). Expect ~10–15 min per fan-only iteration
(`axi_pwm` OOC + top synth + incremental impl + bitstream), with `-jobs 16` and
`Flow_RuntimeOptimized` (the fan PWM has trivial timing, so no aggressive closure needed).

### Task 3: Swap `fan_gpio` → `axi_pwm` in `design_1` (OOC cache + incremental impl)

**Files:**
- Create: `/home/orencollaco/GitHub/axu3eg-pwm-ip/tcl/modify_design.tcl`
- Produces: `/home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xsa` (new hardware handoff)

**Interfaces:**
- Consumes: IP `user.org:user:axi_pwm:1.0` (Task 2).
- Produces: a bitstream + XSA where `axi_pwm` sits at `0x80090000` and its `pwm_out` reaches top-level port `fan_tri_o[0]` (AA11).

- [ ] **Step 1: Confirm the project opens and note the PL clock (headless)**

```tcl
# quick_open.tcl
open_project /home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xpr
open_bd_design [get_files design_1.bd]
puts "PLCLK: [get_property CONFIG.FREQ_HZ [get_bd_pins fan_gpio/s_axi_aclk]]"
```
Run: `vivado -mode batch -source quick_open.tcl 2>&1 | grep PLCLK`
Expected: prints the fan_gpio AXI clock freq (e.g., `100000000`). Record it as `PLCLK`; PERIOD = `PLCLK/25000` (100 MHz → 4000). If the project errors on open, use the `Alinx/vivado` copy.

- [ ] **Step 2: Modify the block design (remove fan_gpio, add axi_pwm on same AXI + address, route to fan_tri_o)**

```tcl
# tcl/modify_design.tcl
set proj /home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xpr
open_project $proj
set_property ip_repo_paths /home/orencollaco/GitHub/axu3eg-pwm-ip/ip_repo [current_project]
update_ip_catalog
open_bd_design [get_files design_1.bd]

# capture the AXI master + clock/reset nets feeding the old fan_gpio
set aclk  [get_bd_nets -of_objects [get_bd_pins fan_gpio/s_axi_aclk]]
set arstn [get_bd_nets -of_objects [get_bd_pins fan_gpio/s_axi_aresetn]]
set axim  [find_bd_objs -relation connected_to [get_bd_intf_pins fan_gpio/S_AXI]]

delete_bd_objs [get_bd_cells fan_gpio]
create_bd_cell -type ip -vlnv user.org:user:axi_pwm:1.0 fan_pwm

# reconnect clock/reset/AXI exactly as fan_gpio had them
connect_bd_net [get_bd_pins fan_pwm/s00_axi_aclk]    $aclk
connect_bd_net [get_bd_pins fan_pwm/s00_axi_aresetn] $arstn
connect_bd_intf_net $axim [get_bd_intf_pins fan_pwm/S00_AXI]

# external output port -> reuse fan_tri_o name so the XDC still applies
if {[llength [get_bd_ports fan_tri_o]] == 0} {
  create_bd_port -dir O fan_tri_o
}
connect_bd_net [get_bd_pins fan_pwm/pwm_out] [get_bd_ports fan_tri_o]

# pin the address to 0x80090000 (unchanged) so the driver base is stable
assign_bd_address -target_address_space [get_bd_addr_spaces */Data] \
  [get_bd_addr_segs fan_pwm/S00_AXI/reg0] -offset 0x80090000 -range 64K
validate_bd_design
save_bd_design
```
Run: `vivado -mode batch -source tcl/modify_design.tcl 2>&1 | tail -20`
Expected: `validate_bd_design` reports no critical errors. If `fan_tri_o` was a 1-bit vector (`[0:0]`), adjust `create_bd_port -dir O -from 0 -to 0 fan_tri_o` to keep the XDC `fan_tri_o[0]` name.

- [ ] **Step 3: Synthesize + implement (incremental) + export XSA (background it)**

```tcl
# build.tcl — OOC IP synth is cached (only axi_pwm re-synths); impl is incremental.
# Set an incremental reference from the stock implemented checkpoint if present.
set ref /home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.runs/impl_1/design_1_wrapper_routed.dcp
if {[file exists $ref]} {
  set_property incremental_checkpoint $ref [get_runs impl_1]
}
set_property strategy Flow_RuntimeOptimized [get_runs synth_1]
set_property strategy Flow_RuntimeOptimized [get_runs impl_1]
launch_runs impl_1 -to_step write_bitstream -jobs 16
wait_on_run impl_1
write_hw_platform -fixed -include_bit -force -file /home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xsa
```
Run in background: `vivado -mode batch -source tcl/build.tcl > /tmp/vivado_build.log 2>&1 &`
Expected: **first** build after the swap ~20–30 min (only `axi_pwm` OOC re-synths, heavy IPs
reused; one incremental impl); **subsequent** fan-only iterations ~10–15 min. `axu3eg_trd.xsa`
written. Verify: `grep -iE "Bitstream generation completed|write_hw_platform.*Complete" /tmp/vivado_build.log`.
Incremental impl reuse is reported in the log as `% of cells reused` — expect a high figure
(the fan is a tiny fraction of the device).

- [ ] **Step 4: Sanity-check the XSA contains the fan_tri_o/axi_pwm**

```bash
unzip -l /home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xsa | grep -iE 'hwh|bit'
python3 -c "import zipfile,sys; z=zipfile.ZipFile('/home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xsa'); h=[n for n in z.namelist() if n.endswith('.hwh')][0]; d=z.read(h).decode(errors='replace'); print('axi_pwm' , 'axi_pwm' in d); print('0x80090000', '80090000' in d.replace('0x','').lower())"
```
Expected: prints `axi_pwm True` and address present.

- [ ] **Step 5: Commit the IP/tcl (XSA is large — keep out of git)**

```bash
cd /home/orencollaco/GitHub/axu3eg-pwm-ip
echo "*.xsa" >> .gitignore
git add -A && git commit -q -m "design_1: replace fan_gpio with axi_pwm @0x80090000 -> fan_tri_o (AA11)"
```

---

### Task 4: Re-import XSA into PetaLinux and update the device tree

**Files:**
- Modify (PetaLinux, via tool): `project-spec/hw-description/*` (from new XSA)
- Modify: `project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`

**Interfaces:**
- Consumes: `axu3eg_trd.xsa` (Task 3).
- Produces: a device-tree node `fan_pwm@80090000` (compatible `user,axi-pwm-fan`) that Task 5's driver binds to; the old `&fan_gpio`-based `gpio-fan` cooling node is replaced.

- [ ] **Step 1: Re-import the hardware description**

```bash
source ~/petalinux/2023.2/settings.sh
cd /home/orencollaco/GitHub/zcu106-2023.2-axu3eg
petalinux-config --get-hw-description=/home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xsa --silentconfig
```
Expected: completes; `components/plnx_workspace/device-tree/device-tree/pl.dtsi` now shows a node at `0x80090000` that is the AXI PWM (an `axi_gpio` node will be GONE; the generic node may appear as `axi_pwm` or a generic slave). Verify: `grep -A3 '80090000' components/plnx_workspace/device-tree/device-tree/pl.dtsi`.

- [ ] **Step 2: Replace the cooling-device DT node**

In `system-user.dtsi`, replace the Phase A `cooling_device: gpio-fan { compatible="xlnx,soft-pwm-fan"; ... }` node with:

```dts
	cooling_device: fan-pwm@80090000 {
		compatible = "user,axi-pwm-fan";
		reg = <0x0 0x80090000 0x0 0x10000>;
		pwm-period-cycles = <4000>;   /* 100 MHz / 25 kHz; adjust to PLCLK */
		min-duty-percent = <30>;
		num-levels = <10>;
		active-low;                   /* set CTRL.POLARITY=1 */
		#cooling-cells = <2>;
	};
```
Keep the `thermal-zones` staircase from Phase A unchanged (it references `&cooling_device`). Verify brace balance: `awk '{o+=gsub(/{/,"{");c+=gsub(/}/,"}")}END{print o,c}' project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`.

- [ ] **Step 3: Verify no dtc errors on a DT-only build**

```bash
source ~/petalinux/2023.2/settings.sh
petalinux-build -c device-tree 2>&1 | tail -5
```
Expected: `Successfully built device-tree` (no dtc syntax errors).

- [ ] **Step 4: Commit**

```bash
cd /home/orencollaco/GitHub/zcu106-2023.2-axu3eg
git add project-spec/hw-description project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi components/plnx_workspace/device-tree/device-tree/pl.dtsi
git commit -q -m "hw: import axi_pwm XSA; DT fan-pwm cooling node @0x80090000"
```

---

### Task 5: `pl-pwm-fan` kernel driver + on-board verification

**Files:**
- Create: `project-spec/meta-user/recipes-modules/pl-pwm-fan/pl-pwm-fan.bb`
- Create: `project-spec/meta-user/recipes-modules/pl-pwm-fan/files/{pl-pwm-fan.c,Makefile,COPYING}`
- Modify: `project-spec/meta-user/conf/user-rootfsconfig`, `project-spec/configs/rootfs_config`

**Interfaces:**
- Consumes: DT node `user,axi-pwm-fan` (Task 4).
- Produces: a `thermal_cooling_device` (`cooling_device0`, max_state = num-levels) that writes PERIOD/DUTY/CTRL to the mapped registers. Reuses the Phase A duty curve `duty = min + (100-min)*state/N`.

- [ ] **Step 1: Write the driver**

```c
// files/pl-pwm-fan.c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/thermal.h>
#define REG_CTRL 0x00
#define REG_PERIOD 0x04
#define REG_DUTY 0x08
#define CTRL_ENABLE BIT(0)
#define CTRL_POLARITY BIT(1)
struct plpwm { void __iomem *base; u32 period, num_levels, min_duty; bool active_low;
               struct thermal_cooling_device *cdev; unsigned long cur; };
static void plpwm_apply(struct plpwm *p, unsigned long state){
	u32 duty, ctrl = CTRL_ENABLE | (p->active_low ? CTRL_POLARITY : 0);
	if (state > p->num_levels) state = p->num_levels;
	if (state == 0) duty = 0;
	else { u32 pct = p->min_duty + (100 - p->min_duty) * state / p->num_levels;
	       if (pct > 100) pct = 100; duty = (u64)p->period * pct / 100; }
	writel(p->period, p->base + REG_PERIOD);
	writel(duty,      p->base + REG_DUTY);
	writel(ctrl,      p->base + REG_CTRL);
	p->cur = state;
}
static int plpwm_get_max(struct thermal_cooling_device *c, unsigned long *s){ *s = ((struct plpwm*)c->devdata)->num_levels; return 0; }
static int plpwm_get_cur(struct thermal_cooling_device *c, unsigned long *s){ *s = ((struct plpwm*)c->devdata)->cur; return 0; }
static int plpwm_set_cur(struct thermal_cooling_device *c, unsigned long s){ plpwm_apply(c->devdata, s); return 0; }
static const struct thermal_cooling_device_ops plpwm_ops = { .get_max_state=plpwm_get_max, .get_cur_state=plpwm_get_cur, .set_cur_state=plpwm_set_cur };
static int plpwm_probe(struct platform_device *pdev){
	struct device *dev = &pdev->dev; struct device_node *np = dev->of_node;
	struct plpwm *p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL); if(!p) return -ENOMEM;
	p->base = devm_platform_ioremap_resource(pdev, 0); if (IS_ERR(p->base)) return PTR_ERR(p->base);
	if (of_property_read_u32(np,"pwm-period-cycles",&p->period)) p->period = 4000;
	if (of_property_read_u32(np,"num-levels",&p->num_levels) || !p->num_levels) p->num_levels = 10;
	if (of_property_read_u32(np,"min-duty-percent",&p->min_duty) || p->min_duty>100) p->min_duty = 30;
	p->active_low = of_property_read_bool(np,"active-low");
	plpwm_apply(p, 0); /* start off */
	p->cdev = devm_thermal_of_cooling_device_register(dev, np, (char*)pdev->name, p, &plpwm_ops);
	if (IS_ERR(p->cdev)) return PTR_ERR(p->cdev);
	platform_set_drvdata(pdev, p);
	dev_info(dev,"pl-pwm-fan: period=%u levels=%u min=%u%% active_low=%d\n",p->period,p->num_levels,p->min_duty,p->active_low);
	return 0;
}
static int plpwm_remove(struct platform_device *pdev){ plpwm_apply(platform_get_drvdata(pdev),0); return 0; }
static const struct of_device_id plpwm_of[] = { {.compatible="user,axi-pwm-fan"}, {} };
MODULE_DEVICE_TABLE(of, plpwm_of);
static struct platform_driver plpwm_driver = { .driver={.name="pl-pwm-fan",.of_match_table=plpwm_of}, .probe=plpwm_probe, .remove=plpwm_remove };
module_platform_driver(plpwm_driver);
MODULE_LICENSE("GPL"); MODULE_AUTHOR("Oren Collaco"); MODULE_DESCRIPTION("PL AXI-PWM fan cooling device");
```

- [ ] **Step 2: Add Makefile, COPYING, recipe; enable in rootfs**

Copy `Makefile` (obj-m := pl-pwm-fan.o) and `COPYING` from `soft-pwm-fan`, and a `.bb` mirroring `soft-pwm-fan.bb` (SRC_URI `pl-pwm-fan.c`). Add `CONFIG_pl-pwm-fan` to `project-spec/meta-user/conf/user-rootfsconfig` and `CONFIG_pl-pwm-fan=y` to `project-spec/configs/rootfs_config`.

- [ ] **Step 3: Build the module (verify it compiles)**

```bash
source ~/petalinux/2023.2/settings.sh
cd /home/orencollaco/GitHub/zcu106-2023.2-axu3eg && petalinux-build -c pl-pwm-fan 2>&1 | tail -3
```
Expected: `Successfully built pl-pwm-fan`.

- [ ] **Step 4: Full build + program bitstream + boot, verify probe**

```bash
petalinux-build 2>&1 | tail -2   # image.ub -> /tftpboot
# program new PL: BOOT.BIN carries the bitstream; regenerate + write to SD, OR jtag-load.
petalinux-package --boot --u-boot --fpga images/linux/system.bit --force 2>&1 | tail -3
# power-cycle board (new bitstream needs BOOT.BIN/JTAG, not just image.ub)
# then find IP + ssh:
ROUTER=192.168.97.1; MAC=00:0a:35:00:22:01
BIP=$(ssh orencollaco@$ROUTER "grep -i $MAC /var/lib/misc/dnsmasq.leases" | awk '{print $3}')
sshpass -p root ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@$BIP \
  'dmesg | grep pl-pwm-fan; cat /sys/class/thermal/cooling_device0/type /sys/class/thermal/cooling_device0/max_state'
```
Expected: `pl-pwm-fan: period=4000 levels=10 ...`; cooling device type present, max_state 10.
NOTE: a new bitstream requires updating BOOT.BIN on the SD card (or JTAG-loading), unlike Phase A where only `image.ub` changed over TFTP. Confirm the SD `BOOT.BIN` is rebuilt with `system.bit`.

- [ ] **Step 5: Verify 25 kHz + proportional fan (the acceptance test)**

```bash
# on board via ssh: drive states, confirm the PWM register + audible ramp
sshpass -p root ssh ... root@$BIP 'bash -s' <<'RUN'
for z in /sys/class/thermal/thermal_zone*; do echo disabled > $z/mode; done
for s in 0 3 5 8 10; do echo $s > /sys/class/thermal/cooling_device0/cur_state;
  echo "state=$s DUTY_reg=$(devmem 0x80090008 32) CTRL=$(devmem 0x80090000 32)"; sleep 3; done
for z in /sys/class/thermal/thermal_zone*; do echo enabled > $z/mode; done
RUN
```
Expected: `DUTY_reg` scales with state (0 → 0x0, 10 → ~0xFA0=4000); with the user listening, the fan now audibly ramps proportionally (validated behavior at ~25 kHz). Carrier frequency check (optional, scope on AA11 or a fast userspace sampler): ~25 kHz.

- [ ] **Step 6: Commit + retire the Phase A software driver from the active DT**

```bash
cd /home/orencollaco/GitHub/zcu106-2023.2-axu3eg
git add project-spec/meta-user/recipes-modules/pl-pwm-fan project-spec/meta-user/conf/user-rootfsconfig project-spec/configs/rootfs_config
git commit -q -m "pl-pwm-fan: hardware-PWM cooling driver; proportional fan validated"
```
(Leave `soft-pwm-fan` in the tree as a documented fallback; the active DT now uses `user,axi-pwm-fan`.)

---

## Notes / risks

- **Bitstream deployment differs from Phase A:** Phase A only swapped `image.ub` over TFTP. Phase B changes the PL, so the new `system.bit` must reach the board via a rebuilt `BOOT.BIN` on the SD card (or JTAG `petalinux-boot --jtag --fpga`). Plan a power-cycle, not just a soft reboot.
- **TRD synthesis time:** the Alinx TRD is large (video/DDR4/MicroBlaze); a full impl run can take 30–60+ min. Run headless in the background.
- **`fan_tri_o` vector width:** if the top port is `fan_tri_o[0:0]`, keep it 1-bit so the existing XDC (`{fan_tri_o[0]}`) still matches; otherwise update the XDC.
- **PLCLK:** PERIOD assumes 100 MHz. Task 3 Step 1 records the real freq; set `pwm-period-cycles` and the DT accordingly (`PLCLK/25000`).
- **Address stability:** keeping `axi_pwm` at `0x80090000` means the driver base and any `devmem` debugging match Phase A muscle memory; only the register meaning changes.
