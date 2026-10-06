/* AICAflow's ARM7 driver (deps/AICAflow/firmware/aicaflow.drv), for dc_aica.c */
	.section .rodata
	.balign 32
	.global _aica_firmware
_aica_firmware:
	.incbin AICA_DRV
	.global _aica_firmware_end
_aica_firmware_end:
	.balign 4
