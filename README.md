# Petalinux development work for Alinx AXU3EG Ultrascle+ MPSoC FPGA board

##### Based of the ZCU106 BSP with board specific drivers and device tree modifications including:

1. Thermal driver for the SysMon features of the Zynq Processor Subsystem (PS) including thermal diodes (sensors) on the SoC as described in Zynq UltraScale+ Devices Register Reference ([UG1087](https://docs.xilinx.com/r/en-US/ug1087-zynq-ultrascale-registers/TEMP_FPD-PSSYSMON-Register)) [2022]

    Three temperature sensors have been tested:

        TEMP_FPD (PSSYSMON) -> FPD thermal diode remotely located near the APUs
        TEMP_PL (PLSYSMON) -> Thermal diode located in the PL SysMon unit
        TEMP_LPD (PSSYSMON) -> LPD thermal diode located in the PS SysMon unit near the RPU

    The sensors are registered in the thermal framework using thermal_of_zone_register() based on the device tree.
    The driver code is available [here](project-spec/meta-user/recipes-modules/zynqmp-pssysmon-temp/files/zynqmp-pssysmon-temp.c)

2. GPIO based fan driver to enable temperature based fan control using the Linux kernel thermal management framework. The fan gets registered as a cooling device in the thremal framework. The device tree adds it to thermal zones to trigger active cooling when temperature trip point is reached. Driver code is available [here](project-spec/meta-user/recipes-modules/simple-gpio-fan/files/simple-gpio-fan.c)

3. Device Tree modifications to support temperature based dynamic cooling. Device Tree modifications [here](project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi)