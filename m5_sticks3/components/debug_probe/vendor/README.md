Arm CMSIS-DAP reference firmware (Apache-2.0).
Source: https://github.com/ARM-software/CMSIS-DAP
Pinned commit: 12636590eec66fae2d1bba4518749426ad5a4595
DAP.c, DAP.h, SW_DP.c and JTAG_DP.c are derived from that revision; LICENSE is included.
Local changes: Xtensa cycle delays and frequency policy; unsigned decoding shifts;
SWJ pin wait masks; initialized transfer data on error paths; disabled atomic capability; cancellable SWD/JTAG transfers for bounded worker shutdown.
The upstream engine implements DP/AP posted reads, WAIT/FAULT, parity, match masks,
block transfers, SWJ selection, SWD/JTAG sequences and JTAG chain/IDCODE commands.
JTAG_DP.c retains the upstream bit shifter, with the same cancellation hook as
SW_DP.c. SWO/UART/atomic capabilities remain disabled. JTAG supports 8 TAPs with
1..32-bit IR lengths; the wrapper rejects zero lengths and oversized chains.
A separate checked packet parser bounds all input/output sizes.
