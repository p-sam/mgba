#include "gba.h"

#include "gba-bios.h"
#include "gba-io.h"
#include "gba-sio.h"
#include "gba-thread.h"

#include "util/memory.h"
#include "util/patch.h"

#include <sys/stat.h>

const uint32_t GBA_ARM7TDMI_FREQUENCY = 0x1000000;
const uint32_t GBA_COMPONENT_MAGIC = 0x1000000;

enum {
	SP_BASE_SYSTEM = 0x03FFFF00,
	SP_BASE_IRQ = 0x03FFFFA0,
	SP_BASE_SUPERVISOR = 0x03FFFFE0
};

struct GBACartridgeOverride {
	const char id[4];
	enum SavedataType type;
	int gpio;
};

static const struct GBACartridgeOverride _overrides[] = {
	// Boktai: The Sun is in Your Hand
	{ "U3IE", SAVEDATA_EEPROM, GPIO_RTC | GPIO_LIGHT_SENSOR },
	{ "U3IP", SAVEDATA_EEPROM, GPIO_RTC | GPIO_LIGHT_SENSOR },

	// Boktai 2: Solar Boy Django
	{ "U32E", SAVEDATA_EEPROM, GPIO_RTC | GPIO_LIGHT_SENSOR },
	{ "U32P", SAVEDATA_EEPROM, GPIO_RTC | GPIO_LIGHT_SENSOR },

	// Drill Dozer
	{ "V49J", SAVEDATA_SRAM, GPIO_RUMBLE },
	{ "V49E", SAVEDATA_SRAM, GPIO_RUMBLE },

	// Pokemon Ruby
	{ "AXVJ", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXVE", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXVP", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXVI", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXVS", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXVD", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXVF", SAVEDATA_FLASH1M, GPIO_RTC },

	// Pokemon Sapphire
	{ "AXPJ", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXPE", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXPP", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXPI", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXPS", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXPD", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "AXPF", SAVEDATA_FLASH1M, GPIO_RTC },

	// Pokemon Emerald
	{ "BPEJ", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "BPEE", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "BPEP", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "BPEI", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "BPES", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "BPED", SAVEDATA_FLASH1M, GPIO_RTC },
	{ "BPEF", SAVEDATA_FLASH1M, GPIO_RTC },

	// Pokemon FireRed
	{ "BPRJ", SAVEDATA_FLASH1M, GPIO_NONE },
	{ "BPRE", SAVEDATA_FLASH1M, GPIO_NONE },
	{ "BPRP", SAVEDATA_FLASH1M, GPIO_NONE },

	// Pokemon LeafGreen
	{ "BPGJ", SAVEDATA_FLASH1M, GPIO_NONE },
	{ "BPGE", SAVEDATA_FLASH1M, GPIO_NONE },
	{ "BPGP", SAVEDATA_FLASH1M, GPIO_NONE },

	// RockMan EXE 4.5 - Real Operation
	{ "BR4J", SAVEDATA_FLASH512, GPIO_RTC },

	// Super Mario Advance 4
	{ "AX4J", SAVEDATA_FLASH1M, GPIO_NONE },
	{ "AX4E", SAVEDATA_FLASH1M, GPIO_NONE },
	{ "AX4P", SAVEDATA_FLASH1M, GPIO_NONE },

	// Wario Ware Twisted
	{ "RWZJ", SAVEDATA_SRAM, GPIO_RUMBLE | GPIO_GYRO },
	{ "RWZE", SAVEDATA_SRAM, GPIO_RUMBLE | GPIO_GYRO },
	{ "RWZP", SAVEDATA_SRAM, GPIO_RUMBLE | GPIO_GYRO },

	{ { 0, 0, 0, 0 }, 0, 0 }
};

static void GBAInit(struct ARMCore* cpu, struct ARMComponent* component);
static void GBAInterruptHandlerInit(struct ARMInterruptHandler* irqh);
static void GBAProcessEvents(struct ARMCore* cpu);
static int32_t GBATimersProcessEvents(struct GBA* gba, int32_t cycles);
static void GBAHitStub(struct ARMCore* cpu, uint32_t opcode);
static void GBAIllegal(struct ARMCore* cpu, uint32_t opcode);

static void _checkOverrides(struct GBA* gba, uint32_t code);

void GBACreate(struct GBA* gba) {
	gba->d.id = GBA_COMPONENT_MAGIC;
	gba->d.init = GBAInit;
	gba->d.deinit = 0;
}

static void GBAInit(struct ARMCore* cpu, struct ARMComponent* component) {
	struct GBA* gba = (struct GBA*) component;
	gba->cpu = cpu;
	gba->debugger = 0;
	gba->savefile = 0;

	GBAInterruptHandlerInit(&cpu->irqh);
	GBAMemoryInit(gba);

	gba->video.p = gba;
	GBAVideoInit(&gba->video);

	gba->audio.p = gba;
	GBAAudioInit(&gba->audio);

	GBAIOInit(gba);

	gba->sio.p = gba;
	GBASIOInit(&gba->sio);

	gba->timersEnabled = 0;
	memset(gba->timers, 0, sizeof(gba->timers));

	gba->springIRQ = 0;
	gba->keySource = 0;
	gba->rotationSource = 0;
	gba->rumble = 0;

	gba->logLevel = GBA_LOG_INFO | GBA_LOG_WARN | GBA_LOG_ERROR | GBA_LOG_FATAL;

	gba->biosChecksum = GBAChecksum(gba->memory.bios, SIZE_BIOS);
}

void GBADestroy(struct GBA* gba) {
	if (gba->pristineRom == gba->memory.rom) {
		gba->memory.rom = 0;
	}
	mappedMemoryFree(gba->pristineRom, gba->pristineRomSize);
	GBAMemoryDeinit(gba);
	GBAVideoDeinit(&gba->video);
	GBAAudioDeinit(&gba->audio);
}

void GBAInterruptHandlerInit(struct ARMInterruptHandler* irqh) {
	irqh->reset = GBAReset;
	irqh->processEvents = GBAProcessEvents;
	irqh->swi16 = GBASwi16;
	irqh->swi32 = GBASwi32;
	irqh->hitIllegal = GBAIllegal;
	irqh->readCPSR = GBATestIRQ;
	irqh->hitStub = GBAHitStub;
}

void GBAReset(struct ARMCore* cpu) {
	ARMSetPrivilegeMode(cpu, MODE_IRQ);
	cpu->gprs[ARM_SP] = SP_BASE_IRQ;
	ARMSetPrivilegeMode(cpu, MODE_SUPERVISOR);
	cpu->gprs[ARM_SP] = SP_BASE_SUPERVISOR;
	ARMSetPrivilegeMode(cpu, MODE_SYSTEM);
	cpu->gprs[ARM_SP] = SP_BASE_SYSTEM;
}

static void GBAProcessEvents(struct ARMCore* cpu) {
	do {
		struct GBA* gba = (struct GBA*) cpu->master;
		int32_t cycles = cpu->cycles;
		int32_t nextEvent = INT_MAX;
		int32_t testEvent;

		if (gba->springIRQ) {
			ARMRaiseIRQ(cpu);
			gba->springIRQ = 0;
		}

		testEvent = GBAVideoProcessEvents(&gba->video, cycles);
		if (testEvent < nextEvent) {
			nextEvent = testEvent;
		}

		testEvent = GBAAudioProcessEvents(&gba->audio, cycles);
		if (testEvent < nextEvent) {
			nextEvent = testEvent;
		}

		testEvent = GBATimersProcessEvents(gba, cycles);
		if (testEvent < nextEvent) {
			nextEvent = testEvent;
		}

		testEvent = GBAMemoryRunDMAs(gba, cycles);
		if (testEvent < nextEvent) {
			nextEvent = testEvent;
		}

		testEvent = GBASIOProcessEvents(&gba->sio, cycles);
		if (testEvent < nextEvent) {
			nextEvent = testEvent;
		}

		cpu->cycles -= cycles;
		cpu->nextEvent = nextEvent;

		if (cpu->halted) {
			cpu->cycles = cpu->nextEvent;
		}
	} while (cpu->cycles >= cpu->nextEvent);
}

static int32_t GBATimersProcessEvents(struct GBA* gba, int32_t cycles) {
	int32_t nextEvent = INT_MAX;
	if (gba->timersEnabled) {
		struct GBATimer* timer;
		struct GBATimer* nextTimer;

		timer = &gba->timers[0];
		if (timer->enable) {
			timer->nextEvent -= cycles;
			timer->lastEvent -= cycles;
			if (timer->nextEvent <= 0) {
				timer->lastEvent = timer->nextEvent;
				timer->nextEvent += timer->overflowInterval;
				gba->memory.io[REG_TM0CNT_LO >> 1] = timer->reload;
				timer->oldReload = timer->reload;

				if (timer->doIrq) {
					GBARaiseIRQ(gba, IRQ_TIMER0);
				}

				if (gba->audio.enable) {
					if ((gba->audio.chALeft || gba->audio.chARight) && gba->audio.chATimer == 0) {
						GBAAudioSampleFIFO(&gba->audio, 0, timer->lastEvent);
					}

					if ((gba->audio.chBLeft || gba->audio.chBRight) && gba->audio.chBTimer == 0) {
						GBAAudioSampleFIFO(&gba->audio, 1, timer->lastEvent);
					}
				}

				nextTimer = &gba->timers[1];
				if (nextTimer->countUp) {
					++gba->memory.io[REG_TM1CNT_LO >> 1];
					if (!gba->memory.io[REG_TM1CNT_LO >> 1]) {
						nextTimer->nextEvent = 0;
					}
				}
			}
			nextEvent = timer->nextEvent;
		}

		timer = &gba->timers[1];
		if (timer->enable) {
			timer->nextEvent -= cycles;
			timer->lastEvent -= cycles;
			if (timer->nextEvent <= 0) {
				timer->lastEvent = timer->nextEvent;
				timer->nextEvent += timer->overflowInterval;
				gba->memory.io[REG_TM1CNT_LO >> 1] = timer->reload;
				timer->oldReload = timer->reload;

				if (timer->doIrq) {
					GBARaiseIRQ(gba, IRQ_TIMER1);
				}

				if (gba->audio.enable) {
					if ((gba->audio.chALeft || gba->audio.chARight) && gba->audio.chATimer == 1) {
						GBAAudioSampleFIFO(&gba->audio, 0, timer->lastEvent);
					}

					if ((gba->audio.chBLeft || gba->audio.chBRight) && gba->audio.chBTimer == 1) {
						GBAAudioSampleFIFO(&gba->audio, 1, timer->lastEvent);
					}
				}

				if (timer->countUp) {
					timer->nextEvent = INT_MAX;
				}

				nextTimer = &gba->timers[2];
				if (nextTimer->countUp) {
					++gba->memory.io[REG_TM2CNT_LO >> 1];
					if (!gba->memory.io[REG_TM2CNT_LO >> 1]) {
						nextTimer->nextEvent = 0;
					}
				}
			}
			if (timer->nextEvent < nextEvent) {
				nextEvent = timer->nextEvent;
			}
		}

		timer = &gba->timers[2];
		if (timer->enable) {
			timer->nextEvent -= cycles;
			timer->lastEvent -= cycles;
			nextEvent = timer->nextEvent;
			if (timer->nextEvent <= 0) {
				timer->lastEvent = timer->nextEvent;
				timer->nextEvent += timer->overflowInterval;
				gba->memory.io[REG_TM2CNT_LO >> 1] = timer->reload;
				timer->oldReload = timer->reload;

				if (timer->doIrq) {
					GBARaiseIRQ(gba, IRQ_TIMER2);
				}

				if (timer->countUp) {
					timer->nextEvent = INT_MAX;
				}

				nextTimer = &gba->timers[3];
				if (nextTimer->countUp) {
					++gba->memory.io[REG_TM3CNT_LO >> 1];
					if (!gba->memory.io[REG_TM3CNT_LO >> 1]) {
						nextTimer->nextEvent = 0;
					}
				}
			}
			if (timer->nextEvent < nextEvent) {
				nextEvent = timer->nextEvent;
			}
		}

		timer = &gba->timers[3];
		if (timer->enable) {
			timer->nextEvent -= cycles;
			timer->lastEvent -= cycles;
			nextEvent = timer->nextEvent;
			if (timer->nextEvent <= 0) {
				timer->lastEvent = timer->nextEvent;
				timer->nextEvent += timer->overflowInterval;
				gba->memory.io[REG_TM3CNT_LO >> 1] = timer->reload;
				timer->oldReload = timer->reload;

				if (timer->doIrq) {
					GBARaiseIRQ(gba, IRQ_TIMER3);
				}

				if (timer->countUp) {
					timer->nextEvent = INT_MAX;
				}
			}
			if (timer->nextEvent < nextEvent) {
				nextEvent = timer->nextEvent;
			}
		}
	}
	return nextEvent;
}

void GBAAttachDebugger(struct GBA* gba, struct ARMDebugger* debugger) {
	gba->debugger = debugger;
}

void GBADetachDebugger(struct GBA* gba) {
	gba->debugger = 0;
}

void GBALoadROM(struct GBA* gba, int fd, const char* fname) {
	struct stat info;
	gba->pristineRom = fileMemoryMap(fd, SIZE_CART0, MEMORY_READ);
	gba->memory.rom = gba->pristineRom;
	gba->activeFile = fname;
	fstat(fd, &info);
	gba->pristineRomSize = info.st_size;
	gba->memory.romSize = gba->pristineRomSize;
	if (gba->savefile) {
		GBASavedataInit(&gba->memory.savedata, gba->savefile);
	}
	GBAGPIOInit(&gba->memory.gpio, &((uint16_t*) gba->memory.rom)[GPIO_REG_DATA >> 1]);
	_checkOverrides(gba, ((struct GBACartridge*) gba->memory.rom)->id);
	// TODO: error check
}

void GBALoadBIOS(struct GBA* gba, int fd) {
	gba->memory.bios = fileMemoryMap(fd, SIZE_BIOS, MEMORY_READ);
	gba->memory.fullBios = 1;
	uint32_t checksum = GBAChecksum(gba->memory.bios, SIZE_BIOS);
	GBALog(gba, GBA_LOG_DEBUG, "BIOS Checksum: 0x%X", checksum);
	if (checksum == GBA_BIOS_CHECKSUM) {
		GBALog(gba, GBA_LOG_INFO, "Official GBA BIOS detected");
	} else if (checksum == GBA_DS_BIOS_CHECKSUM) {
		GBALog(gba, GBA_LOG_INFO, "Official GBA (DS) BIOS detected");
	} else {
		GBALog(gba, GBA_LOG_WARN, "BIOS checksum incorrect");
	}
	gba->biosChecksum = checksum;
	if ((gba->cpu->gprs[ARM_PC] >> BASE_OFFSET) == BASE_BIOS) {
		gba->cpu->memory.setActiveRegion(gba->cpu, gba->cpu->gprs[ARM_PC]);
	}
	// TODO: error check
}

void GBAApplyPatch(struct GBA* gba, struct Patch* patch) {
	size_t patchedSize = patch->outputSize(patch, gba->memory.romSize);
	if (!patchedSize) {
		return;
	}
	gba->memory.rom = anonymousMemoryMap(patchedSize);
	memcpy(gba->memory.rom, gba->pristineRom, gba->memory.romSize > patchedSize ? patchedSize : gba->memory.romSize);
	if (!patch->applyPatch(patch, gba->memory.rom, patchedSize)) {
		mappedMemoryFree(gba->memory.rom, patchedSize);
		gba->memory.rom = gba->pristineRom;
		return;
	}
	gba->memory.romSize = patchedSize;
}

void GBATimerUpdateRegister(struct GBA* gba, int timer) {
	struct GBATimer* currentTimer = &gba->timers[timer];
	if (currentTimer->enable && !currentTimer->countUp) {
		gba->memory.io[(REG_TM0CNT_LO + (timer << 2)) >> 1] = currentTimer->oldReload + ((gba->cpu->cycles - currentTimer->lastEvent) >> currentTimer->prescaleBits);
	}
}

void GBATimerWriteTMCNT_LO(struct GBA* gba, int timer, uint16_t reload) {
	gba->timers[timer].reload = reload;
}

void GBATimerWriteTMCNT_HI(struct GBA* gba, int timer, uint16_t control) {
	struct GBATimer* currentTimer = &gba->timers[timer];
	GBATimerUpdateRegister(gba, timer);

	int oldPrescale = currentTimer->prescaleBits;
	switch (control & 0x0003) {
	case 0x0000:
		currentTimer->prescaleBits = 0;
		break;
	case 0x0001:
		currentTimer->prescaleBits = 6;
		break;
	case 0x0002:
		currentTimer->prescaleBits = 8;
		break;
	case 0x0003:
		currentTimer->prescaleBits = 10;
		break;
	}
	currentTimer->countUp = !!(control & 0x0004);
	currentTimer->doIrq = !!(control & 0x0040);
	currentTimer->overflowInterval = (0x10000 - currentTimer->reload) << currentTimer->prescaleBits;
	int wasEnabled = currentTimer->enable;
	currentTimer->enable = !!(control & 0x0080);
	if (!wasEnabled && currentTimer->enable) {
		if (!currentTimer->countUp) {
			currentTimer->nextEvent = gba->cpu->cycles + currentTimer->overflowInterval;
		} else {
			currentTimer->nextEvent = INT_MAX;
		}
		gba->memory.io[(REG_TM0CNT_LO + (timer << 2)) >> 1] = currentTimer->reload;
		currentTimer->oldReload = currentTimer->reload;
		currentTimer->lastEvent = 0;
		gba->timersEnabled |= 1 << timer;
	} else if (wasEnabled && !currentTimer->enable) {
		if (!currentTimer->countUp) {
			gba->memory.io[(REG_TM0CNT_LO + (timer << 2)) >> 1] = currentTimer->oldReload + ((gba->cpu->cycles - currentTimer->lastEvent) >> oldPrescale);
		}
		gba->timersEnabled &= ~(1 << timer);
	} else if (currentTimer->prescaleBits != oldPrescale && !currentTimer->countUp) {
		// FIXME: this might be before present
		currentTimer->nextEvent = currentTimer->lastEvent + currentTimer->overflowInterval;
	}

	if (currentTimer->nextEvent < gba->cpu->nextEvent) {
		gba->cpu->nextEvent = currentTimer->nextEvent;
	}
};

void GBAWriteIE(struct GBA* gba, uint16_t value) {
	if (value & (1 << IRQ_KEYPAD)) {
		GBALog(gba, GBA_LOG_STUB, "Keypad interrupts not implemented");
	}

	if (value & (1 << IRQ_GAMEPAK)) {
		GBALog(gba, GBA_LOG_STUB, "Gamepak interrupts not implemented");
	}

	if (gba->memory.io[REG_IME >> 1] && value & gba->memory.io[REG_IF >> 1]) {
		ARMRaiseIRQ(gba->cpu);
	}
}

void GBAWriteIME(struct GBA* gba, uint16_t value) {
	if (value && gba->memory.io[REG_IE >> 1] & gba->memory.io[REG_IF >> 1]) {
		ARMRaiseIRQ(gba->cpu);
	}
}

void GBARaiseIRQ(struct GBA* gba, enum GBAIRQ irq) {
	gba->memory.io[REG_IF >> 1] |= 1 << irq;
	gba->cpu->halted = 0;

	if (gba->memory.io[REG_IME >> 1] && (gba->memory.io[REG_IE >> 1] & 1 << irq)) {
		ARMRaiseIRQ(gba->cpu);
	}
}

void GBATestIRQ(struct ARMCore* cpu) {
	struct GBA* gba = (struct GBA*) cpu->master;
	if (gba->memory.io[REG_IME >> 1] && gba->memory.io[REG_IE >> 1] & gba->memory.io[REG_IF >> 1]) {
		gba->springIRQ = 1;
		gba->cpu->nextEvent = 0;
	}
}

void GBAHalt(struct GBA* gba) {
	gba->cpu->nextEvent = 0;
	gba->cpu->halted = 1;
}

static void _GBAVLog(struct GBA* gba, enum GBALogLevel level, const char* format, va_list args) {
	if (!gba) {
		struct GBAThread* threadContext = GBAThreadGetContext();
		if (threadContext) {
			gba = threadContext->gba;
		}
	}

	if (gba && gba->logHandler) {
		gba->logHandler(gba, level, format, args);
		return;
	}

	if (gba && !(level & gba->logLevel) && level != GBA_LOG_FATAL) {
		return;
	}

	vprintf(format, args);
	printf("\n");

	if (level == GBA_LOG_FATAL) {
		abort();
	}
}

void GBALog(struct GBA* gba, enum GBALogLevel level, const char* format, ...) {
	va_list args;
	va_start(args, format);
	_GBAVLog(gba, level, format, args);
	va_end(args);
}

void GBADebuggerLogShim(struct ARMDebugger* debugger, enum DebuggerLogLevel level, const char* format, ...) {
	struct GBA* gba = 0;
	if (debugger->cpu) {
		gba = (struct GBA*) debugger->cpu->master;
	}

	enum GBALogLevel gbaLevel;
	switch (level) {
	case DEBUGGER_LOG_DEBUG:
		gbaLevel = GBA_LOG_DEBUG;
		break;
	case DEBUGGER_LOG_INFO:
		gbaLevel = GBA_LOG_INFO;
		break;
	case DEBUGGER_LOG_WARN:
		gbaLevel = GBA_LOG_WARN;
		break;
	case DEBUGGER_LOG_ERROR:
		gbaLevel = GBA_LOG_ERROR;
		break;
	}
	va_list args;
	va_start(args, format);
	_GBAVLog(gba, gbaLevel, format, args);
	va_end(args);
}


void GBAHitStub(struct ARMCore* cpu, uint32_t opcode) {
	struct GBA* gba = (struct GBA*) cpu->master;
	enum GBALogLevel level = GBA_LOG_FATAL;
	if (gba->debugger) {
		level = GBA_LOG_STUB;
		ARMDebuggerEnter(gba->debugger, DEBUGGER_ENTER_ILLEGAL_OP);
	}
	GBALog(gba, level, "Stub opcode: %08x", opcode);
}

void GBAIllegal(struct ARMCore* cpu, uint32_t opcode) {
	struct GBA* gba = (struct GBA*) cpu->master;
	GBALog(gba, GBA_LOG_WARN, "Illegal opcode: %08x", opcode);
	if (gba->debugger) {
		ARMDebuggerEnter(gba->debugger, DEBUGGER_ENTER_ILLEGAL_OP);
	}
}

void _checkOverrides(struct GBA* gba, uint32_t id) {
	int i;
	for (i = 0; _overrides[i].id[0]; ++i) {
		const uint32_t* overrideId = (const uint32_t*) _overrides[i].id;
		if (*overrideId == id) {
			switch (_overrides[i].type) {
				case SAVEDATA_FLASH512:
				case SAVEDATA_FLASH1M:
					gba->memory.savedata.type = _overrides[i].type;
					GBASavedataInitFlash(&gba->memory.savedata);
					break;
				case SAVEDATA_EEPROM:
					GBASavedataInitEEPROM(&gba->memory.savedata);
					break;
				case SAVEDATA_SRAM:
					GBASavedataInitSRAM(&gba->memory.savedata);
					break;
				case SAVEDATA_NONE:
					break;
			}

			if (_overrides[i].gpio & GPIO_RTC) {
				GBAGPIOInitRTC(&gba->memory.gpio);
			}

			if (_overrides[i].gpio & GPIO_GYRO) {
				GBAGPIOInitGyro(&gba->memory.gpio);
			}

			if (_overrides[i].gpio & GPIO_RUMBLE) {
				GBAGPIOInitRumble(&gba->memory.gpio);
			}
			return;
		}
	}
}
