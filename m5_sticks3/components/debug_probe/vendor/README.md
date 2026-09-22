Arm CMSIS-DAP reference firmware (Apache-2.0).
Source: https://github.com/ARM-software/CMSIS-DAP
Pinned commit: 12636590eec66fae2d1bba4518749426ad5a4595
DAP.c, DAP.h and SW_DP.c are derived from that revision; LICENSE is included.
Local changes: Xtensa cycle delays and frequency policy; unsigned decoding shifts;
SWJ pin wait masks; initialized transfer data on error paths; disabled atomic capability; cancellable SWD transfers for bounded worker shutdown.
The upstream engine implements DP/AP posted reads, WAIT/FAULT, parity, match masks,
block transfers, SWJ selection and SWD sequences. JTAG/SWO/UART/atomic capabilities
are disabled. A separate checked packet parser bounds all input/output sizes.
