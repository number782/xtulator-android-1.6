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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "config.h"
#include "cpu/cpu.h"
#include "cpu/dynrec.h"
#include "modules/video/cga.h"
#include "modules/video/vga.h"
#include "utility.h"
#include "memory.h"
#include "diag.h"

uint8_t* memory_mapRead[MEMORY_RANGE];
uint8_t* memory_mapWrite[MEMORY_RANGE];
uint8_t (*memory_mapReadCallback[MEMORY_RANGE])(void* udata, uint32_t addr);
void (*memory_mapWriteCallback[MEMORY_RANGE])(void* udata, uint32_t addr, uint8_t value);
void* memory_udata[MEMORY_RANGE];

void cpu_write(CPU_t* cpu, uint32_t addr32, uint8_t value) {
	addr32 &= MEMORY_MASK;
	if (memory_mapWrite[addr32] != NULL) {
		*(memory_mapWrite[addr32]) = value;
		/* Invalidate any dynarec blocks that may have been translated
		 * from this address — writing to RAM can modify executable code
		 * (e.g. boot sector load, program loading, self-modifying code). */
		dynrec_invalidate_range(addr32, 1);
	}
	else if (memory_mapWriteCallback[addr32] != NULL) {
		void (*cb)(void*, uint32_t, uint8_t) = memory_mapWriteCallback[addr32];
		uint32_t cbaddr = (uint32_t)(uintptr_t)cb;
		/* Guard stale write-callback pointers, mirroring cpu_read(). A
		 * callback outside the libxtulator.so text region is a stale/corrupt
		 * slot and would BLX into unmapped memory (the 0x224 fault). Log it
		 * and drop the write so emulation can proceed to reveal the real
		 * fault. libxtulator.so is relocated at ~0x42000000; valid callbacks
		 * (cga_writememory, vga_writememory, ...) live in its text range. */
		if (cbaddr < 0x40000000u || cbaddr > 0x50000000u) {
			int fd = open("/sdcard/xt_memcb_dump.log",
			              O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd >= 0) {
				char b[224];
				int n = snprintf(b, sizeof(b),
					"BADWRITECALLBACK wr addr=0x%05X cb=0x%08X udata=%p\n",
					(unsigned)addr32, cbaddr, memory_udata[addr32]);
				write(fd, b, n);
				close(fd);
			}
			return;
		}
		(*cb)(memory_udata[addr32], addr32, value);
	}
}

uint8_t cpu_read(CPU_t* cpu, uint32_t addr32) {
	addr32 &= MEMORY_MASK;

	if (memory_mapRead[addr32] != NULL) {
		return *(memory_mapRead[addr32]);
	}

    if (memory_mapReadCallback[addr32] != NULL) {
        uint8_t (*cb)(void*, uint32_t) = memory_mapReadCallback[addr32];
        uint32_t cbaddr = (uint32_t)(uintptr_t)cb;
        /* A read-data callback that points outside this module's text is the
         * signature of the crash-on-first-BIOS-instruction fault: cpu_read
         * would BLX to cbaddr and fetch at an unmapped address. libxtulator.so
         * is relocated at ~0x42000000; anything far outside its text is almost
         * always a stale/MCorrupt callback slot. Log it and return open-bus
         * (0xFF) instead of calling through, so execution can proceed far
         * enough to reveal the real fault. */
        if (cbaddr < 0x40000000u || cbaddr > 0x50000000u) {
            int fd = open("/sdcard/xt_memcb_dump.log",
                          O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                char b[192];
                int n = snprintf(b, sizeof(b),
                    "BADCALLBACK rd addr=0x%05X cb=0x%08X udata=%p\n",
                    (unsigned)addr32, cbaddr, memory_udata[addr32]);
                write(fd, b, n);
                close(fd);
            }
            return 0xFF;
        }
        return (*cb)(memory_udata[addr32], addr32);
    }

	return 0xFF;
}

void memory_mapRegister(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb) {
	uint32_t i;
	DIAG("memory_mapRegister: start=0x%08X, len=0x%08X, readb=%p, writeb=%p", start, len, readb, writeb);
	for (i = 0; i < len; i++) {
		if ((start + i) >= MEMORY_RANGE) {
			break;
		}
		memory_mapRead[start + i] = (readb == NULL) ? NULL : readb + i;
		memory_mapWrite[start + i] = (writeb == NULL) ? NULL : writeb + i;
	}
}

void memory_mapCallbackRegister(uint32_t start, uint32_t count, uint8_t(*readb)(void*, uint32_t), void (*writeb)(void*, uint32_t, uint8_t), void* udata) {
	uint32_t i;
	for (i = 0; i < count; i++) {
		if ((start + i) >= MEMORY_RANGE) {
			break;
		}
		memory_mapReadCallback[start + i] = readb;
		memory_mapWriteCallback[start + i] = writeb;
		memory_udata[start + i] = udata;
	}
}

int memory_init() {
	uint32_t i;

	for (i = 0; i < MEMORY_RANGE; i++) {
		memory_mapRead[i] = NULL;
		memory_mapWrite[i] = NULL;
		memory_mapReadCallback[i] = NULL;
		memory_mapWriteCallback[i] = NULL;
		memory_udata[i] = NULL;
	}

	return 0;
}
