#include <stdlib.h>
#include <stdint.h>
#include "mgba/core/core.h"
#include "mgba/core/log.h"
#include "mgba/core/timing.h"
#include "mgba/core/serialize.h"
#include "mgba/core/blip_buf.h"
#include "mgba/gba/core.h"
#include "mgba/gba/interface.h"
#include "mgba/internal/gba/gba.h"
#include "mgba/internal/gba/video.h"
#include "mgba/internal/gba/overrides.h"
#include "mgba/internal/arm/isa-inlines.h"
#include "mgba/debugger/debugger.h"
#include "mgba-util/common.h"
#include "mgba-util/vfs.h"

const char* const binaryName = "mgba";

#ifdef _WIN32
#define EXP __declspec(dllexport)
#else
#define EXP __attribute__((visibility("default")))
#endif

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:		the pointer to the member.
 * @type:	   the type of the container struct this is embedded in.
 * @member:	 the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({					  \
		const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
		(type *)( (char *)__mptr - offsetof(type,member) );})
struct VFile* VFileOpenFD(const char* path, int flags) { return NULL; }

typedef struct
{
	struct mCore* core;
	struct mLogger logger;
	struct GBA* gba; // anything that uses this will be deprecated eventually
	color_t vbuff[GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS];
	void* rom;
	struct VFile* romvf;
	char bios[16384];
	struct VFile* biosvf;
	char sram[131072];
	struct VFile* sramvf;
	struct mKeyCallback keysource;
	struct mRotationSource rotsource;
	struct mRTCSource rtcsource;
	struct GBALuminanceSource lumasource;
	struct mDebugger debugger;
	int16_t tiltx;
	int16_t tilty;
	int16_t tiltz;
	int64_t time;
	uint8_t light;
	uint16_t keys;
	int lagged;
	int skipbios;
	uint32_t palette[65536];
	void (*input_callback)(void);
	void (*trace_callback)(const char *buffer);
	void (*exec_callback)(uint32_t pc);
	void (*mem_callback)(uint32_t addr, enum mWatchpointType type, uint32_t oldValue, uint32_t newValue);
} bizctx;

static int32_t GetX(struct mRotationSource* rotationSource)
{
	return container_of(rotationSource, bizctx, rotsource)->tiltx << 16;
}
static int32_t GetY(struct mRotationSource* rotationSource)
{
	return container_of(rotationSource, bizctx, rotsource)->tilty << 16;
}
static int32_t GetZ(struct mRotationSource* rotationSource)
{
	return container_of(rotationSource, bizctx, rotsource)->tiltz << 16;
}
static uint8_t GetLight(struct GBALuminanceSource* luminanceSource)
{
	return container_of(luminanceSource, bizctx, lumasource)->light;
}
static time_t GetTime(struct mRTCSource* rtcSource)
{
	return container_of(rtcSource, bizctx, rtcsource)->time;
}
static uint16_t GetKeys(struct mKeyCallback* keypadSource)
{
	bizctx *ctx = container_of(keypadSource, bizctx, keysource);
	ctx->input_callback();
	ctx->lagged = false;
	return ctx->keys;
}
static void RotationCB(struct mRotationSource* rotationSource)
{
	bizctx* ctx = container_of(rotationSource, bizctx, rotsource);
	ctx->input_callback();
	ctx->lagged = false;
}
static void LightCB(struct GBALuminanceSource* luminanceSource)
{
	bizctx* ctx = container_of(luminanceSource, bizctx, lumasource);
	ctx->input_callback();
	ctx->lagged = false;
}
static void TimeCB(struct mRTCSource* rtcSource)
{
	// no, reading the rtc registers should not unset the lagged flag
	// bizctx* ctx = container_of(rtcSource, bizctx, rtcsource);
	// ctx->lagged = false;
}
static void logdebug(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args)
{

}

static void resetinternal(bizctx* ctx)
{
	ctx->core->reset(ctx->core);
	if (ctx->skipbios)
		GBASkipBIOS(ctx->gba);
}

EXP void BizDestroy(bizctx* ctx)
{
	ctx->core->deinit(ctx->core);
	free(ctx->rom);
	free(ctx);
}

typedef struct
{
	enum SavedataType savetype;
	enum GBAHardwareDevice hardware;
	uint32_t idleLoop;
} overrideinfo;

void exec_hook(struct mDebugger* debugger)
{
	bizctx* ctx = container_of(debugger, bizctx, debugger);
	if (ctx->trace_callback)
	{
		char trace[1024];
		trace[sizeof(trace) - 1] = '\0';
		size_t traceSize = sizeof(trace) - 2;
		debugger->platform->trace(debugger->platform, trace, &traceSize);
		if (traceSize + 1 <= sizeof(trace)) {
			trace[traceSize] = '\n';
			trace[traceSize + 1] = '\0';
		}
		ctx->trace_callback(trace);
	}
	if (ctx->exec_callback)
		ctx->exec_callback(_ARMPCAddress(debugger->core->cpu));
}

EXP void BizSetInputCallback(bizctx* ctx, void(*callback)(void))
{
	ctx->input_callback = callback;
}

EXP void BizSetTraceCallback(bizctx* ctx, void(*callback)(const char *buffer))
{
	ctx->trace_callback = callback;
}

EXP void BizSetExecCallback(bizctx* ctx, void(*callback)(uint32_t pc))
{
	ctx->exec_callback = callback;
}

EXP void BizSetMemCallback(bizctx* ctx, void(*callback)(uint32_t addr, enum mWatchpointType type, uint32_t oldValue, uint32_t newValue))
{
	ctx->mem_callback = callback;
}

static void watchpoint_entry(struct mDebugger* debugger, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info)
{
	bizctx* ctx = container_of(debugger, bizctx, debugger);
	if (reason == DEBUGGER_ENTER_WATCHPOINT && info && ctx->mem_callback)
		ctx->mem_callback(info->address, info->type.wp.accessType, info->type.wp.oldValue, info->type.wp.newValue);
	debugger->state = ctx->trace_callback || ctx->exec_callback ? DEBUGGER_CALLBACK : DEBUGGER_RUNNING;
}

EXP ssize_t BizSetWatchpoint(bizctx* ctx, uint32_t addr, enum mWatchpointType type)
{
	struct mWatchpoint watchpoint = {
		.address = addr,
		.segment = -1,
		.type = type
	};
	return ctx->debugger.platform->setWatchpoint(ctx->debugger.platform, &watchpoint);
}

EXP bool BizClearWatchpoint(bizctx* ctx, ssize_t id)
{
	return ctx->debugger.platform->clearBreakpoint(ctx->debugger.platform, id);
}

EXP bizctx* BizCreate(const void* bios, const void* data, int length, const overrideinfo* dbinfo, int skipbios)
{
	bizctx* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
	{
		return NULL;
	}

	ctx->rom = malloc(length);
	if (!ctx->rom)
	{
		free(ctx);
		return NULL;
	}

	ctx->logger.log = logdebug;
	mLogSetDefaultLogger(&ctx->logger);
	ctx->skipbios = skipbios;

	memcpy(ctx->rom, data, length);
	ctx->romvf = VFileFromMemory(ctx->rom, length);
	ctx->core = GBACoreCreate();
	if (!ctx->core)
	{
		ctx->romvf->close(ctx->romvf);
		free(ctx->rom);
		free(ctx);
		return NULL;
	}
	mCoreInitConfig(ctx->core, NULL);
	if (!ctx->core->init(ctx->core))
	{
		free(ctx->rom);
		free(ctx);
		return NULL;
	}
	ctx->gba = ctx->core->board;

	ctx->core->setVideoBuffer(ctx->core, ctx->vbuff, GBA_VIDEO_HORIZONTAL_PIXELS);
	ctx->core->setAudioBufferSize(ctx->core, 1024);

	blip_set_rates(ctx->core->getAudioChannel(ctx->core, 0), ctx->core->frequency(ctx->core), 44100);
	blip_set_rates(ctx->core->getAudioChannel(ctx->core, 1), ctx->core->frequency(ctx->core), 44100);

	ctx->core->loadROM(ctx->core, ctx->romvf);
	memset(ctx->sram, 0xff, 131072);
	ctx->sramvf = VFileFromMemory(ctx->sram, 131072);
	ctx->core->loadSave(ctx->core, ctx->sramvf);

	mCoreSetRTC(ctx->core, &ctx->rtcsource);
	
	ctx->gba->idleOptimization = IDLE_LOOP_IGNORE; // Don't do "idle skipping"
	ctx->gba->keyCallback = &ctx->keysource; // Callback for key reading

	ctx->keysource.readKeys = GetKeys;
	ctx->rotsource.sample = RotationCB;
	ctx->rotsource.readTiltX = GetX;
	ctx->rotsource.readTiltY = GetY;
	ctx->rotsource.readGyroZ = GetZ;
	ctx->lumasource.sample = LightCB;
	ctx->lumasource.readLuminance = GetLight;
	ctx->rtcsource.sample = TimeCB;
	ctx->rtcsource.unixTime = GetTime;
	
	ctx->core->setPeripheral(ctx->core, mPERIPH_ROTATION, &ctx->rotsource);
	ctx->core->setPeripheral(ctx->core, mPERIPH_GBA_LUMINANCE, &ctx->lumasource);

	if (bios)
	{
		memcpy(ctx->bios, bios, 16384);
		ctx->biosvf = VFileFromMemory(ctx->bios, 16384);
		/*if (!GBAIsBIOS(ctx->biosvf))
		{
			ctx->biosvf->close(ctx->biosvf);
			GBADestroy(&ctx->gba);
			free(ctx);
			return NULL;
		}*/
		ctx->core->loadBIOS(ctx->core, ctx->biosvf, 0);
	}

	if (dbinfo) // front end override
	{
		struct GBACartridgeOverride override;
		const struct GBACartridge* cart = (const struct GBACartridge*) ctx->gba->memory.rom;
		memcpy(override.id, &cart->id, sizeof(override.id));
		override.savetype = dbinfo->savetype;
		override.hardware = dbinfo->hardware;
		override.idleLoop = dbinfo->idleLoop;
		GBAOverrideApply(ctx->gba, &override);
	}
	
	mDebuggerAttach(&ctx->debugger, ctx->core);
	ctx->debugger.custom = exec_hook;
	ctx->debugger.entered = watchpoint_entry;
	
	resetinternal(ctx);
	return ctx;
}

EXP void BizReset(bizctx* ctx)
{
	resetinternal(ctx);
}

static void blit(uint32_t* dst, const color_t* src, const uint32_t* palette)
{
	uint32_t* dst_end = dst + GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS;

	while (dst < dst_end)
	{
		*dst++ = palette[*src++];
	}
}

EXP int BizAdvance(bizctx* ctx, uint16_t keys, uint32_t* vbuff, int* nsamp, int16_t* sbuff,
	int64_t time, int16_t gyrox, int16_t gyroy, int16_t gyroz, uint8_t luma)
{
	ctx->core->setKeys(ctx->core, keys);
	ctx->keys = keys;
	ctx->light = luma;
	ctx->time = time;
	ctx->tiltx = gyrox;
	ctx->tilty = gyroy;
	ctx->tiltz = gyroz;
	ctx->lagged = true;

	ctx->debugger.state = ctx->trace_callback || ctx->exec_callback ? DEBUGGER_CALLBACK : DEBUGGER_RUNNING;
	mDebuggerRunFrame(&ctx->debugger);

	blit(vbuff, ctx->vbuff, ctx->palette);
	*nsamp = blip_samples_avail(ctx->core->getAudioChannel(ctx->core, 0));
	if (*nsamp > 1024)
		*nsamp = 1024;
	blip_read_samples(ctx->core->getAudioChannel(ctx->core, 0), sbuff, 1024, true);
	blip_read_samples(ctx->core->getAudioChannel(ctx->core, 1), sbuff + 1, 1024, true);
	return ctx->lagged;
}

EXP void BizSetPalette(bizctx* ctx, const uint32_t* palette)
{
	memcpy(ctx->palette, palette, sizeof(ctx->palette));
}

struct MemoryAreas
{
	const void* bios;
	const void* wram;
	const void* iwram;
	const void* mmio;
	const void* palram;
	const void* vram;
	const void* oam;
	const void* rom;
	const void* sram;
};

EXP void BizGetMemoryAreas(bizctx* ctx, struct MemoryAreas* dst)
{
	size_t sizeOut;
	dst->bios = ctx->core->getMemoryBlock(ctx->core, REGION_BIOS, &sizeOut);
	dst->wram = ctx->core->getMemoryBlock(ctx->core, REGION_WORKING_RAM, &sizeOut);
	dst->iwram = ctx->core->getMemoryBlock(ctx->core, REGION_WORKING_IRAM, &sizeOut);
	dst->mmio = ctx->gba->memory.io;
	dst->palram = ctx->core->getMemoryBlock(ctx->core, REGION_PALETTE_RAM, &sizeOut);
	dst->vram = ctx->core->getMemoryBlock(ctx->core, REGION_VRAM, &sizeOut);
	dst->oam = ctx->core->getMemoryBlock(ctx->core, REGION_OAM, &sizeOut);
	dst->rom = ctx->core->getMemoryBlock(ctx->core, REGION_CART0, &sizeOut);
	// Return the buffer that BizHawk hands to mGBA for storing savedata.
	// getMemoryBlock is avoided because mGBA doesn't always know save type at startup,
	// so getMemoryBlock may return nothing until the save type is detected.
	// (returning the buffer directly avoids 0-size and variable-size savedata)
	dst->sram = ctx->sram;
}

EXP int BizGetSaveRam(bizctx* ctx, void* data, int size)
{
	ctx->sramvf->seek(ctx->sramvf, 0, SEEK_SET);
	return ctx->sramvf->read(ctx->sramvf, data, size);
}

EXP void BizPutSaveRam(bizctx* ctx, const void* data, int size)
{
	ctx->sramvf->seek(ctx->sramvf, 0, SEEK_SET);
	ctx->sramvf->write(ctx->sramvf, data, size);
}

// state sizes can vary!
EXP int BizStartGetState(bizctx* ctx, struct VFile** file, int* size)
{
	struct VFile* vf = VFileMemChunk(NULL, 0);
	if (!mCoreSaveStateNamed(ctx->core, vf, SAVESTATE_SAVEDATA))
	{
		vf->close(vf);
		return 0;
	}
	*file = vf;
	*size = vf->seek(vf, 0, SEEK_END);
	return 1;
}

EXP void BizFinishGetState(struct VFile* file, void* data, int size)
{
	file->seek(file, 0, SEEK_SET);
	file->read(file, data, size);
	file->close(file);
}

EXP int BizPutState(bizctx* ctx, const void* data, int size)
{
	struct VFile* vf = VFileFromConstMemory(data, size);
	int ret = mCoreLoadStateNamed(ctx->core, vf, SAVESTATE_SAVEDATA);
	vf->close(vf);
	return ret;
}

EXP void BizSetLayerMask(bizctx *ctx, int mask)
{
	for (int i = 0; i < 5; i++)
		ctx->core->enableVideoLayer(ctx->core, i, mask & 1 << i);
}

EXP void BizSetSoundMask(bizctx* ctx, int mask)
{
	for (int i = 0; i < 6; i++)
		ctx->core->enableAudioChannel(ctx->core, i, mask & 1 << i);
}

EXP void BizGetRegisters(bizctx* ctx, int* dest)
{
	memcpy(dest, ctx->gba->cpu, 18 * sizeof(int));
}

EXP void BizSetRegister(bizctx* ctx, int index, int value)
{
	if (index >= 0 && index < 16)
	{
		ctx->gba->cpu->gprs[index] = value;	
	}

	if (index == 16)
	{
		memcpy(&ctx->gba->cpu->cpsr, &value, sizeof(int));
	}

	if (index == 17)
	{
		memcpy(&ctx->gba->cpu->spsr, &value, sizeof(int));
	}
}

EXP uint64_t BizGetGlobalTime(bizctx* ctx)
{
	return mTimingGlobalTime(ctx->debugger.core->timing);
}

EXP void BizWriteBus(bizctx* ctx, uint32_t addr, uint8_t val)
{
	ctx->core->rawWrite8(ctx->core, addr, -1, val);
}

EXP uint8_t BizReadBus(bizctx* ctx, uint32_t addr)
{
	return ctx->core->busRead8(ctx->core, addr);
}
