# Phase B (hardware PWM fan) — progress ledger

Plan: docs/superpowers/plans/2026-07-01-hardware-pwm-fan-phase-b.md
IP repo: /home/orencollaco/GitHub/axu3eg-pwm-ip (separate)

Execution: Tasks 1-2 subagent-driven; Tasks 3-5 inline (hardware-in-the-loop).

- Task 1 (pwm_core VHDL + sim): complete (commits b7f2408..f320e1d, sim PWM_CORE_TEST_PASS via xsim, review clean)
- Task 2 (package axi_pwm AXI4-Lite IP): complete (commit a83e930; OOC synth 0 errors, pwm_out OUT + pwm_core + AXI slave in netlist; BD-validate ok). NOTE: Vivado 2023.2 has no VHDL peripheral template (Verilog-only generate) -> VHDL hand-written; inert .v scaffolds left on disk but NOT referenced in component.xml (all file entries vhdlSource) so synthesis uses VHDL.
- Task 3 (swap fan_gpio->axi_pwm in TRD): COMPLETE. Build ~6 min (OOC cache), TIMING MET (WNS +0.048). XSA=/home/orencollaco/GitHub/AlinxMigrated/axu3eg_trd.xsa (fan_pwm+fan_tri_o present, fan_gpio gone). incremental_ref_routed.dcp saved (future builds ~fast). GOTCHA fixed: incremental ref must be OUTSIDE run dir, and clear stale incremental_checkpoint property before a full build.
- Task 4 (re-import XSA + device tree): COMPLETE. DTG node fan_pwm@0x80090000 (compatible xlnx,axi-pwm-1.0, clk misc_clk_0=200MHz). system-user.dtsi: &fan_pwm override (user,axi-pwm-fan) + thermal maps repointed. DT compiles.
  - HW repo: /home/orencollaco/GitHub/AlinxMigrated git-initialized as "zcu106-axu3eg-hardware" (dir kept to preserve build cache); main=stock baseline (f0fbf56), branch hardware-pwm-fan; BD swap = 64a691f.
  - Facts: PL clock 200 MHz -> PWM period 8000 for 25 kHz. fan on ps8_0_axi_periph/M02_AXI, reset rst_ps8_0_149M. fan_tri_o plain 1-bit port [0:0] (was GPIO interface port /fan). addr 0x80090000/64K. axi_pwm seg = S00_AXI_reg.
  - Backups: design_1.bd.pre-pwm-backup, system.xdc.pre-pwm-backup (gitignored; git is now the real safety net).
  - Build: incremental ref DCP present; Flow_RuntimeOptimized; -jobs 16; output -> AlinxMigrated/axu3eg_trd.xsa.
- Task 4 (re-import XSA + device tree): pending (inline)
- Task 5 (pl-pwm-fan driver + on-board verify): COMPLETE + VALIDATED. Driver uses cooling-levels (per-mille) table. Tuned curve (off<50C,7% floor,->100% by 94C,2C hyst). On board: driver probes, DUTY reg matches each state, fan off at idle 41C, 7% self-starts (confirmed by ear). Persistent U-Boot bootcmd (fpga load system.bit.bin + image.ub) saved -> reboots auto-deploy over TFTP. PHASE B DONE.
