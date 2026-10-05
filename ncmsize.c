/*
 * ncmsize: shows or sets the largest block (NCM NTB) a Realtek USB Ethernet
 * adapter may send to the Mac, with the NCM request SET_NTB_INPUT_SIZE, while
 * Apple's driver stays attached and keeps reading into buffers of the size it
 * chose (InputSize).  README.md explains why.
 *
 * Before sending the request, ncmsize checks the USB ID, then the chip
 * version read from the adapter, and that the size is no larger than the
 * driver's reads: a larger block overflows a read, and the RTL8156B then hangs
 * until it is unplugged.  The adapter itself rejects sizes below 2048 bytes
 * (the NCM minimum) and above its own maximum.  The NCM specification allows
 * the request only while the data interface is idle; the RTL8156B accepts it
 * while running.
 *
 * -d run watches for new adapters (IOKit), interface changes (a kernel event
 * socket) and wakes (IORegisterForSystemPower), checks the adapters 1, 4 and
 * 10 seconds after each, and once a minute in any case.  -d on writes a
 * launchd agent that runs it at every login.
 *
 * Build: clang -O2 -o ncmsize ncmsize.c -framework IOKit -framework CoreFoundation
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/net_kev.h>
#include <sys/ioctl.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <mach/mach_error.h>
#include <mach-o/dyld.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/usb/IOUSBLib.h>

/* CDC NCM 1.0 */
#define NCM_SUBCLASS		0x0d	/* communication interface subclass */
#define NCM_FUNCTIONAL_DESC	0x1a	/* functional descriptor subtype */
#define NCM_NTB_INPUT_SIZE_8	0x20	/* bmNetworkCapabilities: 8-byte form */
#define GET_NTB_INPUT_SIZE	0x85
#define SET_NTB_INPUT_SIZE	0x86

/* Realtek register read, as get_registers() in Linux's r8152 driver */
#define RTL_REQ_GET_REGS	0x05
#define RTL_MCU_TYPE_PLA	0x0100
#define RTL_PLA_TCR0		0xe610	/* its upper half, PLA_TCR1, holds the version */
#define RTL_VERSION_MASK	0x7cf0

#define MAX_ADAPTERS		16

/* Watch mode */
#define SAFETY_CHECK		60.0	/* seconds between checks without any event */
#define MAX_PENDING		32

/* Background mode */
#define AGENT_LABEL		"local.ncmsize"
#define AGENT_PLIST		"Library/LaunchAgents/" AGENT_LABEL ".plist"
#define AGENT_LOG		"Library/Logs/ncmsize.log"

/* USB IDs of Realtek RTL815x adapters, from the tables of Linux's r8152 driver and Realtek's */
static const struct {
	uint16_t vid, pid;
} rtl_ids[] = {
	{ 0x0bda, 0x8050 }, { 0x0bda, 0x8053 }, { 0x0bda, 0x8152 }, { 0x0bda, 0x8153 },
	{ 0x0bda, 0x8155 }, { 0x0bda, 0x8156 }, { 0x0bda, 0x8157 }, { 0x0bda, 0x815a },
	{ 0x045e, 0x07ab }, { 0x045e, 0x07c6 }, { 0x045e, 0x0927 }, { 0x045e, 0x0c5e },
	{ 0x04e8, 0xa101 },
	{ 0x17ef, 0x304f }, { 0x17ef, 0x3052 }, { 0x17ef, 0x3054 }, { 0x17ef, 0x3057 },
	{ 0x17ef, 0x3062 }, { 0x17ef, 0x3069 }, { 0x17ef, 0x3082 }, { 0x17ef, 0x3098 },
	{ 0x17ef, 0x7205 }, { 0x17ef, 0x720a }, { 0x17ef, 0x720b }, { 0x17ef, 0x720c },
	{ 0x17ef, 0x7214 }, { 0x17ef, 0x721e }, { 0x17ef, 0x8153 }, { 0x17ef, 0xa359 },
	{ 0x17ef, 0xa387 },
	{ 0x13b1, 0x0041 }, { 0x0955, 0x09ff }, { 0x2357, 0x0601 }, { 0x2357, 0x0602 },
	{ 0x2001, 0xb301 }, { 0x413c, 0xb097 }, { 0x0b05, 0x1976 }, { 0x0b05, 0x1d91 }, { 0x20f4, 0xe02b },
};

/* Chip names for -c, by the version in PLA_TCR1 (versions as in Linux's r8152) */
static const struct chip {
	const char *name;
	const char *label;
	uint16_t version[4];
} chips[] = {
	{ "rtl8156b", "RTL8156B", { 0x7400, 0x7410 } },
	{ "rtl8157", "RTL8157", { 0x1030 } },
	{ "rtl8159", "RTL8159", { 0x2020 } },
	{ "rtl8156", "original RTL8156", { 0x7020, 0x7030 } },
	{ "rtl8153", "RTL8153", { 0x5c00, 0x5c10, 0x5c20, 0x5c30 } },
	{ "rtl8153b", "RTL8153B", { 0x6000, 0x6010 } },
	{ "rtl8153c", "RTL8153C", { 0x6400 } },
	{ "rtl8153d", "RTL8153D", { 0x7420 } },
	{ "rtl8152", "RTL8152", { 0x4c00, 0x4c10 } },
	{ "rtl8050", "RTL8050", { 0x4800 } },
};
#define NCHIPS		(sizeof(chips) / sizeof(chips[0]))
#define NVERSIONS	(sizeof(chips[0].version) / sizeof(chips[0].version[0]))

#define MAX_TARGETS	32

/* An adapter run by Apple's NCM driver (AppleUSBNCMData) */
struct adapter {
	io_service_t usb;		/* its IOUSBHostDevice */
	IOUSBDeviceInterface **dev;
	char name[IFNAMSIZ];		/* its network interface */
	unsigned int vid, pid;
	unsigned int release;		/* bcdDevice: tells an RTL8156BG from an RTL8156B */
	int known;			/* a Realtek adapter: its USB ID is in rtl_ids */
	unsigned int version;
	const struct chip *chip;	/* its name, if known */
	unsigned int read_size;		/* the driver's read size (InputSize) */
};

/* What the current configuration's descriptors say */
struct ncm {
	int ifnum;			/* NCM control interface */
	uint16_t size_len;		/* GET/SET_NTB_INPUT_SIZE length: 4 or 8 */
};

/* Settings from the command line */
static uint16_t targets[MAX_TARGETS] = { 0x7400, 0x7410 };	/* rtl8156 */
static int ntargets = 2;
static char picked[128] = "RTL8156B";
static const char *chips_arg;
static const char *want_if;
static unsigned int want;

/* Watch mode state */
static int watching;
static struct {
	char name[IFNAMSIZ];
	char line[256];
} said[MAX_ADAPTERS];
static char watched[MAX_ADAPTERS][IFNAMSIZ];
static int nwatched;
static CFAbsoluteTime pending[MAX_PENDING], next_safety;
static int npending;
static io_connect_t root_port;
static int started;

static int usage(void)
{
	fputs("usage: ncmsize [-b <bytes>] [-c <chips>] [-i <interface>]\n"
	      "       ncmsize -d [on|run] -b <bytes> [-c <chips>] [-i <interface>]\n"
	      "       ncmsize -d off|status\n"
	      "  without -b:         list the adapters, their chip and block size\n"
	      "  -b 24572:           set blocks of 16 full-size frames, full speed\n"
	      "  -c rtl8156b:        RTL8156B adapters (the default, tested)\n"
	      "  -c rtl8157:         RTL8157 adapters (untested)\n"
	      "  -c rtl8159:         RTL8159 adapters (untested)\n"
	      "  -c rtl8156:         original RTL8156 adapters (untested)\n"
	      "  -c 0x1030:          adapters whose chip reports this version\n"
	      "  -c rtl8156b,rtl8157 several, separated by commas\n"
	      "  -i en7:             only the adapter behind en7\n"
	      "  -d, -d on:          keep the size set in the background, from every\n"
	      "                      login; running it again replaces the settings\n"
	      "  -d off:             stop that\n"
	      "  -d status:          say if it runs, with which settings\n"
	      "  -d run:             keep the size set, in this terminal\n", stderr);
	return 2;
}

/* Prints a line, with the time first in watch mode */
static void vsay(FILE *f, const char *fmt, va_list ap)
{
	char stamp[32];
	time_t t;

	if (watching) {
		t = time(NULL);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", localtime(&t));
		fputs(stamp, f);
	}
	vfprintf(f, fmt, ap);
	fputc('\n', f);
	fflush(f);
}

static void say(FILE *f, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsay(f, fmt, ap);
	va_end(ap);
}

/* In watch mode, what was last said about an adapter, so it is said once */
static int same_as_last(const char *name, const char *line)
{
	int i, free_slot = -1;

	for (i = 0; i < MAX_ADAPTERS; i++) {
		if (!strcmp(said[i].name, name)) {
			if (!strcmp(said[i].line, line))
				return 1;
			strlcpy(said[i].line, line, sizeof(said[i].line));
			return 0;
		}
		if (!said[i].name[0] && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		strlcpy(said[free_slot].name, name, sizeof(said[free_slot].name));
		strlcpy(said[free_slot].line, line, sizeof(said[free_slot].line));
	}
	return 0;
}

/* Says something about an adapter; in watch mode, only when it changes */
static void report(const struct adapter *a, FILE *f, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (watching && same_as_last(a->name, line))
		return;
	say(f, "%s", line);
}

static int number_property(io_registry_entry_t entry, CFStringRef key, unsigned int *val)
{
	CFTypeRef v = IORegistryEntryCreateCFProperty(entry, key, NULL, 0);
	int ok = v && CFGetTypeID(v) == CFNumberGetTypeID() &&
		 CFNumberGetValue(v, kCFNumberIntType, val);

	if (v)
		CFRelease(v);
	return ok ? 0 : -1;
}

/* Reads the USB ID; returns 1 if it is a Realtek adapter's */
static int known_id(io_service_t usb, unsigned int *vid, unsigned int *pid)
{
	size_t i;

	*vid = *pid = 0;
	if (number_property(usb, CFSTR(kUSBHostMatchingPropertyVendorID), vid) ||
	    number_property(usb, CFSTR(kUSBHostMatchingPropertyProductID), pid))
		return 0;
	for (i = 0; i < sizeof(rtl_ids) / sizeof(rtl_ids[0]); i++)
		if (rtl_ids[i].vid == *vid && rtl_ids[i].pid == *pid)
			return 1;
	return 0;
}

/* Fills in the adapter that an instance of Apple's NCM driver runs */
static int ncm_driver(io_service_t ncm, struct adapter *a)
{
	io_registry_entry_t e, parent;
	CFTypeRef name;
	int found;

	memset(a, 0, sizeof(*a));
	if (number_property(ncm, CFSTR("InputSize"), &a->read_size))
		return 0;
	name = IORegistryEntrySearchCFProperty(ncm, kIOServicePlane, CFSTR(kIOBSDNameKey), NULL,
					       kIORegistryIterateRecursively);
	found = name && CFGetTypeID(name) == CFStringGetTypeID() &&
		CFStringGetCString(name, a->name, sizeof(a->name), kCFStringEncodingUTF8);
	if (name)
		CFRelease(name);
	if (!found)
		return 0;

	/* the adapter is the first USB device above the driver, not a hub further up */
	e = ncm;
	IOObjectRetain(e);
	while (IORegistryEntryGetParentEntry(e, kIOServicePlane, &parent) == KERN_SUCCESS) {
		IOObjectRelease(e);
		e = parent;
		if (IOObjectConformsTo(e, kIOUSBHostDeviceClassName)) {
			a->usb = e;
			a->known = known_id(e, &a->vid, &a->pid);
			number_property(e, CFSTR(kUSBHostMatchingPropertyDeviceReleaseNumber), &a->release);
			return 1;
		}
	}
	IOObjectRelease(e);
	return 0;
}

/* All adapters run by macOS's NCM driver; returns their number or -1 */
static int find_adapters(struct adapter *list)
{
	io_iterator_t it;
	io_service_t ncm;
	int n = 0;

	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleUSBNCMData"),
					 &it) != KERN_SUCCESS) {
		say(stderr, "cannot list the adapters run by macOS's NCM driver");
		return -1;
	}
	while ((ncm = IOIteratorNext(it))) {
		if (n < MAX_ADAPTERS && ncm_driver(ncm, &list[n]))
			n++;
		IOObjectRelease(ncm);
	}
	IOObjectRelease(it);
	return n;
}

static int open_device(struct adapter *a)
{
	IOCFPlugInInterface **plug = NULL;
	SInt32 score;
	kern_return_t kr;

	kr = IOCreatePlugInInterfaceForService(a->usb, kIOUSBDeviceUserClientTypeID,
					       kIOCFPlugInInterfaceID, &plug, &score);
	if (kr != kIOReturnSuccess || !plug) {
		report(a, stderr, "%s: cannot open the adapter: %s (0x%08x)", a->name,
		       mach_error_string(kr), (unsigned int)kr);
		return -1;
	}
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
				(LPVOID *)&a->dev);
	(*plug)->Release(plug);
	if (!a->dev) {
		report(a, stderr, "%s: cannot open the adapter: no USB device interface", a->name);
		return -1;
	}
	return 0;
}

static void close_device(struct adapter *a)
{
	if (a->dev)
		(*a->dev)->Release(a->dev);
	a->dev = NULL;
	IOObjectRelease(a->usb);
}

static int request(struct adapter *a, uint8_t type, uint8_t req, uint16_t value,
		   uint16_t index, void *buf, uint16_t len)
{
	IOUSBDevRequest r = {
		.bmRequestType = type,
		.bRequest = req,
		.wValue = value,
		.wIndex = index,
		.wLength = len,
		.pData = buf,
	};
	IOReturn kr = (*a->dev)->DeviceRequest(a->dev, &r);

	if (kr != kIOReturnSuccess) {
		/* the adapter stalls a size it does not accept; run() says so */
		if (req == SET_NTB_INPUT_SIZE && kr == kIOUSBPipeStalled)
			return -2;
		report(a, stderr, "%s: USB request 0x%02x failed: %s (0x%08x)", a->name, req,
		       mach_error_string(kr), (unsigned int)kr);
		return -1;
	}
	if (r.wLenDone < len) {
		report(a, stderr, "%s: USB request 0x%02x: short reply, %u of %u bytes", a->name,
		       req, (unsigned int)r.wLenDone, (unsigned int)len);
		return -1;
	}
	return 0;
}

static const struct chip *chip_of(unsigned int ver)
{
	size_t i, j;

	for (i = 0; i < NCHIPS; i++)
		for (j = 0; j < NVERSIONS; j++)
			if (chips[i].version[j] && chips[i].version[j] == ver)
				return &chips[i];
	return NULL;
}

/* Reads the chip version, as __rtl_get_hw_ver() in Linux's r8152 does */
static int identify_chip(struct adapter *a)
{
	uint32_t b;

	if (request(a, USBmakebmRequestType(kUSBIn, kUSBVendor, kUSBDevice), RTL_REQ_GET_REGS,
		    RTL_PLA_TCR0, RTL_MCU_TYPE_PLA, &b, sizeof(b)))
		return -1;
	a->version = OSReadLittleInt16(&b, 2) & RTL_VERSION_MASK;
	a->chip = chip_of(a->version);
	return 0;
}

static const char *label(const struct adapter *a)
{
	static char buf[48];
	const char *name = a->chip ? a->chip->label : "unknown chip";

	/*
	 * bcdDevice 0x3100 is the RTL8156B; the later 0x31xx values (0x3104,
	 * 0x3105, Ugreen's 0x31f4) are RTL8156BG adapters
	 */
	if (a->chip && !strcmp(a->chip->name, "rtl8156b") && (a->release & 0xff00) == 0x3100 &&
	    a->release != 0x3100)
		name = "RTL8156BG";
	snprintf(buf, sizeof(buf), "%s, 0x%04x", name, a->version);
	return buf;
}

/* The -c argument that picks an adapter's chip */
static const char *pick_arg(const struct adapter *a)
{
	static char buf[16];

	if (a->chip)
		return a->chip->name;
	snprintf(buf, sizeof(buf), "0x%04x", a->version);
	return buf;
}

static int targeted(unsigned int ver)
{
	int i;

	for (i = 0; i < ntargets; i++)
		if (targets[i] == ver)
			return 1;
	return 0;
}

/*
 * Reads the current configuration's descriptors from the copy macOS keeps,
 * without any USB request.  Returns 0, -1 when the configuration has no NCM
 * interface, or -2 when the descriptors cannot be read.
 */
static int find_ncm(struct adapter *a, struct ncm *n)
{
	IOUSBDeviceInterface **dev = a->dev;
	IOUSBConfigurationDescriptorPtr cd;
	const uint8_t *p, *end;
	unsigned int config;
	int cls = -1, sub = -1, intf = -1;
	UInt8 count, i, cur;

	if (number_property(a->usb, CFSTR(kUSBHostDevicePropertyCurrentConfiguration), &config)) {
		if ((*dev)->GetConfiguration(dev, &cur) != kIOReturnSuccess)
			return -2;
		config = cur;
	}
	if ((*dev)->GetNumberOfConfigurations(dev, &count) != kIOReturnSuccess)
		return -2;
	for (i = 0; i < count; i++)
		if ((*dev)->GetConfigurationDescriptorPtr(dev, i, &cd) == kIOReturnSuccess &&
		    cd->bConfigurationValue == config)
			break;
	if (i == count)
		return config ? -2 : -1;

	n->ifnum = -1;
	n->size_len = 0;
	p = (const uint8_t *)cd;
	end = p + USBToHostWord(cd->wTotalLength);
	for (; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
		if (p[1] == kUSBInterfaceDesc && p[0] >= 9) {
			intf = p[2];
			cls = p[5];
			sub = p[6];
		} else if (p[1] == kUSBClassSpecificDescriptor && p[0] >= 6 &&
			   p[2] == NCM_FUNCTIONAL_DESC &&
			   cls == kUSBCommunicationControlInterfaceClass && sub == NCM_SUBCLASS) {
			n->ifnum = intf;
			n->size_len = (p[5] & NCM_NTB_INPUT_SIZE_8) ? 8 : 4;
		}
	}
	return n->ifnum >= 0 ? 0 : -1;
}

static int get_size(struct adapter *a, const struct ncm *n, unsigned int *size)
{
	uint32_t b[2];

	if (request(a, USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface), GET_NTB_INPUT_SIZE,
		    0, n->ifnum, b, n->size_len))
		return -1;
	*size = OSReadLittleInt32(b, 0);
	return 0;
}

static int set_size(struct adapter *a, const struct ncm *n, unsigned int size)
{
	/* dwNtbInMaxSize; in the 8-byte form, wNtbInMaxDatagrams 0 (no limit) */
	uint32_t b[2] = { 0 };

	OSWriteLittleInt32(b, 0, size);
	return request(a, USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface),
		       SET_NTB_INPUT_SIZE, 0, n->ifnum, b, n->size_len);
}

/* 1 if the network interface is up, 0 if down, -1 if unknown */
static int interface_up(const char *name)
{
	struct ifreq ifr;
	int s, up = -1;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	if (!ioctl(s, SIOCGIFFLAGS, &ifr))
		up = !!(ifr.ifr_flags & IFF_UP);
	close(s);
	return up;
}

/* The size argument: decimal digits only, 32 bits like the request's field */
static int parse_size(const char *s, unsigned int *size)
{
	unsigned long v;
	char *end;

	if (*s < '0' || *s > '9')
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (*end || errno || v > UINT32_MAX)
		return -1;
	*size = (unsigned int)v;
	return 0;
}

/* The -c argument: chip names or versions (0x...), separated by commas */
static int parse_chips(const char *arg)
{
	char *copy = strdup(arg), *s = copy, *tok, *end;
	unsigned long v;
	size_t i, j;
	int ret = 0;

	ntargets = 0;
	picked[0] = 0;
	while (!ret && (tok = strsep(&s, ","))) {
		for (i = 0; i < NCHIPS && strcmp(tok, chips[i].name); i++)
			;
		if (i < NCHIPS) {
			for (j = 0; j < NVERSIONS && chips[i].version[j] && ntargets < MAX_TARGETS; j++)
				targets[ntargets++] = chips[i].version[j];
			snprintf(picked + strlen(picked), sizeof(picked) - strlen(picked), "%s%s",
				 picked[0] ? " or " : "", chips[i].label);
			continue;
		}
		ret = -1;
		if (strncmp(tok, "0x", 2) || !tok[2])
			break;
		errno = 0;
		v = strtoul(tok + 2, &end, 16);
		if (*end || errno || !v || v & ~(unsigned long)RTL_VERSION_MASK ||
		    ntargets == MAX_TARGETS)
			break;
		targets[ntargets++] = (uint16_t)v;
		snprintf(picked + strlen(picked), sizeof(picked) - strlen(picked), "%schip 0x%04lx",
			 picked[0] ? " or " : "", v);
		ret = 0;
	}
	free(copy);
	return ret;
}

static int is_mode(const char *s)
{
	return !strcmp(s, "on") || !strcmp(s, "off") || !strcmp(s, "run") || !strcmp(s, "status");
}

/* Shows or sets the block size of one adapter; returns an exit status */
static int run(struct adapter *a, int set)
{
	struct ncm n;
	unsigned int before, now;
	const char *note = "";
	int up, ret = find_ncm(a, &n);

	if (ret == -1) {
		report(a, stderr, "%s: not in NCM mode", a->name);
		return 1;
	}
	if (ret == -2) {
		report(a, stderr, "%s: cannot read its USB descriptors", a->name);
		return 1;
	}

	if (!set) {
		char skip[48] = "";

		if (get_size(a, &n, &before))
			return 1;
		if (!targeted(a->version))
			snprintf(skip, sizeof(skip), "; -b skips it without -c %s", pick_arg(a));
		report(a, stdout, "%s (%s): blocks of up to %u bytes (macOS sets %u)%s%s", a->name,
		       label(a), before, a->read_size, interface_up(a->name) == 0 ?
		       "; the interface is down, macOS sets its own size when it comes up" : "",
		       skip);
		return 0;
	}

	if (want > a->read_size) {
		report(a, stderr, "%s: refused: Apple's driver reads %u-byte buffers, and a "
		       "larger block hangs the adapter", a->name, a->read_size);
		return 2;
	}
	/* macOS sets its own size when the interface comes up */
	up = interface_up(a->name);
	if (watching && up != 1) {
		report(a, stderr, "%s: the interface is down; waiting for it to come up", a->name);
		return 2;
	}
	if (up == 0)
		note = "; the interface is down, macOS sets its own size when it comes up";

	if (get_size(a, &n, &before))
		return 1;
	if (before == want) {
		report(a, stdout, "%s (%s): blocks of up to %u bytes already%s", a->name, label(a),
		       before, note);
		return 0;
	}
	ret = set_size(a, &n, want);
	if (ret == -2)
		report(a, stderr, "%s: the adapter refused blocks of %u bytes", a->name, want);
	if (ret)
		return ret == -2 ? 2 : 1;
	if (get_size(a, &n, &now)) {
		report(a, stderr, "%s: asked for %u bytes (was %u), but reading the size back failed",
		       a->name, want, before);
		return 1;
	}
	if (now != want) {
		report(a, stderr, "%s: asked for %u bytes, but the adapter reports %u (was %u)",
		       a->name, want, now, before);
		return 1;
	}
	report(a, stdout, "%s (%s): blocks of up to %u bytes, was %u%s", a->name, label(a), now,
	       before, note);
	/* in watch mode, the next check finding the size set says nothing */
	if (watching) {
		char line[256];

		snprintf(line, sizeof(line), "%s (%s): blocks of up to %u bytes already", a->name,
			 label(a), now);
		same_as_last(a->name, line);
	}
	return 0;
}

/* Shows or sets the block size of every adapter picked; returns an exit status */
static int run_all(int set)
{
	struct adapter list[MAX_ADAPTERS];
	char found[256] = "", names[128] = "";
	int count, i, r, ret = 0, done = 0;
	/* without -b and -c, every adapter is shown, whatever its chip */
	int all = !set && !chips_arg;

	count = find_adapters(list);
	if (count < 0)
		return 1;
	nwatched = 0;
	for (i = 0; i < count; i++) {
		struct adapter *a = &list[i];

		snprintf(names + strlen(names), sizeof(names) - strlen(names), " %s", a->name);
		if (want_if && strcmp(a->name, want_if)) {
			close_device(a);
			continue;
		}
		r = 0;
		if (!a->known) {
			/* nothing is sent to an adapter ncmsize doesn't know */
			if (all || want_if) {
				report(a, set ? stderr : stdout, "%s (USB ID %04x:%04x): not a Realtek "
				       "adapter ncmsize knows; left alone", a->name, a->vid, a->pid);
				if (set)
					r = 2;
				else
					done++;
			}
		} else if (open_device(a) || identify_chip(a)) {
			r = 1;
		} else {
			snprintf(found + strlen(found), sizeof(found) - strlen(found), "%s%s (%s)",
				 *found ? ", " : "", a->name, label(a));
			if (targeted(a->version)) {
				strlcpy(watched[nwatched++], a->name, IFNAMSIZ);
				r = run(a, set);
				done++;
			} else if (!set && (all || want_if)) {
				r = run(a, 0);	/* with the -c that -b needs for it */
				done++;
			} else if (want_if) {
				report(a, stderr, "%s: its chip is %s; pick it with -c %s", a->name,
				       label(a), pick_arg(a));
				r = 2;
			}
		}
		if (r > ret)
			ret = r;
		close_device(a);
	}

	if (!done && !ret) {
		if (watching)
			return 0;	/* nothing to do until an adapter shows up */
		if (want_if)
			say(stderr, "no Realtek adapter behind %s; plugged in:%s", want_if,
			    *names ? names : " none");
		else if (*found)
			say(stderr, "no adapter with %s plugged in; found: %s", picked, found);
		else if (all)
			say(stderr, "no adapter run by macOS's NCM driver is plugged in");
		else
			say(stderr, "no adapter with %s run by macOS's NCM driver is plugged in",
			    picked);
		return 1;
	}
	return ret;
}

/* Watch mode: checks 1, 4 and 10 seconds after an event */
static void schedule_checks(void)
{
	static const double delays[] = { 1, 4, 10 };
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	size_t i;

	for (i = 0; i < sizeof(delays) / sizeof(delays[0]) && npending < MAX_PENDING; i++)
		pending[npending++] = now + delays[i];
}

static void tick(CFRunLoopTimerRef timer, void *info)
{
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	int i, j, due = 0;

	(void)timer;
	(void)info;
	for (i = j = 0; i < npending; i++) {
		if (pending[i] <= now)
			due = 1;
		else
			pending[j++] = pending[i];
	}
	npending = j;
	if (due || now >= next_safety) {
		next_safety = now + SAFETY_CHECK;
		run_all(1);
	}
}

/* Apple's NCM driver attached to a device (or was already attached at start) */
static void matched(void *ctx, io_iterator_t it)
{
	io_object_t o;
	int n = 0;

	(void)ctx;
	while ((o = IOIteratorNext(it))) {
		IOObjectRelease(o);
		n++;
	}
	if (n && started)
		say(stdout, "an adapter was plugged in");
	if (n)
		schedule_checks();
}

/* A network interface changed: flags (up, down), link, or a new interface */
static void kernel_event(CFFileDescriptorRef fdref, CFOptionFlags flags, void *info)
{
	int fd = CFFileDescriptorGetNativeDescriptor(fdref);
	uint32_t buf[256];
	struct kern_event_msg *m = (struct kern_event_msg *)buf;
	struct net_event_data *d;
	static char last[64];
	static CFAbsoluteTime last_time;
	char name[IFNAMSIZ], line[64];
	ssize_t len;
	int i;

	(void)flags;
	(void)info;
	while ((len = recv(fd, buf, sizeof(buf), 0)) >= (ssize_t)(KEV_MSG_HEADER_SIZE + sizeof(*d))) {
		if (m->kev_class != KEV_NETWORK_CLASS || m->kev_subclass != KEV_DL_SUBCLASS ||
		    (m->event_code != KEV_DL_SIFFLAGS && m->event_code != KEV_DL_LINK_ON &&
		     m->event_code != KEV_DL_IF_ATTACHED))
			continue;
		d = (struct net_event_data *)m->event_data;
		snprintf(name, sizeof(name), "%s%u", d->if_name, d->if_unit);
		if (m->event_code == KEV_DL_IF_ATTACHED && !strncmp(name, "en", 2)) {
			say(stdout, "%s appeared", name);
			schedule_checks();
			continue;
		}
		for (i = 0; i < nwatched; i++)
			if (!strcmp(watched[i], name)) {
				/* one line for a burst of identical events */
				snprintf(line, sizeof(line), "%s: %s", name,
					 m->event_code == KEV_DL_LINK_ON ? "link up" :
					 "interface flags changed");
				if (strcmp(line, last) || CFAbsoluteTimeGetCurrent() - last_time > 2)
					say(stdout, "%s", line);
				strlcpy(last, line, sizeof(last));
				last_time = CFAbsoluteTimeGetCurrent();
				schedule_checks();
			}
	}
	CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
}

static void power_event(void *ctx, io_service_t service, natural_t type, void *arg)
{
	(void)ctx;
	(void)service;
	switch (type) {
	case kIOMessageCanSystemSleep:
	case kIOMessageSystemWillSleep:
		IOAllowPowerChange(root_port, (long)arg);
		break;
	case kIOMessageSystemHasPoweredOn:
		say(stdout, "the Mac woke up");
		schedule_checks();
		break;
	}
}

static int watch(void)
{
	struct kev_request kev = { KEV_VENDOR_APPLE, KEV_NETWORK_CLASS, KEV_DL_SUBCLASS };
	IONotificationPortRef port, power_port;
	io_object_t power_notifier;
	io_iterator_t it;
	CFFileDescriptorRef fdref;
	CFRunLoopSourceRef src;
	CFRunLoopTimerRef timer;
	CFRunLoopRef loop = CFRunLoopGetCurrent();
	int fd;

	watching = 1;
	say(stdout, "ncmsize: keeping blocks of up to %u bytes on %s adapters%s%s", want, picked,
	    want_if ? " behind " : "", want_if ? want_if : "");

	port = IONotificationPortCreate(kIOMainPortDefault);
	CFRunLoopAddSource(loop, IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
	if (IOServiceAddMatchingNotification(port, kIOFirstMatchNotification,
					     IOServiceMatching("AppleUSBNCMData"), matched, NULL,
					     &it) != KERN_SUCCESS) {
		say(stderr, "cannot watch for adapters");
		return 1;
	}
	matched(NULL, it);
	started = 1;

	fd = socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT);
	if (fd < 0 || ioctl(fd, SIOCSKEVFILT, &kev) || fcntl(fd, F_SETFL, O_NONBLOCK)) {
		say(stderr, "cannot watch network interfaces: %s", strerror(errno));
		return 1;
	}
	fdref = CFFileDescriptorCreate(NULL, fd, true, kernel_event, NULL);
	CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
	src = CFFileDescriptorCreateRunLoopSource(NULL, fdref, 0);
	CFRunLoopAddSource(loop, src, kCFRunLoopDefaultMode);

	root_port = IORegisterForSystemPower(NULL, &power_port, power_event, &power_notifier);
	if (root_port == MACH_PORT_NULL) {
		say(stderr, "cannot watch for sleep and wake");
		return 1;
	}
	CFRunLoopAddSource(loop, IONotificationPortGetRunLoopSource(power_port),
			   kCFRunLoopDefaultMode);

	next_safety = 0;	/* first check at the first tick */
	timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1, 0, 0, tick, NULL);
	CFRunLoopAddTimer(loop, timer, kCFRunLoopDefaultMode);
	CFRunLoopRun();
	return 0;
}

/*
 * Background mode: runs launchctl and waits for it; returns its exit status.
 * With out, keeps what it prints there (cut at len).
 */
static int launchctl_out(const char *a1, const char *a2, const char *a3, char *out, size_t len)
{
	extern char **environ;
	char *argv[] = { "launchctl", (char *)a1, (char *)a2, (char *)a3, NULL };
	posix_spawn_file_actions_t fa;
	size_t got = 0;
	ssize_t n;
	pid_t pid;
	int status, pipefd[2] = { -1, -1 };

	if (out && pipe(pipefd))
		return -1;
	posix_spawn_file_actions_init(&fa);
	if (out) {
		posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
		posix_spawn_file_actions_addclose(&fa, pipefd[0]);
		posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	} else {
		posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	}
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	status = posix_spawn(&pid, "/bin/launchctl", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (out) {
		close(pipefd[1]);
		while (!status && (n = read(pipefd[0], out + got, len - 1 - got)) > 0)
			got += (size_t)n;
		out[got] = 0;
		/* drain whatever did not fit */
		while (!status && read(pipefd[0], &pipefd[1], sizeof(pipefd[1])) > 0)
			;
		close(pipefd[0]);
	}
	if (status || waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int launchctl(const char *a1, const char *a2, const char *a3)
{
	return launchctl_out(a1, a2, a3, NULL, 0);
}

static void home_path(char *buf, size_t len, const char *rel)
{
	const char *home = getenv("HOME");

	snprintf(buf, len, "%s/%s", home ? home : "", rel);
}

static void add_string(CFMutableArrayRef array, const char *s)
{
	CFStringRef str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);

	CFArrayAppendValue(array, str);
	CFRelease(str);
}

static int write_agent(const char *plist, const char *log)
{
	char exe[PATH_MAX], real[PATH_MAX], size[16];
	uint32_t exe_len = sizeof(exe);
	CFMutableDictionaryRef d;
	CFMutableArrayRef args;
	CFStringRef logstr;
	CFDataRef data;
	FILE *f;
	int ok;

	if (_NSGetExecutablePath(exe, &exe_len) || !realpath(exe, real)) {
		say(stderr, "cannot find where ncmsize itself is");
		return -1;
	}
	snprintf(size, sizeof(size), "%u", want);
	args = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	add_string(args, real);
	add_string(args, "-d");
	add_string(args, "run");
	add_string(args, "-b");
	add_string(args, size);
	if (chips_arg) {
		add_string(args, "-c");
		add_string(args, chips_arg);
	}
	if (want_if) {
		add_string(args, "-i");
		add_string(args, want_if);
	}

	logstr = CFStringCreateWithCString(NULL, log, kCFStringEncodingUTF8);
	d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
				      &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(d, CFSTR("Label"), CFSTR(AGENT_LABEL));
	CFDictionarySetValue(d, CFSTR("ProgramArguments"), args);
	CFDictionarySetValue(d, CFSTR("RunAtLoad"), kCFBooleanTrue);
	CFDictionarySetValue(d, CFSTR("KeepAlive"), kCFBooleanTrue);
	CFDictionarySetValue(d, CFSTR("StandardOutPath"), logstr);
	CFDictionarySetValue(d, CFSTR("StandardErrorPath"), logstr);
	data = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
	CFRelease(d);
	CFRelease(args);
	CFRelease(logstr);
	if (!data)
		return -1;

	f = fopen(plist, "w");
	ok = f && fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), f) ==
		  (size_t)CFDataGetLength(data);
	if (f && fclose(f))
		ok = 0;
	CFRelease(data);
	if (!ok)
		say(stderr, "cannot write %s: %s", plist, strerror(errno));
	return ok ? 0 : -1;
}

/* -d on: (re)installs the launchd agent; -d off: removes it */
static int background(int off)
{
	char plist[PATH_MAX], log[PATH_MAX], dir[PATH_MAX], domain[32], service[64];
	int i, loaded;

	home_path(plist, sizeof(plist), AGENT_PLIST);
	home_path(log, sizeof(log), AGENT_LOG);
	snprintf(domain, sizeof(domain), "gui/%u", getuid());
	snprintf(service, sizeof(service), "%s/%s", domain, AGENT_LABEL);

	loaded = launchctl("bootout", service, NULL) == 0;
	if (off) {
		if (unlink(plist) && errno == ENOENT && !loaded) {
			say(stdout, "ncmsize was not running in the background");
			return 0;
		}
		say(stdout, "ncmsize stopped and removed from the background");
		return 0;
	}

	home_path(dir, sizeof(dir), "Library/LaunchAgents");
	mkdir(dir, 0755);
	home_path(dir, sizeof(dir), "Library/Logs");
	mkdir(dir, 0755);
	if (write_agent(plist, log))
		return 1;
	/* a service still being torn down refuses a bootstrap for a moment */
	for (i = 0; i < 20; i++) {
		if (!launchctl("bootstrap", domain, plist)) {
			say(stdout, "ncmsize %s in the background: blocks of up to %u bytes on %s "
			    "adapters%s%s, from every login", loaded ? "restarted" : "started", want,
			    picked, want_if ? " behind " : "", want_if ? want_if : "");
			say(stdout, "log: %s; stop it with: ncmsize -d off", log);
			return 0;
		}
		usleep(250000);
	}
	say(stderr, "launchctl bootstrap %s %s failed", domain, plist);
	return 1;
}

/* The value after a "key = " line of launchctl print, or NULL */
static const char *print_field(const char *text, const char *key, char *buf, size_t len)
{
	const char *p = text, *e;
	size_t klen = strlen(key);

	while ((p = strstr(p, key))) {
		if ((p == text || p[-1] == '\t' || p[-1] == ' ') && !strncmp(p + klen, " = ", 3)) {
			p += klen + 3;
			e = strchr(p, '\n');
			if (!e)
				e = p + strlen(p);
			snprintf(buf, len, "%.*s", (int)(e - p), p);
			return buf;
		}
		p += klen;
	}
	return NULL;
}

/* -d status: is the launchd agent installed and running, and with which settings */
static int background_status(void)
{
	char plist[PATH_MAX], log[PATH_MAX], service[64], text[16384], state[64], pid[32];
	char settings[512] = "", program[PATH_MAX] = "", line[512] = "", last[512] = "";
	CFArrayRef args = NULL;
	CFPropertyListRef pl = NULL;
	CFDataRef data = NULL;
	CFIndex i, n;
	FILE *f;
	int running = 0;

	home_path(plist, sizeof(plist), AGENT_PLIST);
	home_path(log, sizeof(log), AGENT_LOG);
	snprintf(service, sizeof(service), "gui/%u/%s", getuid(), AGENT_LABEL);

	f = fopen(plist, "r");
	if (!f) {
		say(stdout, "ncmsize is not set to run in the background; "
		    "start it with: ncmsize -d -b 24572");
		return 1;
	}
	n = (CFIndex)fread(text, 1, sizeof(text), f);
	fclose(f);
	data = CFDataCreate(NULL, (const UInt8 *)text, n);
	if (data)
		pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
	if (pl && CFGetTypeID(pl) == CFDictionaryGetTypeID())
		args = CFDictionaryGetValue(pl, CFSTR("ProgramArguments"));
	if (args && CFGetTypeID(args) == CFArrayGetTypeID()) {
		n = CFArrayGetCount(args);
		for (i = 0; i < n; i++) {
			CFStringRef a = CFArrayGetValueAtIndex(args, i);
			char arg[PATH_MAX];

			if (CFGetTypeID(a) != CFStringGetTypeID() ||
			    !CFStringGetCString(a, arg, sizeof(arg), kCFStringEncodingUTF8))
				continue;
			if (i == 0)
				strlcpy(program, arg, sizeof(program));
			else if (i > 2)	/* after the program, -d and run */
				snprintf(settings + strlen(settings), sizeof(settings) - strlen(settings),
					 "%s%s", *settings ? " " : "", arg);
		}
	}
	if (pl)
		CFRelease(pl);
	if (data)
		CFRelease(data);

	if (!launchctl_out("print", service, NULL, text, sizeof(text)) &&
	    print_field(text, "state", state, sizeof(state)) && !strcmp(state, "running") &&
	    print_field(text, "pid", pid, sizeof(pid)))
		running = 1;
	if (running)
		say(stdout, "ncmsize runs in the background (pid %s), with: %s", pid, settings);
	else
		say(stdout, "ncmsize is set to run in the background with: %s, but is not "
		    "running; restart it with: ncmsize -d %s", settings, settings);
	if (*program && access(program, X_OK))
		say(stdout, "its program, %s, is missing: run ncmsize -d again from where "
		    "ncmsize is now", program);

	f = fopen(log, "r");
	if (f) {
		while (fgets(line, sizeof(line), f))
			strlcpy(last, line, sizeof(last));
		fclose(f);
		last[strcspn(last, "\n")] = 0;
		if (*last)
			say(stdout, "last in %s: %s", log, last);
	}
	return running ? 0 : 1;
}

int main(int argc, char **argv)
{
	const char *mode = NULL;
	int opt, set = 0;

	while ((opt = getopt(argc, argv, "b:c:di:")) != -1) {
		switch (opt) {
		case 'b':
			if (parse_size(optarg, &want))
				return usage();
			set = 1;
			break;
		case 'c':
			chips_arg = optarg;
			if (parse_chips(optarg))
				return usage();
			break;
		case 'd':
			/* -d alone means -d on; any other word is left for the checks below */
			mode = "on";
			if (optind < argc && is_mode(argv[optind]))
				mode = argv[optind++];
			break;
		case 'i':
			want_if = optarg;
			break;
		default:
			return usage();
		}
	}
	if (optind < argc)
		return usage();

	if (mode && (!strcmp(mode, "off") || !strcmp(mode, "status"))) {
		if (set || chips_arg || want_if)
			return usage();
		return !strcmp(mode, "off") ? background(1) : background_status();
	}
	if (mode && !set)
		return usage();

	if (mode && !strcmp(mode, "on"))
		return background(0);
	if (mode)
		return watch();
	return run_all(set);
}
