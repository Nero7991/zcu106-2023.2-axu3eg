# Petalinux development work for Alinx AXU3EG Ultrascle+ MPSoC FPGA board

##### Based of the ZCU106 BSP with board specific drivers and device tree modifications including:

1. Thermal driver for the SysMon features of the Zynq Processor Subsystem (PS) including thermal diodes (sensors) on the SoC as described in Zynq UltraScale+ Devices Register Reference ([UG1087](https://docs.xilinx.com/r/en-US/ug1087-zynq-ultrascale-registers/TEMP_FPD-PSSYSMON-Register)) [2022]

    Three temperature sensors have been tested:

        TEMP_FPD (PSSYSMON) -> FPD thermal diode remotely located near the APUs
        TEMP_PL (PLSYSMON) -> Thermal diode located in the PL SysMon unit
        TEMP_LPD (PSSYSMON) -> LPD thermal diode located in the PS SysMon unit near the RPU

    The sensors are registered in the thermal framework using thermal_of_zone_register() based on the device tree.
    The driver code is available [here](project-spec/meta-user/recipes-modules/zynqmp-pssysmon-temp/files/zynqmp-pssysmon-temp.c)

2. Temperature based fan control using the Linux kernel thermal management framework. The fan is registered as a cooling device; the device tree adds it to the thermal zones so active cooling is driven by temperature trip points. This evolved through three stages:

    a. **On/off (initial)** - a GPIO cooling driver toggling the fan through a 1-bit AXI GPIO. Two states only. Driver code [here](project-spec/meta-user/recipes-modules/simple-gpio-fan/files/simple-gpio-fan.c)

    b. **Software PWM (interim)** - a multi-state cooling driver that drives the same GPIO line with an hrtimer soft-PWM (~100 Hz). It proved the multi-level thermal path in software, but the board's 2-wire brushless fan does not speed-control at 100 Hz (it stalls below a threshold and runs near full above it), so this stays as a fallback only. Driver code [here](project-spec/meta-user/recipes-modules/soft-pwm-fan/files/soft-pwm-fan.c)

    c. **Hardware PWM (current)** - a custom AXI4-Lite PWM peripheral (`axi_pwm`, VHDL) added to the PL generates a 25 kHz PWM on the fan pin (package AA11), at which the fan speed-controls smoothly. The `pl-pwm-fan` driver writes the peripheral's duty register from a device-tree `cooling-levels` table, registered as a thermal cooling device. Tuned by ear to a quiet curve: a silent, self-starting 7% idle floor that ramps to full only when the die actually gets hot. Driver code [here](project-spec/meta-user/recipes-modules/pl-pwm-fan/files/pl-pwm-fan.c). The reusable PWM IP and the `design_1` block-design change live in the companion `zcu106-axu3eg-hardware` repo; implementation notes are in the [Phase B plan](docs/superpowers/plans/2026-07-01-hardware-pwm-fan-phase-b.md)

3. Device Tree modifications to support temperature based dynamic cooling - the SysMon sensor nodes, the `fan_pwm` cooling device, and the per-zone thermal trip staircase / `cooling-levels` fan curve. Device Tree modifications [here](project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi)

4. `axu-mon` - a small live sensor monitor (htop-style) that continuously shows the three SoC temperatures, the fan cooling state and live PWM duty/frequency read from the `axi_pwm` peripheral, plus CPU load and memory. Run `axu-mon` on the board (`q` to quit). Script [here](project-spec/meta-user/recipes-apps/axu-mon/files/axu-mon)
