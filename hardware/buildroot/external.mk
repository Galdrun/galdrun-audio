include $(sort $(wildcard $(BR2_EXTERNAL_RAUMFELD_PATH)/package/*/*.mk))

ifeq ($(BR2_LINUX_KERNEL_INTREE_DTS_NAME),"ti/omap/am335x-raumfeld-connector2")
define RAUMFELD_CONNECTOR2_LINUX_PATCH
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_RAUMFELD_PATH)/board/connector2/raumfeld-connector2.c $(@D)/sound/soc/ti/raumfeld-connector2.c
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_RAUMFELD_PATH)/board/connector2/firmware/am335x-pm-firmware.elf $(@D)/firmware/am335x-pm-firmware.elf
	grep -q SND_SOC_RAUMFELD_CONNECTOR2 $(@D)/sound/soc/ti/Kconfig || \
		printf '\nconfig SND_SOC_RAUMFELD_CONNECTOR2\n\ttristate "Custom Connector 2 audio"\n\tdepends on SND_SOC && SOC_AM33XX\n\tselect SND_SOC_DAVINCI_MCASP\n\tselect SND_SOC_CS4271_I2C\n\tselect SND_SOC_AK4104\n' >> $(@D)/sound/soc/ti/Kconfig
	grep -q raumfeld-connector2.o $(@D)/sound/soc/ti/Makefile || \
		printf '\nobj-$$(CONFIG_SND_SOC_RAUMFELD_CONNECTOR2) += raumfeld-connector2.o\n' >> $(@D)/sound/soc/ti/Makefile
endef
LINUX_POST_PATCH_HOOKS += RAUMFELD_CONNECTOR2_LINUX_PATCH
endif

ifeq ($(BR2_LINUX_KERNEL_INTREE_DTS_NAME),"nxp/imx/imx7d-galdrun-connector3-ram")
define GALDRUN_CONNECTOR3_LINUX_PATCH
	grep -q SND_SOC_GALDRUN_CONNECTOR3 $(@D)/sound/soc/fsl/Kconfig || \
		printf '\nconfig SND_SOC_GALDRUN_CONNECTOR3\n\tbool "Galdrun Connector 3 audio"\n\tdepends on SND_SOC && SOC_IMX7D\n\tselect SND_SOC_FSL_SAI\n\tselect SND_SOC_SPDIF\n' >> $(@D)/sound/soc/fsl/Kconfig
	grep -q galdrun-connector3-audio.o $(@D)/sound/soc/fsl/Makefile || \
		printf '\nobj-$$(CONFIG_SND_SOC_GALDRUN_CONNECTOR3) += galdrun-connector3-audio.o\n' >> $(@D)/sound/soc/fsl/Makefile
endef
LINUX_POST_PATCH_HOOKS += GALDRUN_CONNECTOR3_LINUX_PATCH
define GALDRUN_CONNECTOR3_AUDIO_SOURCE
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_RAUMFELD_PATH)/board/connector3/ram/galdrun-connector3-audio.c $(@D)/sound/soc/fsl/galdrun-connector3-audio.c
endef
LINUX_PRE_BUILD_HOOKS += GALDRUN_CONNECTOR3_AUDIO_SOURCE
endif

# The USB installer's initramfs goes in uncompressed: the XZ kernel compresses it
# with everything else. Only settable after Buildroot names the initramfs source.
ifeq ($(BR2_TARGET_ROOTFS_INITRAMFS),y)
define RAUMFELD_INSTALLER_INITRAMFS_FIXUP
	$(call KCONFIG_DISABLE_OPT,CONFIG_INITRAMFS_COMPRESSION_GZIP)
	$(call KCONFIG_ENABLE_OPT,CONFIG_INITRAMFS_COMPRESSION_NONE)
endef
LINUX_KCONFIG_FIXUP_CMDS += $(RAUMFELD_INSTALLER_INITRAMFS_FIXUP)
endif
