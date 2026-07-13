/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2025 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the CC-BY-ND as described in the
 * included LICENSE.CC-BY-ND file.  This file may be modified for
 * personal use, but modified copies MAY NOT be redistributed in any form.
 */
 
 
 /* 
  * This works in conjuction with the BBShiftPanel.asm file to handle the OE line.
  * By offloading that to the second PRU, the timing of the OE line can be much 
  * more exact providing smoother display.
  */
  
#include "ResourceTable.asm"


#if !defined(RUNNING_ON_PRU0) && !defined(RUNNING_ON_PRU1)
#define RUNNING_ON_PRU0
#endif


#include "FalconUtils.asm"
#include "FalconPRUDefs.hp"
#include "SMEMRing.hp"

#define OE_PIN      0

#define curBright   r25
#define extraWait   r26

#define tmpReg1     r28
#define tmpReg2     r29

#ifdef PRU0_PREFETCH
// PRU0 owns consumption of the ARM->PRU shared-memory ring for the
// two-PRU, 16-output shift-panel firmware.  It keeps one 48-byte (four
// pixel) block ahead of PRU1 in scratchpad bank 11.  PRU1 acknowledges a
// block via bank 10 as soon as XIN has copied it into its registers, allowing
// this PRU to fetch the following block while PRU1 shifts the current one.
#define prefetchAck r14
// r1-r13 are deliberately contiguous: scratchpad bank 11 carries sequence
// in r1 followed immediately by the 48-byte block in r2-r13.
#define prefetchSeq r1
#define ringReadPtr r16
#define ringBase    r17
#define availBytes  r18
#define consumedCnt r19
#define ringCtrl    r20
#define prefetchData r2

PREFETCH_NEXT .macro
    .newblock
    XIN   10, &prefetchAck, 4
    // Acknowledge must match the published sequence before bank 11 can be
    // overwritten.  This makes the single scratchpad block safe even when
    // PRU1 is delayed at a row boundary.
    QBNE  DONE?, prefetchAck, prefetchSeq
    QBLE  HAVEDATA?, availBytes, 48
    LBBO  &tmpReg1, ringCtrl, 0, 4
    SUB   availBytes, tmpReg1, consumedCnt
    QBGT  DONE?, availBytes, 48
HAVEDATA?:
    LBBO  &prefetchData, ringReadPtr, 0, 48
    ADD   ringReadPtr, ringReadPtr, 48
    ADD   consumedCnt, consumedCnt, 48
    SUB   availBytes, availBytes, 48
    SBBO  &consumedCnt, ringCtrl, 4, 4
    QBNE  NOWRAP?, ringReadPtr, ringCtrl
    MOV   ringReadPtr, ringBase
NOWRAP?:
    ADD   prefetchSeq, prefetchSeq, 1
    XOUT  11, &prefetchSeq, 52
DONE?:
    .endm
#endif

DISPLAY_OFF .macro
    SET r30, r30, OE_PIN
    .endm

DISPLAY_ON .macro
    CLR r30, r30, OE_PIN
    .endm



;*****************************************************************************
;                                  Main Loop
;*****************************************************************************
    .sect    ".text:main"
    .global    ||main||
||main||:
	// Configure the programmable pointer register for PRU by setting
	// c28_pointer[15:0] field to 0x0120.  This will make C28 point to
	// 0x00012000 (PRU shared RAM).
	//MOV	r0, 0x00000120
	//MOV	r1, CTPPR_0 + PRU_MEMORY_OFFSET
	//SBBO    &r0, r1, 0x00, 4

	// Configure the programmable pointer register for PRU by setting
	// c31_pointer[15:0] field to 0x0010.  This will make C31 point to
	// 0x80001000 (DDR memory).
	LDI32	r0, 0x00100000
	LDI32	r1, CTPPR_1 + PRU_MEMORY_OFFSET
    SBBO    &r0, r1, 0x00, 4

    // Make sure the brightness is clear at start
	LDI 	curBright, 0x0
	LDI		extraWait, 0x0
	XOUT 	12, &curBright, 8

#ifdef PRU0_PREFETCH
    // The ARM publishes this configuration in PRU1 data RAM after both
    // firmwares are running.  PRU0 owns the read counter in the ring; PRU1
    // only consumes the scratchpad copy.
RINGCFGWAIT:
    LDI   tmpReg2, SMEM_RING_CONFIG_OFFSET
    LBCO  &ringBase, CONST_OTHERPRUDRAM, tmpReg2, 8
    QBEQ  RINGCFGWAIT, availBytes, 0
    MOV   ringReadPtr, ringBase
    ADD   ringCtrl, ringBase, availBytes
    LDI   consumedCnt, 0
    LDI   availBytes, 0
    SBBO  &consumedCnt, ringCtrl, 4, 4
    LDI   prefetchAck, 0
    LDI   prefetchSeq, 0
    // The scratchpad banks survive firmware reloads: clear the published
    // sequence first so PRU1 (which expects sequence one) can never consume
    // a stale block from a previous run, then clear the acknowledgement.
    // Note PRU1 enables the PRUSS-wide XIN/XOUT shift feature, so r0.b0
    // must stay zero here as well (it is - the CTPPR setup leaves it 0).
    XOUT  11, &prefetchSeq, 4
    XOUT  10, &prefetchAck, 4
    // Prime the first block before PRU1 begins its first stride.
    PREFETCH_NEXT
#endif
  
	// Wait for the start condition from the main program to indicate
	// that we have a rendered frame ready to clock out.  This also
	// handles the exit case if an invalid value is written to the start
	// start position.
_LOOP:
    // make sure the display is off
    DISPLAY_OFF

#ifdef PRU0_PREFETCH
    // PRU1 shifts its first stride before it sends the first OE command, so
    // keep the one-block queue primed while waiting for that command too.
    PREFETCH_NEXT
#endif
	XIN 	12, &curBright, 8
	// Wait for a non-zero brightness
	QBEQ	_LOOP, curBright, 0

	// Command of 0xFFFFFFFF is the signal to exit
    LDI32     tmpReg1, 0xFFFFFFFF
	QBNE	DOOUTPUT, curBright, tmpReg1
    JMP     EXIT

DOOUTPUT:
#ifdef PRU0_PREFETCH
    // The timer lets this PRU prefetch during OE-on without changing the
    // requested brightness.  Do not begin a new shared-RAM transfer in the
    // last 64 cycles; short bit planes retain their original precise timing.
    RESET_PRU_CLOCK tmpReg1, tmpReg2
#endif
    // turn the display on
    DISPLAY_ON

ONLOOP:
#ifdef PRU0_PREFETCH
    GET_PRU_CLOCK tmpReg1, tmpReg2, 4
    QBGT    ONWORK, tmpReg1, curBright
    QBA     ONDONE
ONWORK:
    SUB     tmpReg2, curBright, tmpReg1
    QBLT    ONWORKFETCH, tmpReg2, 64
ONTAIL:
    GET_PRU_CLOCK tmpReg1, tmpReg2, 4
    QBGT    ONTAIL, tmpReg1, curBright
    QBA     ONDONE
ONWORKFETCH:
    PREFETCH_NEXT
    QBA     ONLOOP
ONDONE:
#else
    SUB     curBright, curBright, 2
    QBLT    ONLOOP, curBright, 1
#endif

    DISPLAY_OFF

#ifdef PRU0_PREFETCH
    // one refill after the display turns off; it must stay out of the loop
    // below, whose 3-cycle iterations are what calibrates extraWait
    PREFETCH_NEXT
#endif
OFFLOOP:
	QBGT	NOTIFY, extraWait.w0, 3
	SUB		extraWait.w0, extraWait.w0, 3
	JMP		OFFLOOP

NOTIFY:
    LDI curBright, 0
	XOUT 12, &curBright, 4

    JMP _LOOP

EXIT:
	// Send notification to Host for program completion
	LDI R31.b0, PRU_ARM_INTERRUPT+16
	HALT
