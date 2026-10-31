/*
 * The panel's brightness, as iOS asks for it: a stand-in for the closed backlight driver.
 *
 * The window server does not set the panel's brightness itself. Its CoreBrightness library finds an
 * IOKit service for the display's backlight -- on this phone the class the kernel's own driver
 * publishes is AppleBacklight, and the user client that BrightnessControl wraps in
 * BCAppleBacklightBrtControl is the one that carries the level -- and the brightness that the
 * Control Centre slider (or the automatic dimming) asks for goes out through it. This kernel has no
 * such service, which is why that slider does nothing at all: the request is written into
 * CoreBrightness's own properties and then has nowhere to go.
 *
 * This file is the receiving end. It publishes the service, and the command the panel itself
 * understands -- DCS 0x51 on the DSI link, see hakios.display.c -- is where the level ends up.
 *
 * The protocol is not documented, so the first version of this file only records what arrives:
 * boot-arg backlight=1 publishes the service and every call to the user client is logged with its
 * selector and scalars (IOLog, so it lands in the run's log), and backlight_step=1 answers every
 * method with success without doing anything. The numbers in a log are the specification; guessing
 * them is how the display's command path was lost for a week.
 *
 * Boot-arg backlight=1        publish the stand-in service (class AppleBacklight)
 *          backlight_arm=1    ... and the second class name too, AppleARMBacklight, to see which
 *                             of the two the window server's CoreBrightness looks for
 *          backlight_dcs=1    send the level the service is given to the panel (DCS 0x51)
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <pexpert/pexpert.h>

#include "MiniDrivers.h"

extern "C" void hakios_display_set_brightness(uint32_t level);   /* hakios.display.c: DCS 0x51 */

extern "C" uint32_t gHakiosBacklightLevel;

static uint32_t gBacklightPublished;    /* how many services this made (the counting is for the log) */
static uint32_t gBacklightCalls;        /* how many times the user client was asked for */
static uint32_t gPanelLevel = 0x7ff;    /* the panel's level as last given to it (DCS 0x51), 0..0x7ff */

/*
 * One service for the display's backlight. The two class names are the two that the userland's
 * brightness stack may be looking for; both are the same object as far as the protocol goes.
 *
 * The names have to be these: IOServiceMatching("AppleBacklight") is a match on the class name
 * (IOService::matchPropertyTable, IOProviderClass is compared with metaCast), so a service whose
 * class is called anything else -- MiniAppleBacklight, say -- is not found at all. The shared base
 * class is deliberately *not* called AppleBacklight, and AppleARMBacklight does not derive from
 * AppleBacklight either: a match for AppleBacklight returns every instance that metaCasts to it,
 * subclass included, and the window server finding two backlights would be a new problem.
 */
class MiniBacklightService : public IOService {
	OSDeclareDefaultStructors(MiniBacklightService)

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties, IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(MiniBacklightService, IOService);

class AppleBacklight : public MiniBacklightService {
	OSDeclareDefaultStructors(AppleBacklight)
};

OSDefineMetaClassAndStructors(AppleBacklight, MiniBacklightService);

class AppleARMBacklight : public MiniBacklightService {
	OSDeclareDefaultStructors(AppleARMBacklight)
};

OSDefineMetaClassAndStructors(AppleARMBacklight, MiniBacklightService);

/* The one user client: it logs what it is asked and, when told to, sends it to the panel. */
class MiniAppleBacklightUserClient : public IOUserClient {
	OSDeclareDefaultStructors(MiniAppleBacklightUserClient)

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(MiniAppleBacklightUserClient, IOUserClient);

/*
 * What the caller reads before it can ask for a level: the range this service accepts.
 *
 * This is not a guess. The userland's own binaries (the dyld shared cache of the 16.5 image, in
 * /System/Cryptexes/OS/System/Library/Caches/com.apple.dyld) say what is read and by whom:
 *
 *   BrightnessControl, -[BCAppleBacklightBrtControl initWithService:] reads
 *       IODisplayParameters -> brightness -> min, max      ("...got back service %u", "Unable to
 *       find backlight service.", "_maxUser >= _minUser", "_maxNits >= _minNits", and works in
 *       BrightnessMilliNits / BrightnessMicroAmps)
 *   PowerLog's PLDisplayAgent reads the same dictionary's min, max, rawData, brightness
 *   CoreBrightness (CBDisplayContaineriOS) looks the service up by class name AppleARMBacklight;
 *       BCAppleBacklightBrtControl looks it up by AppleBacklight -- which is why both classes are
 *       published, not one
 *
 * A service without the dictionary has a range of nothing, and a caller that requires max >= min
 * has nothing to move. So it is published with the service and kept current with the level.
 */
static const uint32_t kPanelMaxLevel = 0x7ff;    /* mdss-dsi-bl-max-level */

static void
publishBrightnessParameters(IOService * service, uint32_t level)
{
	OSDictionary * params;
	OSDictionary * bright;
	OSNumber * value;
	OSNumber * low;
	OSNumber * high;

	/* getProvider() is what the user-client path hands over; a client with no provider is not worth
	 * a panic over a published property. */
	if (service == NULL) {
		return;
	}
	params = OSDictionary::withCapacity(1);
	bright = OSDictionary::withCapacity(3);
	value = OSNumber::withNumber((unsigned long long)level, 32);
	low = OSNumber::withNumber((unsigned long long)0, 32);
	high = OSNumber::withNumber((unsigned long long)kPanelMaxLevel, 32);

	if (params != NULL && bright != NULL && value != NULL && low != NULL && high != NULL) {
		bright->setObject("value", value);
		bright->setObject("min", low);
		bright->setObject("max", high);
		params->setObject("brightness", bright);
		service->setProperty("IODisplayParameters", params);
	}
	OSSafeReleaseNULL(high);
	OSSafeReleaseNULL(low);
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(bright);
	OSSafeReleaseNULL(params);
}

bool
MiniBacklightService::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	/* IODisplayParameters before registerService(): a caller that finds the service reads its
	 * range in the same breath, and a match with no range is a backlight that cannot move. */
	publishBrightnessParameters(this, gPanelLevel);
	IOLog("AppleBacklight: published as %s, provider %s, brightness %#x of %#x\n",
	    getMetaClass()->getClassName(),
	    provider != NULL ? provider->getMetaClass()->getClassName() : "none", gPanelLevel, kPanelMaxLevel);
	registerService();
	return true;
}

/* What a written dictionary contains, to the depth that matters: the keys, and the numbers. */
static void
showProperties(const OSDictionary * dict, unsigned depth)
{
	OSCollectionIterator * it = OSCollectionIterator::withCollection((const OSCollection *)dict);

	if (it == NULL || depth >= 4) {
		return;
	}
	for (OSSymbol * key = OSDynamicCast(OSSymbol, it->getNextObject()); key != NULL;
	    key = OSDynamicCast(OSSymbol, it->getNextObject())) {
		OSObject * value = dict->getObject(key);
		const char * name = key->getCStringNoCopy();

		if (OSDynamicCast(OSDictionary, value) != NULL) {
			printf("AppleBacklight:   %u: %s = %u keys\n", depth, name, ((const OSDictionary *)value)->getCount());
			showProperties((const OSDictionary *)value, depth + 1);
		} else if (OSDynamicCast(OSNumber, value) != NULL) {
			printf("AppleBacklight:   %u: %s = %llu\n", depth, name, ((const OSNumber *)value)->unsigned64BitValue());
		} else {
			printf("AppleBacklight:   %u: %s = not a number\n", depth, name);
		}
	}
	it->release();
}

/*
 * The second way in: a client that opens no user client writes the dictionary on the service itself
 * (IORegistryEntrySetCFProperties). The base class answers "unsupported" and keeps nothing
 * (iokit/Kernel/IORegistryEntry.cpp), so without this an IODisplayParameters write by such a client
 * is lost with no trace in the log. What macOS does with {brightness = {value, max}} is scaled to the
 * panel's 0..0x7ff here; whether the panel is written is still backlight_dcs=1's decision, as on the
 * user-client path.
 */
IOReturn
MiniBacklightService::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSDictionary * params;
	OSDictionary * bright;
	OSNumber * value;
	OSNumber * max;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	printf("AppleBacklight: %u properties written on %s\n", dict->getCount(), getMetaClass()->getClassName());
	showProperties(dict, 1);

	params = OSDynamicCast(OSDictionary, dict->getObject("IODisplayParameters"));
	bright = params != NULL ? OSDynamicCast(OSDictionary, params->getObject("brightness")) : NULL;
	value = bright != NULL ? OSDynamicCast(OSNumber, bright->getObject("value")) : NULL;
	max = bright != NULL ? OSDynamicCast(OSNumber, bright->getObject("max")) : NULL;
	if (value != NULL && max != NULL && max->unsigned32BitValue() != 0) {
		uint64_t v = value->unsigned32BitValue();
		uint64_t m = max->unsigned32BitValue();
		uint32_t level = (uint32_t)((v * kPanelMaxLevel + m / 2) / m);

		if (level > kPanelMaxLevel) {
			level = kPanelMaxLevel;
		}
		gPanelLevel = level;
		publishBrightnessParameters(this, level);
		if (gHakiosBacklightLevel != 0) {
			hakios_display_set_brightness(level);
		}
		printf("AppleBacklight:   level %llu of %llu -> panel %#x%s\n", v, m, level,
		    gHakiosBacklightLevel != 0 ? "" : " (not sent: no backlight_dcs=1)");
	}
	return kIOReturnSuccess;
}

IOReturn
MiniBacklightService::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties, IOUserClient ** handler)
{
	MiniAppleBacklightUserClient * client;

	/*
	 * printf, not IOLog: this is the one line that says the window server found the service at
	 * all, and it has to sit in the same buffer the loader fetches the log from.
	 */
	printf("AppleBacklight: CLIENT OPENED on %s: type %u, %u properties\n", getMetaClass()->getClassName(),
	    type, properties != NULL ? properties->getCount() : 0);
	gBacklightCalls++;
	if (handler == NULL) {
		return kIOReturnBadArgument;
	}
	client = OSTypeAlloc(MiniAppleBacklightUserClient);
	if (client == NULL || !client->initWithTask(owningTask, securityID, type, properties)) {
		OSSafeReleaseNULL(client);
		return kIOReturnNoResources;
	}
	if (!client->attach(this)) {
		client->release();
		return kIOReturnNoResources;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnNoResources;
	}
	*handler = client;
	return kIOReturnSuccess;
}

bool
MiniAppleBacklightUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties)
{
	return IOUserClient::initWithTask(owningTask, securityID, type, properties);
}

bool
MiniAppleBacklightUserClient::start(IOService * provider)
{
	return provider != NULL && IOUserClient::start(provider);
}

IOReturn
MiniAppleBacklightUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
MiniAppleBacklightUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	uint32_t in = args != NULL ? args->scalarInputCount : 0;

	if (args == NULL) {
		return kIOReturnBadArgument;
	}
	printf("AppleBacklight: selector %u: %u scalars in [%#llx %#llx %#llx %#llx %#llx %#llx], %u out; "
	    "structure %u in, %u out\n", selector, in,
	    in > 0 ? args->scalarInput[0] : 0, in > 1 ? args->scalarInput[1] : 0,
	    in > 2 ? args->scalarInput[2] : 0, in > 3 ? args->scalarInput[3] : 0,
	    in > 4 ? args->scalarInput[4] : 0, in > 5 ? args->scalarInput[5] : 0,
	    args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	if (in > 0) {
		/* The level is a float in every brightness protocol of this family: say what it is as one. */
		union {
			uint64_t bits;
			float f[2];
		} v = { args->scalarInput[0] };
		printf("AppleBacklight:   ... as a float: %f\n", v.f[0]);
	}
	/*
	 * A client that speaks in a structure rather than in scalars would otherwise be a line that says
	 * "structure 16 in" and nothing else. The first thirty-two bytes are what the next version needs,
	 * and they cost nothing to keep: this path is taken once or twice a run.
	 */
	if (args->structureInput != NULL) {
		for (uint32_t i = 0; i < args->structureInputSize && i < 32; i += 8) {
			uint64_t word = 0;
			uint32_t left = args->structureInputSize - i;

			memcpy(&word, (const uint8_t *)args->structureInput + i, left > 8 ? 8 : left);
			printf("AppleBacklight:   structure in [%u]: %#llx\n", i, word);
		}
	}
	/* The caller reads its outputs only if the method succeeded, so they had better be defined. */
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	/*
	 * A method with nothing in and a scalar out is the "get" side of a level protocol; answer with the
	 * level the panel is at, as the factor the "set" side takes (0..1). Zero would tell the caller the
	 * screen is at its darkest, and the next thing it might do is apply that.
	 */
	if (in == 0 && args->scalarOutputCount >= 1) {
		union {
			float f;
			uint32_t bits;
		} v;

		v.f = (float)gPanelLevel / 2047.0f;
		args->scalarOutput[0] = v.bits;
	}
	/*
	 * backlight_dcs=1: the first scalar of a one-scalar method is taken as the level, which is the
	 * shape every brightness user client of this family has (a level, and nothing else). A value that
	 * reads as a float in 0..1 is one of those factors and is scaled to the panel's 0..0x7ff; anything
	 * else is taken as the level itself. What it really is, the log says; this is what the run is for.
	 */
	if (gHakiosBacklightLevel != 0 && in >= 1) {
		union {
			uint64_t bits;
			float f[2];
		} v = { args->scalarInput[0] };
		uint32_t level = (uint32_t)args->scalarInput[0];

		if (v.f[0] >= 0.0f && v.f[0] <= 1.0f) {
			level = (uint32_t)(v.f[0] * 0x7ff + 0.5f);
		}
		printf("AppleBacklight:   ... sent to the panel as level %#x\n", level);
		gPanelLevel = level;
		/* the level a reader sees is the level the panel has, not the one published at start */
		publishBrightnessParameters(getProvider(), level);
		hakios_display_set_brightness(level);
	}
	return kIOReturnSuccess;
}

uint32_t gHakiosBacklightLevel;         /* backlight_dcs=1: send levels to the panel */

static void
publishBacklightClass(IOService * provider, bool arm)
{
	IOService * service;

	service = arm ? (IOService *) OSTypeAlloc(AppleARMBacklight) : (IOService *) OSTypeAlloc(AppleBacklight);
	if (service == NULL || !service->init() || !service->attach(provider)) {
		OSSafeReleaseNULL(service);
		return;
	}
	service->setName(arm ? "AppleARMBacklight" : "AppleBacklight");
	service->start(provider);
	gBacklightPublished++;
	OSSafeReleaseNULL(service);
}

static void startReportThread(void);

/*
 * From the BSD side, where printf goes into the run's log (the platform expert starts too early for
 * IOLog to be kept): whether the service is there, and whether the window server has ever asked
 * for its user client. Those two numbers say whether a brightness that does nothing is "the service
 * is missing" or "the service is there and nobody talks to it", which is the whole question.
 */
extern "C" void
hakios_backlight_report(void)
{
	printf("AppleBacklight: %u services published, the user client asked for %u times\n", gBacklightPublished,
	    gBacklightCalls);
	startReportThread();    /* BSD context: the one place where a thread may safely be made this early */
}

/*
 * ... and again while the system runs. The first report lands in a boot whose early kernel log
 * the loader has already overwritten by the time anyone reads it, and the user client is only
 * asked for once the window server is up. Twenty seconds apart, a minute's worth in each log
 * fetch, so neither the "found" nor the "still nobody" answer can slip between two fetches.
 */
static bool gReportThreadStarted;

static void
backlightReportThread(void * parameter __unused, wait_result_t result __unused)
{
	for (int i = 0; i < 60; i++) {
		delay_for_interval(20, NSEC_PER_SEC);
		hakios_backlight_report();
	}
	thread_terminate(current_thread());
}

static void
startReportThread(void)
{
	thread_t thread;

	if (gReportThreadStarted) {
		return;
	}
	gReportThreadStarted = true;
	if (kernel_thread_start(backlightReportThread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
MiniAppleBacklightStart(IOService * provider)
{
	uint32_t on = 0, arm = 0, dcs = 0;

	if (!PE_parse_boot_argn("backlight", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (PE_parse_boot_argn("backlight_arm", &arm, sizeof(arm)) && arm != 0) {
	}
	if (PE_parse_boot_argn("backlight_dcs", &dcs, sizeof(dcs)) && dcs != 0) {
		gHakiosBacklightLevel = 1;
	}
	publishBacklightClass(provider, false);
	if (arm != 0) {
		publishBacklightClass(provider, true);
	}
	/* The report thread is started from BSD (hakios_backlight_report), not here: this runs before the scheduler will take a new thread. */
}
