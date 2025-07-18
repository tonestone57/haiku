/*
 * Copyright 2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT license.
 *
 * Author:
 *		François Revol, revol@free.fr.
 *
 */

.text

.globl ColdReboot
ColdReboot:
	move.l	_EXEC_BASE_NAME, %a6
	jsr		(%a6, -0x2d6)
	rts

.globl CloseLibrary
CloseLibrary:
	move.l	_EXEC_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	jsr		(%a6, -0x19e)
	rts

.globl CloseDevice
CloseDevice:
	move.l	_EXEC_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	jsr		(%a6, -0x1c2)
	rts

.globl DeleteIORequest
DeleteIORequest:
	move.l	_EXEC_BASE_NAME, %a6
	move.l	%sp@(4), %a0
	jsr		(%a6, -0x294)
	rts

.globl ClearScreen
ClearScreen:
	move.l	_GRAPHICS_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	jsr		(%a6, -0x30)
	rts

.globl LoadRGB4
LoadRGB4:
	move.l	_GRAPHICS_BASE_NAME, %a6
	move.l	%sp@(4), %a0
	move.l	%sp@(8), %a1
	move.l	%sp@(12), %d0
	jsr		(%a6, -0xc0)
	rts

.globl Move
Move:
	move.l	_GRAPHICS_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	move.l	%sp@(8), %d0
	move.l	%sp@(12), %d1
	jsr		(%a6, -0xf0)
	rts

.globl SetAPen
SetAPen:
	move.l	_GRAPHICS_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	move.l	%sp@(8), %d0
	jsr		(%a6, -0x156)
	rts

.globl SetBPen
SetBPen:
	move.l	_GRAPHICS_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	move.l	%sp@(8), %d0
	jsr		(%a6, -0x15c)
	rts

.globl SetDrMd
SetDrMd:
	move.l	_GRAPHICS_BASE_NAME, %a6
	move.l	%sp@(4), %a1
	move.l	%sp@(8), %d0
	jsr		(%a6, -0x162)
	rts

.globl QueryKeys
QueryKeys:
	move.l	_LOWLEVEL_BASE_NAME, %a6
	move.l	%sp@(4), %a0
	move.l	%sp@(8), %d1
	jsr		(%a6, -0x36)
	rts
