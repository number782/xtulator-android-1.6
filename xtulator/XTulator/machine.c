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
	Machine definitions.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "config.h"
#include "debuglog.h"
#include "cpu/cpu.h"
#include "chipset/i8259.h"
#include "chipset/i8253.h"
#include "chipset/i8237.h"
#include "chipset/i8255.h"
#include "chipset/uart.h"
#include "modules/audio/pcspeaker.h"
#include "modules/audio/opl2.h"
#include "modules/audio/blaster.h"
#include "modules/disk/biosdisk.h"
#include "modules/disk/fdc.h"
#include "modules/input/mouse.h"
#include "modules/input/input.h"
#ifdef USE_NE2000
#include "modules/io/ne2000.h"
#include "modules/io/pcap-win32.h"
#endif
#include "modules/io/tcpmodem.h"
#include "modules/video/cga.h"
#include "modules/video/vga.h"
#include "rtc.h"
#include "memory.h"
#include "utility.h"
#include "timing.h"
#include "machine.h"
#include "machine.h"
#include "diag.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#define MACHINE_LOGI(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_INFO, "XTulator-MACHINE", __VA_ARGS__); } } while (0)
#else
#define MACHINE_LOGI(...)
#endif


/*
	ID string, full description, init function, default video, speed in MHz (-1 = unlimited), default hardware flags
*/
const MACHINEDEF_t machine_defs[] = {
	{ "generic_xt", "Generic XT clone with VGA, speed unlimited", machine_init_generic_xt, VIDEO_CARD_VGA, -1, MACHINE_HW_BLASTER | MACHINE_HW_UART1_MOUSE | MACHINE_HW_DISK_HLE | MACHINE_HW_RTC },
	{ "ibm_xt", "IBM XT", machine_init_generic_xt, VIDEO_CARD_CGA, 4.77, MACHINE_HW_UART1_MOUSE | MACHINE_HW_RTC },
	{ "ami_xt", "AMI XT clone", machine_init_generic_xt, VIDEO_CARD_CGA, 4.77, MACHINE_HW_UART1_MOUSE | MACHINE_HW_RTC },
	{ "phoenix_xt", "Pheonix XT clone", machine_init_generic_xt, VIDEO_CARD_CGA, 4.77, MACHINE_HW_UART1_MOUSE | MACHINE_HW_RTC },
	{ "xi8088", "Xi 8088", machine_init_generic_xt, VIDEO_CARD_CGA, 4.77, MACHINE_HW_UART1_MOUSE | MACHINE_HW_RTC },
	{ "zenithss", "Zenith SuperSport 8088", machine_init_generic_xt, VIDEO_CARD_CGA, 4.77, MACHINE_HW_UART1_MOUSE | MACHINE_HW_RTC },
	{ "landmark", "Supersoft/Landmark diagnostic ROM", machine_init_generic_xt, VIDEO_CARD_CGA, 4.77, MACHINE_HW_UART1_MOUSE | MACHINE_HW_RTC },
	{ NULL }
};

const MACHINEMEM_t machine_mem[][10] = {
//Generic XT clone
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
#ifndef USE_DISK_HLE
		{ MACHINE_MEM_ROM, 0xD0000, 0x02000, MACHINE_ROM_REQUIRED, "roms/disk/ide_xt.bin" },
#endif
		{ MACHINE_MEM_ROM, 0xFE000, 0x02000, MACHINE_ROM_REQUIRED, "roms/machine/generic_xt/pcxtbios.bin" },
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//IBM XT
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
#ifndef USE_DISK_HLE
		{ MACHINE_MEM_ROM, 0xD0000, 0x02000, MACHINE_ROM_REQUIRED, "roms/disk/ide_xt.bin" },
#endif
		{ MACHINE_MEM_ROM, 0xF0000, 0x08000, MACHINE_ROM_REQUIRED, "roms/machine/ibm_xt/5000027.u19" },
		{ MACHINE_MEM_ROM, 0xF8000, 0x08000, MACHINE_ROM_REQUIRED, "roms/machine/ibm_xt/1501512.u18" },
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//AMI XT clone
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
#ifndef USE_DISK_HLE
		{ MACHINE_MEM_ROM, 0xD0000, 0x02000, MACHINE_ROM_REQUIRED, "roms/disk/ide_xt.bin" },
#endif
		{ MACHINE_MEM_ROM, 0xFE000, 0x02000, MACHINE_ROM_REQUIRED, "roms/machine/ami_xt/ami_8088_bios_31jan89.bin" },
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//Phoenix XT clone
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
#ifndef USE_DISK_HLE
		{ MACHINE_MEM_ROM, 0xD0000, 0x02000, MACHINE_ROM_REQUIRED, "roms/disk/ide_xt.bin" },
#endif
		{ MACHINE_MEM_ROM, 0xFE000, 0x02000, MACHINE_ROM_REQUIRED, "roms/machine/phoenix_xt/000p001.bin" },
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//Xi 8088
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
#ifndef USE_DISK_HLE
		{ MACHINE_MEM_ROM, 0xD0000, 0x02000, MACHINE_ROM_REQUIRED, "roms/disk/ide_xt.bin" },
#endif
		{ MACHINE_MEM_ROM, 0xF0000, 0x10000, MACHINE_ROM_REQUIRED, "roms/machine/xi8088/bios128k-2.0.bin" }, //last half of this ROM is just filler for the 128k chip...
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//Zenith SuperSport 8088
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
#ifndef USE_DISK_HLE
		{ MACHINE_MEM_ROM, 0xD0000, 0x02000, MACHINE_ROM_REQUIRED, "roms/disk/ide_xt.bin" },
#endif
		{ MACHINE_MEM_RAM, 0xF0000, 0x04000, MACHINE_ROM_ISNOTROM, NULL }, //scratchpad RAM
		{ MACHINE_MEM_ROM, 0xF8000, 0x08000, MACHINE_ROM_REQUIRED, "roms/machine/zenithss/z184m v3.1d.10d" },
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//Supersoft/Landmark diagnostic
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
		{ MACHINE_MEM_ROM, 0xF8000, 0x08000, MACHINE_ROM_REQUIRED, "roms/machine/landmark/landmark.bin" },
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},
};

uint8_t mac[6] = { 0xac, 0xde, 0x48, 0x88, 0xbb, 0xab };

int machine_init_generic_xt(MACHINE_t* machine) {
	if (machine == NULL) return -1;

    MACHINE_LOGI("generic_xt: before i8259_init");
	i8259_init(&machine->i8259);
    MACHINE_LOGI("generic_xt: before i8253_init");
	i8253_init(&machine->i8253, &machine->i8259, &machine->pcspeaker);
    MACHINE_LOGI("generic_xt: before i8237_init");
	i8237_init(&machine->i8237, &machine->CPU);
    MACHINE_LOGI("generic_xt: before i8255_init");
	i8255_init(&machine->i8255, &machine->KeyState, &machine->pcspeaker);
    MACHINE_LOGI("generic_xt: before pcspeaker_init");
	pcspeaker_init(&machine->pcspeaker);

	//check machine HW flags and init devices accordingly
    MACHINE_LOGI("generic_xt: checking BLASTER hwflags");
    MACHINE_LOGI("generic_xt: hwflags=0x%llx", (unsigned long long)machine->hwflags);
    MACHINE_LOGI("generic_xt: BLASTER bit=%d", (machine->hwflags & 0x2ULL) != 0);
    MACHINE_LOGI("generic_xt: SKIP_BLASTER bit=%d", (machine->hwflags & 0x4000000000000000ULL) != 0);
    uint64_t hwf = machine->hwflags;
    int has_blaster = (hwf & 0x2ULL) != 0;
    int skip_blaster = (hwf & 0x4000000000000000ULL) != 0;
    MACHINE_LOGI("generic_xt: has_blaster=%d, skip_blaster=%d", has_blaster, skip_blaster);
    if (has_blaster && !skip_blaster) {
    MACHINE_LOGI("generic_xt: entering BLASTER init");
    MACHINE_LOGI("generic_xt: before blaster_init");
    MACHINE_LOGI("generic_xt: before OPL3_init");
		OPL3_init(&machine->OPL3);
    MACHINE_LOGI("generic_xt: after OPL3_init");
    MACHINE_LOGI("generic_xt: before mixBlaster assignment");
		machine->mixBlaster = 1;
    MACHINE_LOGI("generic_xt: after mixBlaster assignment");
    MACHINE_LOGI("generic_xt: before mixOPL assignment");
		machine->mixOPL = 1;
    MACHINE_LOGI("generic_xt: after mixOPL assignment");
	}
	else if ((machine->hwflags & MACHINE_HW_OPL) && !(machine->hwflags & MACHINE_HW_SKIP_OPL)) { //else if because some games won't detect an SB without seeing the OPL, so if SB enabled then OPL already is
		//opl2_init(&machine->OPL2);
		OPL3_init(&machine->OPL3);
		machine->mixOPL = 1;
	}
    MACHINE_LOGI("generic_xt: checking RTC hwflags");
    uint64_t hwf2 = machine->hwflags;
    int has_rtc = (hwf2 & 0x100ULL) != 0;
    int skip_rtc = (hwf2 & 0x400000000000000ULL) != 0; // MACHINE_HW_SKIP_RTC
    MACHINE_LOGI("generic_xt: has_rtc=%d, skip_rtc=%d", has_rtc, skip_rtc);
    if (has_rtc && !skip_rtc) {
    MACHINE_LOGI("generic_xt: entering RTC init");
    MACHINE_LOGI("generic_xt: before rtc_init");
		rtc_init(&machine->CPU);
    MACHINE_LOGI("generic_xt: after rtc_init");
	}

    MACHINE_LOGI("generic_xt: checking UART0 hwflags");
    uint64_t hwf3 = machine->hwflags;
    MACHINE_LOGI("generic_xt: UART0_NONE=%d, UART0_MOUSE=%d, SKIP_UART0=%d",
        (hwf3 & 0x4ULL) != 0, (hwf3 & 0x8ULL) != 0, (hwf3 & 0x2000000000000000ULL) != 0);
    
	if ((machine->hwflags & MACHINE_HW_UART0_NONE) && !(machine->hwflags & MACHINE_HW_SKIP_UART0)) {
        MACHINE_LOGI("generic_xt: before uart_init UART0_NONE");
		uart_init(&machine->UART[0], &machine->i8259, 0x3F8, 4, NULL, NULL, NULL, NULL);
        MACHINE_LOGI("generic_xt: after uart_init UART0_NONE");
	}
	else if ((machine->hwflags & MACHINE_HW_UART0_MOUSE) && !(machine->hwflags & MACHINE_HW_SKIP_UART0)) {
        MACHINE_LOGI("generic_xt: before uart_init UART0_MOUSE");
		uart_init(&machine->UART[0], &machine->i8259, 0x3F8, 4, NULL, NULL, (void*)mouse_togglereset, NULL);
        MACHINE_LOGI("generic_xt: after uart_init UART0_MOUSE");
		mouse_init(&machine->UART[0]);
        MACHINE_LOGI("generic_xt: after mouse_init");
		timing_addTimer(mouse_rxpoll, NULL, baudrate / 9, TIMING_ENABLED);
        MACHINE_LOGI("generic_xt: after timing_addTimer");
	}
#ifdef ENABLE_TCP_MODEM
	else if ((machine->hwflags & MACHINE_HW_UART0_TCPMODEM) && !(machine->hwflags & MACHINE_HW_SKIP_UART0)) {
        MACHINE_LOGI("generic_xt: before uart_init UART0_TCPMODEM");
		uart_init(&machine->UART[0], &machine->i8259, 0x3F8, 4, (void*)tcpmodem_tx, &machine->tcpmodem[0], NULL, NULL);
        MACHINE_LOGI("generic_xt: after uart_init UART0_TCPMODEM");
		tcpmodem_init(&machine->tcpmodem[0], &machine->UART[0], 23);
        MACHINE_LOGI("generic_xt: after tcpmodem_init");
		timing_addTimer(tcpmodem_rxpoll, &machine->tcpmodem[0], baudrate / 9, TIMING_ENABLED);
        MACHINE_LOGI("generic_xt: after timing_addTimer TCPMODEM");
	}
#endif
    MACHINE_LOGI("generic_xt: checking UART1 hwflags");
    uint64_t hwf4 = machine->hwflags;
    MACHINE_LOGI("generic_xt: UART1_NONE=%d, UART1_MOUSE=%d, SKIP_UART1=%d",
        (hwf4 & 0x10ULL) != 0, (hwf4 & 0x20ULL) != 0, (hwf4 & 0x4000000000000000ULL) != 0);
    
	if ((machine->hwflags & MACHINE_HW_UART1_NONE) && !(machine->hwflags & MACHINE_HW_SKIP_UART1)) {
        MACHINE_LOGI("generic_xt: before uart_init UART1_NONE");
		uart_init(&machine->UART[1], &machine->i8259, 0x2F8, 3, NULL, NULL, NULL, NULL);
        MACHINE_LOGI("generic_xt: after uart_init UART1_NONE");
	}
	else if ((machine->hwflags & MACHINE_HW_UART1_MOUSE) && !(machine->hwflags & MACHINE_HW_SKIP_UART1)) {
        MACHINE_LOGI("generic_xt: before uart_init UART1_MOUSE");
		uart_init(&machine->UART[1], &machine->i8259, 0x2F8, 3, NULL, NULL, (void*)mouse_togglereset, NULL);
        MACHINE_LOGI("generic_xt: after uart_init UART1_MOUSE");
		mouse_init(&machine->UART[1]);
        MACHINE_LOGI("generic_xt: after mouse_init");
		timing_addTimer(mouse_rxpoll, NULL, baudrate / 9, TIMING_ENABLED);
        MACHINE_LOGI("generic_xt: after timing_addTimer");
	}
#ifdef ENABLE_TCP_MODEM
	else if ((machine->hwflags & MACHINE_HW_UART1_TCPMODEM) && !(machine->hwflags & MACHINE_HW_SKIP_UART1)) {
        MACHINE_LOGI("generic_xt: before uart_init UART1_TCPMODEM");
		uart_init(&machine->UART[1], &machine->i8259, 0x2F8, 3, (void*)tcpmodem_tx, &machine->tcpmodem[1], NULL, NULL);
        MACHINE_LOGI("generic_xt: after uart_init UART1_TCPMODEM");
		tcpmodem_init(&machine->tcpmodem[1], &machine->UART[1], 23);
        MACHINE_LOGI("generic_xt: after tcpmodem_init");
		timing_addTimer(tcpmodem_rxpoll, &machine->tcpmodem[1], baudrate / 9, TIMING_ENABLED);
        MACHINE_LOGI("generic_xt: after timing_addTimer TCPMODEM");
	}
#endif

#ifdef USE_NE2000
	if (machine->hwflags & MACHINE_HW_NE2000) {
		ne2000_init(&machine->ne2000, &machine->i8259, 0x300, 2, (uint8_t*)&mac);
		if (machine->pcap_if > -1) {
			if (pcap_init(&machine->ne2000, machine->pcap_if)) {
				return -1;
			}
		}
	}
#endif

	cpu_reset(&machine->CPU);
#ifndef USE_DISK_HLE
	fdc_init(&fdc, &machine->CPU, &i8259, &i8237);
	fdc_insert(&fdc, 0, "dos622.img");
#else
	biosdisk_init(&machine->CPU);
#endif

	switch (videocard) {
	case VIDEO_CARD_CGA:
		if (cga_init()) return -1;
		break;
	case VIDEO_CARD_VGA:
		if (vga_init()) return -1;
		break;
	}

	return 0;
}
int machine_init(MACHINE_t* machine, char* id) {
    if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-MACHINE", "machine_init: ENTRY");
    MACHINE_LOGI("machine_init: START, id=%s", id ? id : "NULL");
    int num = 0, match = 0, i = 0;
    MACHINE_LOGI("machine_init: before do-while loop");

    do {
        MACHINE_LOGI("machine_init: loop num=%d, id=%p", num, (void*)machine_defs[num].id);
        if (machine_defs[num].id == NULL) {
            MACHINE_LOGI("machine_init: NULL id at num=%d", num);
            debug_log(DEBUG_ERROR, "[MACHINE] ERROR: Machine definition not found: %s\r\n", id);
            return -1;
        }
        MACHINE_LOGI("machine_init: before _stricmp, id=%s, defs_id=%s", id ? id : "NULL", machine_defs[num].id ? machine_defs[num].id : "NULL");
        if (_stricmp(id, machine_defs[num].id) == 0) {
            MACHINE_LOGI("machine_init: match found");
            match = 1;
        }
        else {
            MACHINE_LOGI("machine_init: no match, num++");
            num++;
        }
    } while (!match);
    MACHINE_LOGI("machine_init: match at num=%d", num);

    MACHINE_LOGI("machine_init: after match, before memory init");
    MACHINE_LOGI("machine_init: machine_mem[%d][0].memtype=%d", num, machine_mem[num][0].memtype);
	debug_log(DEBUG_INFO, "[MACHINE] Initializing machine: \"%s\" (%s)\r\n", machine_defs[num].description, machine_defs[num].id);
    DIAG("machine_init: starting, id=%s", id);

	//Initialize machine memory map
	while(1) {
        MACHINE_LOGI("machine_init: while loop iteration i=%d", i);
        MACHINE_LOGI("machine_init: memtype=%d, start=0x%X, size=%u", machine_mem[num][i].memtype, machine_mem[num][i].start, machine_mem[num][i].size);
		uint8_t* temp;
        MACHINE_LOGI("machine_init: before ENDLIST check");
		if (machine_mem[num][i].memtype == MACHINE_MEM_ENDLIST) {
        MACHINE_LOGI("machine_init: after ENDLIST check, breaking");
			break;
		}
        if (machine_mem[num][i].memtype == MACHINE_MEM_ROM) {
            DIAG("machine_init: loading ROM at 0x%05X, size=0x%04X, file=%s", machine_mem[num][i].start, machine_mem[num][i].size, machine_mem[num][i].filename);
        }
		temp = (uint8_t*)malloc((size_t)machine_mem[num][i].size);
		if ((temp == NULL) &&
			((machine_mem[num][i].required == MACHINE_ROM_REQUIRED) || (machine_mem[num][i].required == MACHINE_ROM_ISNOTROM))) {
			debug_log(DEBUG_ERROR, "[MACHINE] ERROR: Unable to allocate %lu bytes of memory\r\n", machine_mem[num][i].size);
			return -1;
		}
		if (machine_mem[num][i].memtype == MACHINE_MEM_RAM) {
			memory_mapRegister(machine_mem[num][i].start, machine_mem[num][i].size, temp, temp);
		} else if (machine_mem[num][i].memtype == MACHINE_MEM_ROM) {
			int ret;
#ifdef __ANDROID__
			{
				char cwd[1024];
				char resolved[2048];
				if (getcwd(cwd, sizeof(cwd)) != NULL) {
					snprintf(resolved, sizeof(resolved), "%s/%s", cwd, machine_mem[num][i].filename);
					MACHINE_LOGI("machine_init: loading ROM, filename=%s, cwd=%s, resolved=%s",
						machine_mem[num][i].filename, cwd, resolved);
				} else {
					MACHINE_LOGI("machine_init: loading ROM, filename=%s, getcwd failed",
						machine_mem[num][i].filename);
				}
			}
#endif
			ret = utility_loadFile(temp, machine_mem[num][i].size, machine_mem[num][i].filename);
			if ((machine_mem[num][i].required == MACHINE_ROM_REQUIRED) && ret) {
				debug_log(DEBUG_ERROR, "[MACHINE] Could not open file, or size is less than expected: %s\r\n", machine_mem[num][i].filename);
				return -1;
			}
			memory_mapRegister(machine_mem[num][i].start, machine_mem[num][i].size, temp, NULL);
		}
		i++;
	}

    MACHINE_LOGI("machine_init: after while loop, before hwflags");
    MACHINE_LOGI("machine_init: machine=%p, hwflags=0x%llx", (void*)machine, (unsigned long long)machine_defs[num].hwflags);
    MACHINE_LOGI("machine_init: before hwflags read");
    uint64_t old_hwflags = machine->hwflags;
    MACHINE_LOGI("machine_init: old_hwflags=0x%llx", (unsigned long long)old_hwflags);
    MACHINE_LOGI("machine_init: before hwflags write");
    machine->hwflags = old_hwflags | machine_defs[num].hwflags;
    MACHINE_LOGI("machine_init: after hwflags write");

    MACHINE_LOGI("machine_init: before videocard check");
    MACHINE_LOGI("machine_init: videocard=%d", videocard);
	if (videocard == 0xFF) {
    MACHINE_LOGI("machine_init: before videocard assignment, video=%d", machine_defs[num].video);
		videocard = machine_defs[num].video;
    MACHINE_LOGI("machine_init: after videocard assignment");
	}

    MACHINE_LOGI("machine_init: before speedarg check, speedarg=%f", speedarg);
    MACHINE_LOGI("machine_init: in speedarg > 0 branch");
    MACHINE_LOGI("machine_init: checking speedarg");
    int speedarg_is_zero = 0;
    {
        uint64_t speedarg_bits;
        memcpy(&speedarg_bits, &speedarg, sizeof(speedarg));
        MACHINE_LOGI("machine_init: speedarg bits=0x%llx", (unsigned long long)speedarg_bits);
        speedarg_is_zero = (speedarg_bits == 0);
    }
    if (!speedarg_is_zero) {
        MACHINE_LOGI("machine_init: speedarg non-zero, checking sign");
        uint64_t speedarg_bits;
        memcpy(&speedarg_bits, &speedarg, sizeof(speedarg));
        int speedarg_negative = (speedarg_bits & 0x8000000000000000ULL) != 0;
        MACHINE_LOGI("machine_init: speedarg_negative=%d", speedarg_negative);
        if (speedarg_negative) {
            speed = -1;
        } else {
            speed = speedarg;
        }
    } else {
        MACHINE_LOGI("machine_init: speedarg is zero, using default");
        speed = machine_defs[num].speed;
    MACHINE_LOGI("machine_init: after speed = default");
    }

    MACHINE_LOGI("machine_init: before machine-specific init");
	if ((*machine_defs[num].init)(machine)) { //call machine-specific init routine
		return -1;
	}
    DIAG("machine_init: memory map complete");

	return num;
}

void machine_list() {
	int machine = 0;

	printf("Valid " STR_TITLE " machines:\r\n");

	while(machine_defs[machine].id != NULL) {
		printf("%s: \"%s\"\r\n", machine_defs[machine].id, machine_defs[machine].description);
		machine++;
	}
}
