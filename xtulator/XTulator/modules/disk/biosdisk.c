/*
  XTulator: A portable, open-source 80186 PC emulator.
  Copyright (C)2020 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*

	NOTE: I consider HLE of the disk system at the BIOS level a hack.
	I want to get rid of this and implement proper FDC and HDC controllers.
	This is here for testing purposes of the rest of the system until
	I get around to that...

*/

#include <stdio.h>
#include <stdint.h>
#include "biosdisk.h"
#include "../../config.h"
#include "../../cpu/cpu.h"
#include "../../debuglog.h"
#include "../../diag.h"

#ifdef __ANDROID__
#include <android/log.h>
#define BIOSDISK_LOGI(...) do { if (trace_flags & TRACE_FLAG_DISK) { __android_log_print(ANDROID_LOG_INFO, "XTulator-BIOSDISK", __VA_ARGS__); } } while (0)
#else
#define BIOSDISK_LOGI(...)
#endif

DISK_t biosdisk[4];
uint8_t biosdisk_sectbuf[512];

uint8_t bootdrive = 0xFF;

uint8_t biosdisk_insert(CPU_t* cpu, uint8_t drivenum, char* filename) {
	BIOSDISK_LOGI("Inserting disk %u: %s", drivenum, filename);
	debug_log(DEBUG_INFO, "[BIOSDISK] Inserting disk %u: %s\r\n", drivenum, filename);
	if (biosdisk[drivenum].inserted) fclose(biosdisk[drivenum].diskfile);
	biosdisk[drivenum].inserted = 1;
	BIOSDISK_LOGI("Calling fopen with %s", filename);
	biosdisk[drivenum].diskfile = fopen(filename, "r+b");
	BIOSDISK_LOGI("fopen returned %p", biosdisk[drivenum].diskfile);
	if (biosdisk[drivenum].diskfile == NULL) {
		biosdisk[drivenum].inserted = 0;
		BIOSDISK_LOGI("Failed to insert disk %u: %s", drivenum, filename);
		debug_log(DEBUG_INFO, "[BIOSDISK] Failed to insert disk %u: %s\r\n", drivenum, filename);
		return 1;
	}
	BIOSDISK_LOGI("Calling fseek");
	fseek(biosdisk[drivenum].diskfile, 0L, SEEK_END);
	BIOSDISK_LOGI("Calling ftell");
	biosdisk[drivenum].filesize = ftell(biosdisk[drivenum].diskfile);
	BIOSDISK_LOGI("filesize = %u", biosdisk[drivenum].filesize);
	BIOSDISK_LOGI("After ftell, before fseek SEEK_SET");
	fseek(biosdisk[drivenum].diskfile, 0L, SEEK_SET);
	BIOSDISK_LOGI("After fseek SEEK_SET, before if drivenum");
	/* Determine geometry by image size, not drive number.
	   Floppy images have standard sizes; hard disk images are larger. */
	if (biosdisk[drivenum].filesize <= 1474560) { // 1.44MB floppy or smaller
		biosdisk[drivenum].cyls = 80;
		biosdisk[drivenum].sects = 18;
		biosdisk[drivenum].heads = 2;
		if (biosdisk[drivenum].filesize <= 1228800) biosdisk[drivenum].sects = 15;
		if (biosdisk[drivenum].filesize <= 737280) biosdisk[drivenum].sects = 9;
		if (biosdisk[drivenum].filesize <= 368640) {
			biosdisk[drivenum].cyls = 40;
			biosdisk[drivenum].sects = 9;
		}
		if (biosdisk[drivenum].filesize <= 163840) {
			biosdisk[drivenum].cyls = 40;
			biosdisk[drivenum].sects = 8;
			biosdisk[drivenum].heads = 1;
		}
		DIAG("BIOSDISK: INSERT drive=%u as FLOPPY size=%u geom=%ux%ux%u", drivenum, biosdisk[drivenum].filesize, biosdisk[drivenum].cyls, biosdisk[drivenum].heads, biosdisk[drivenum].sects);
	}
	else if (drivenum >= 2) { //it's a hard disk image (larger than floppy)
		biosdisk[drivenum].sects = 63;
		biosdisk[drivenum].heads = 16;
		biosdisk[drivenum].cyls = biosdisk[drivenum].filesize / (biosdisk[drivenum].sects * biosdisk[drivenum].heads * 512);
		DIAG("BIOSDISK: INSERT drive=%u as HDD size=%u geom=%ux%ux%u", drivenum, biosdisk[drivenum].filesize, biosdisk[drivenum].cyls, biosdisk[drivenum].heads, biosdisk[drivenum].sects);
		cpu_write(cpu, 0x475, biosdisk_gethdcount());
	}
	else { // floppy drive but image larger than 1.44MB - treat as large floppy
		biosdisk[drivenum].cyls = 80;
		biosdisk[drivenum].sects = 18;
		biosdisk[drivenum].heads = 2;
		DIAG("BIOSDISK: INSERT drive=%u as LARGE FLOPPY size=%u geom=%ux%ux%u", drivenum, biosdisk[drivenum].filesize, biosdisk[drivenum].cyls, biosdisk[drivenum].heads, biosdisk[drivenum].sects);
	}

	BIOSDISK_LOGI("biosdisk_insert: about to return 0");
	return 0;
}
void biosdisk_eject(CPU_t* cpu, uint8_t drivenum) {
	biosdisk[drivenum].inserted = 0;
	if (drivenum >= 2) {
		cpu_write(cpu, 0x475, biosdisk_gethdcount());
	}
	if (biosdisk[drivenum].diskfile != NULL) fclose(biosdisk[drivenum].diskfile);
}

uint8_t biosdisk_read(CPU_t* cpu, uint8_t drivenum, uint16_t dstseg, uint16_t dstoff, uint16_t cyl, uint16_t sect, uint16_t head, uint16_t sectcount) {
	uint32_t memdest, lba, fileoffset, cursect, sectoffset;
	static uint32_t dbg_count = 0;
	/* Log the first few requests and then every 64th: the FreeDOS boot sector
	   spins here forever when a read fails, so one line per attempt would be
	   thousands of lines per second. */
	uint8_t dbg = (dbg_count < 4) || ((dbg_count & 63) == 0);
	dbg_count++;

	if (!sect || !biosdisk[drivenum].inserted) {
		if (dbg) DIAG("BIOSDISK: REJECT drive=%u sect=%u inserted=%u (fileoffset not computed)", drivenum, sect, biosdisk[drivenum].inserted);
		return 1;
	}
	lba = ((uint32_t)cyl * (uint32_t)biosdisk[drivenum].heads + (uint32_t)head) * (uint32_t)biosdisk[drivenum].sects + (uint32_t)sect - 1UL;
	fileoffset = lba * 512UL;
	if (fileoffset > biosdisk[drivenum].filesize) {
		if (dbg) DIAG("BIOSDISK: PAST-EOF drive=%u chs=%u/%u/%u lba=%u offset=%u filesize=%u", drivenum, cyl, head, sect, lba, fileoffset, biosdisk[drivenum].filesize);
		return 1;
	}
	
	/* Always log boot sector reads (cyl=0, head=0, sect=1) */
	if (cyl == 0 && head == 0 && sect == 1) {
		DIAG("BIOSDISK: BOOT SECTOR READ drive=%u chs=%u/%u/%u count=%u -> %04X:%04X lba=%u filesize=%u geom=%ux%ux%u",
			drivenum, cyl, head, sect, sectcount, dstseg, dstoff, lba, biosdisk[drivenum].filesize,
			biosdisk[drivenum].cyls, biosdisk[drivenum].heads, biosdisk[drivenum].sects);
	}
	
	fseek(biosdisk[drivenum].diskfile, fileoffset, SEEK_SET);
	memdest = ((uint32_t)dstseg << 4) + (uint32_t)dstoff;
	for (cursect = 0; cursect < sectcount; cursect++) {
		if (fread(biosdisk_sectbuf, 1, 512, biosdisk[drivenum].diskfile) < 512) break;
		for (sectoffset = 0; sectoffset < 512; sectoffset++) {
			cpu_write(cpu, memdest++, biosdisk_sectbuf[sectoffset]);
		}
	}
	if (dbg) DIAG("BIOSDISK: read drive=%u chs=%u/%u/%u count=%u -> %04X:%04X lba=%u read=%u/%u geom=%ux%ux%u size=%u",
		drivenum, cyl, head, sect, sectcount, dstseg, dstoff, lba, cursect, sectcount,
		biosdisk[drivenum].cyls, biosdisk[drivenum].heads, biosdisk[drivenum].sects,
		biosdisk[drivenum].filesize);
	cpu->regs.byteregs[regal] = cursect;
	cpu->cf = 0;
	cpu->regs.byteregs[regah] = 0;
	return 0;
}

uint8_t biosdisk_write(CPU_t* cpu, uint8_t drivenum, uint16_t dstseg, uint16_t dstoff, uint16_t cyl, uint16_t sect, uint16_t head, uint16_t sectcount) {
	uint32_t memdest, lba, fileoffset, cursect, sectoffset;
	if (!sect || !biosdisk[drivenum].inserted) return 1;
	lba = ((uint32_t)cyl * (uint32_t)biosdisk[drivenum].heads + (uint32_t)head) * (uint32_t)biosdisk[drivenum].sects + (uint32_t)sect - 1UL;
	fileoffset = lba * 512UL;
	if (fileoffset > biosdisk[drivenum].filesize) return 1;
	fseek(biosdisk[drivenum].diskfile, fileoffset, SEEK_SET);
	memdest = ((uint32_t)dstseg << 4) + (uint32_t)dstoff;
	for (cursect = 0; cursect < sectcount; cursect++) {
		for (sectoffset = 0; sectoffset < 512; sectoffset++) {
			biosdisk_sectbuf[sectoffset] = cpu_read(cpu, memdest++);
		}
		fwrite(biosdisk_sectbuf, 1, 512, biosdisk[drivenum].diskfile);
	}
	cpu->regs.byteregs[regal] = (uint8_t)sectcount;
	cpu->cf = 0;
	cpu->regs.byteregs[regah] = 0;
	return 0;
}

void biosdisk_int19h(CPU_t* cpu, uint8_t intnum) {
	if (intnum != 0x19) return;
	
	cpu_write(cpu, 0x475, biosdisk_gethdcount());

	//put "STI" and then "JMP -1" code at bootloader location in case nothing gets read from disk
	cpu_write(cpu, 0x07C00, 0xFB);
	cpu_write(cpu, 0x07C01, 0xEB);
	cpu_write(cpu, 0x07C02, 0xFE);

	cpu->regs.byteregs[regdl] = bootdrive;
	biosdisk_read(cpu, (cpu->regs.byteregs[regdl] & 0x80) ? cpu->regs.byteregs[regdl] - 126 : cpu->regs.byteregs[regdl], 0x0000, 0x7C00, 0, 1, 0, 1);
	cpu->segregs[regcs] = 0x0000;
	cpu->ip = 0x7C00;
}

void biosdisk_int13h(CPU_t* cpu, uint8_t intnum) {
	static uint8_t lastah = 0, lastcf = 0;
	uint8_t curdisk;

	if (intnum != 0x13) return;

	curdisk = cpu->regs.byteregs[regdl];
	if (curdisk & 0x80) curdisk = curdisk - 126;
	
	/* Log all int13h calls */
	DIAG("BIOSDISK: int13h AH=%02X DL=%02X (drive=%u) CH=%02X CL=%02X DH=%02X ES:BX=%04X:%04X",
		cpu->regs.byteregs[regah], cpu->regs.byteregs[regdl], curdisk,
		cpu->regs.byteregs[regch], cpu->regs.byteregs[regcl], cpu->regs.byteregs[regdh],
		cpu->segregs[reges], getreg16(cpu, regbx));

	if (curdisk > 3) {
		cpu->cf = 1;
		cpu->regs.byteregs[regah] = 1;
		return;
	}

	switch (cpu->regs.byteregs[regah]) {
	case 0: //reset disk system
		cpu->regs.byteregs[regah] = 0;
		cpu->cf = 0; //useless function in an emulator. say success and return.
		break;
	case 1: //return last status
		cpu->regs.byteregs[regah] = lastah;
		cpu->cf = lastcf;
		return;
	case 2: //read sector(s) into memory
		if (biosdisk[curdisk].inserted) {
			if (biosdisk_read(cpu, curdisk, cpu->segregs[reges], getreg16(cpu, regbx), (uint16_t)cpu->regs.byteregs[regch] + ((uint16_t)cpu->regs.byteregs[regcl] / 64) * 256, (uint16_t)cpu->regs.byteregs[regcl] & 63, (uint16_t)cpu->regs.byteregs[regdh], (uint16_t)cpu->regs.byteregs[regal])) {
				cpu->cf = 1;
				cpu->regs.byteregs[regah] = 1;
			}
		}
		else {
			cpu->cf = 1;
			cpu->regs.byteregs[regah] = 1;
		}
		break;
	case 3: //write sector(s) from memory
		if (biosdisk[curdisk].inserted) {
			if (biosdisk_write(cpu, curdisk, cpu->segregs[reges], getreg16(cpu, regbx), (uint16_t)cpu->regs.byteregs[regch] + ((uint16_t)cpu->regs.byteregs[regcl] / 64) * 256, (uint16_t)cpu->regs.byteregs[regcl] & 63, (uint16_t)cpu->regs.byteregs[regdh], (uint16_t)cpu->regs.byteregs[regal])) {
				cpu->cf = 1;
				cpu->regs.byteregs[regah] = 1;
			}
		}
		else {
			cpu->cf = 1;
			cpu->regs.byteregs[regah] = 1;
		}
		break;
	case 4:
	case 5: //format track
		cpu->cf = 0;
		cpu->regs.byteregs[regah] = 0;
		break;
	case 8: //get drive parameters
		if (biosdisk[curdisk].inserted) {
			cpu->cf = 0;
			cpu->regs.byteregs[regah] = 0;
			cpu->regs.byteregs[regch] = biosdisk[curdisk].cyls - 1;
			cpu->regs.byteregs[regcl] = biosdisk[curdisk].sects & 63;
			cpu->regs.byteregs[regcl] = cpu->regs.byteregs[regcl] + (biosdisk[curdisk].cyls / 256) * 64;
			cpu->regs.byteregs[regdh] = biosdisk[curdisk].heads - 1;
			if (curdisk < 2) {
				cpu->regs.byteregs[regbl] = 4; //else regs.byteregs[regbl] = 0;
				cpu->regs.byteregs[regdl] = 2;
			}
			else cpu->regs.byteregs[regdl] = biosdisk_gethdcount();
		}
		else {
			cpu->cf = 1;
			cpu->regs.byteregs[regah] = 0xAA;
		}
		break;
	default:
		cpu->cf = 1;
	}
	lastah = cpu->regs.byteregs[regah];
	lastcf = cpu->cf;
	DIAG("BIOSDISK: int13h RET AH=%02X CF=%u", cpu->regs.byteregs[regah], cpu->cf);
	BIOSDISK_LOGI("biosdisk_gethdcount: start");
	if (cpu->regs.byteregs[regdl] & 0x80) cpu_write(cpu, 0x474, cpu->regs.byteregs[regah]);
}

uint8_t biosdisk_gethdcount() {
	uint8_t ret = 0, i;
	BIOSDISK_LOGI("biosdisk_gethdcount: returning %d", ret);

	for (i = 2; i < 4; i++) {
		if (biosdisk[i].inserted) ret++;
	}
	return ret;
}

void biosdisk_init(CPU_t* cpu) {
	cpu_registerIntCallback(cpu, 0x13, (void (*)(void*, uint8_t))biosdisk_int13h);
	cpu_registerIntCallback(cpu, 0x19, (void (*)(void*, uint8_t))biosdisk_int19h);
}
