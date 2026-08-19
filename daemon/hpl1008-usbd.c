// hpl1008-usbd: native macOS USB bridge for the HP Laser 1003-1008.
//
// Receives an SPL3/QPDL job (produced by the patched SpliX rastertoqpdl CUPS filter)
// on 127.0.0.1:9108 and writes it to the printer's bulk-OUT endpoint using Apple's
// IOKit USB framework. No Python, no libusb, no Homebrew: just system frameworks.
//
// Runs as root (a LaunchDaemon): macOS attaches a driver to the printer interface and
// only root can Seize it, and only root can drive USB on recent macOS at all.
//
// Build: clang -O2 -o hpl1008-usbd hpl1008-usbd.c -framework IOKit -framework CoreFoundation
// Usage: hpl1008-usbd            (run as the socket daemon)
//        hpl1008-usbd <file>     (one-shot: write <file> to the printer, for testing)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define HP_VID  0x03F0
#define PORT    9108
#define LOGPATH "/private/tmp/hpl1008-daemon.log"

// In CUPS backend mode we log to stderr instead of a file: the backend sandbox blocks
// writes to /private/tmp, but CUPS captures backend stderr into error_log. Lines prefixed
// with "DEBUG:" (and "ERROR:") are parsed by cupsd, so diagnostics survive the sandbox.
static int g_backend = 0;

static void logmsg(const char *fmt, ...) {
    va_list ap;
    if (g_backend) {
        fputs("DEBUG: hpl100x: ", stderr);
        va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
        fputc('\n', stderr); fflush(stderr);
        return;
    }
    FILE *f = fopen(LOGPATH, "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32]; strftime(ts, sizeof ts, "%F %T", localtime(&t));
    fprintf(f, "%s ", ts);
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Find the first HP (vendor 0x03F0) USB device, returning an opened device interface.
// Matches both the modern (IOUSBHostDevice) and legacy (IOUSBDevice) class names and
// checks the vendor id from the device descriptor (robust across macOS versions).
static IOUSBDeviceInterface **find_device(void) {
    const char *classes[] = { "IOUSBHostDevice", kIOUSBDeviceClassName };
    for (int ci = 0; ci < 2; ci++) {
        CFMutableDictionaryRef match = IOServiceMatching(classes[ci]);
        if (!match) continue;
        io_iterator_t iter = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
            continue;
        io_service_t svc;
        while ((svc = IOIteratorNext(iter))) {
            IOCFPlugInInterface **plugin = NULL; SInt32 score = 0;
            IOUSBDeviceInterface **dev = NULL;
            if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                    kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS && plugin) {
                (*plugin)->QueryInterface(plugin,
                    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
                (*plugin)->Release(plugin);
            }
            IOObjectRelease(svc);
            if (dev) {
                UInt16 vid = 0, pid = 0;
                (*dev)->GetDeviceVendor(dev, &vid);
                (*dev)->GetDeviceProduct(dev, &pid);
                if (vid == HP_VID) {
                    logmsg("found device %04x:%04x via %s", vid, pid, classes[ci]);
                    IOObjectRelease(iter);
                    return dev;
                }
                (*dev)->Release(dev);
            }
        }
        IOObjectRelease(iter);
    }
    return NULL;
}

// From the config descriptor find the classic printer interface: class 7, protocol 1 or 2,
// with a bulk-OUT endpoint. This printer is dual-mode: interface 0 has alt 0 = 7/1/2
// (classic raw printing) and alt 1 = 7/1/4 (IPP-over-USB). macOS often leaves interface 0
// on the IPP-USB alt, so the live interface iterator only shows proto 4. The config
// descriptor lists every alt setting, so we can find the classic one and switch to it.
static int find_classic_iface(IOUSBDeviceInterface **dev, UInt8 *ifaceNum, UInt8 *alt) {
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg) != kIOReturnSuccess || !cfg) return 0;
    const unsigned char *p = (const unsigned char *)cfg;
    int total = p[2] | (p[3] << 8);
    int curCls = -1, curProto = -1; UInt8 curIf = 0, curAlt = 0;
    for (int i = 0; i + 2 <= total; ) {
        int len = p[i], type = p[i + 1];
        if (len == 0) break;
        if (type == 4 && i + 9 <= total) {                         // interface descriptor
            curIf = p[i + 2]; curAlt = p[i + 3]; curCls = p[i + 5]; curProto = p[i + 7];
        } else if (type == 5 && i + 6 <= total) {                  // endpoint descriptor
            int addr = p[i + 2], isBulk = (p[i + 3] & 3) == 2, isOut = !(addr & 0x80);
            if (curCls == 7 && (curProto == 1 || curProto == 2) && isBulk && isOut) {
                *ifaceNum = curIf; *alt = curAlt; return 1;        // first classic bulk-OUT wins
            }
        }
        i += len;
    }
    return 0;
}

// Open the classic printer interface and return it plus its bulk-OUT pipe reference. When
// the config descriptor tells us which interface/alt is the classic one, we match that
// interface by NUMBER (its live nub may currently be on the IPP-USB alt) and force it back
// to the classic alt with SetAlternateInterface before looking for the bulk-OUT pipe.
static IOUSBInterfaceInterface **open_printer_interface(IOUSBDeviceInterface **dev, UInt8 *pipeOut) {
    (*dev)->USBDeviceOpenSeize(dev);   // best effort; ignore if already open

    UInt8 wantIf = 0, wantAlt = 0;
    int haveTarget = find_classic_iface(dev, &wantIf, &wantAlt);
    if (haveTarget) logmsg("  classic printer iface = num %u alt %u (from config descriptor)", wantIf, wantAlt);
    else            logmsg("  no classic iface in config descriptor; falling back to proto match");

    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    io_iterator_t iter = 0;
    if ((*dev)->CreateInterfaceIterator(dev, &req, &iter) != kIOReturnSuccess) return NULL;

    IOUSBInterfaceInterface **result = NULL;
    io_service_t usbIf;
    while ((usbIf = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plugin = NULL; SInt32 score = 0;
        IOUSBInterfaceInterface **intf = NULL;
        if (IOCreatePlugInInterfaceForService(usbIf, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS && plugin) {
            (*plugin)->QueryInterface(plugin,
                CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&intf);
            (*plugin)->Release(plugin);
        }
        IOObjectRelease(usbIf);
        if (!intf) continue;

        UInt8 cls = 0, sub = 0, proto = 0, altNum = 0, ifNum = 0;
        (*intf)->GetInterfaceClass(intf, &cls);
        (*intf)->GetInterfaceSubClass(intf, &sub);
        (*intf)->GetInterfaceProtocol(intf, &proto);
        (*intf)->GetAlternateSetting(intf, &altNum);
        (*intf)->GetInterfaceNumber(intf, &ifNum);
        logmsg("  iface num=%u class=%u sub=%u proto=%u alt=%u", ifNum, cls, sub, proto, altNum);

        // Target the classic interface by number when we know it (its nub may be on the
        // IPP-USB alt right now); otherwise fall back to the old class/proto match.
        int isTarget = haveTarget ? (ifNum == wantIf) : (cls == 7 && (proto == 1 || proto == 2));
        if (isTarget) {
            IOReturn ir = (*intf)->USBInterfaceOpenSeize(intf);
            if (ir == kIOReturnSuccess) {
                UInt8 useAlt = haveTarget ? wantAlt : altNum;
                IOReturn ar = (*intf)->SetAlternateInterface(intf, useAlt);   // force classic (raw) alt
                logmsg("  SetAlternateInterface(%u) -> 0x%08x", useAlt, ar);
                UInt8 n = 0; (*intf)->GetNumEndpoints(intf, &n);
                for (UInt8 pipe = 1; pipe <= n; pipe++) {
                    UInt8 dir = 0, num = 0, tt = 0, interval = 0; UInt16 mps = 0;
                    (*intf)->GetPipeProperties(intf, pipe, &dir, &num, &tt, &mps, &interval);
                    if (dir == kUSBOut && tt == kUSBBulk) {
                        *pipeOut = pipe; result = intf;
                        logmsg("  -> using bulk-out pipe %u (ep 0x%02x) on iface %u alt %u", pipe, num, ifNum, useAlt);
                        break;
                    }
                }
                if (result) break;
                (*intf)->USBInterfaceClose(intf);
            } else {
                logmsg("  seize failed 0x%08x", ir);
            }
        }
        (*intf)->Release(intf);
    }
    IOObjectRelease(iter);
    return result;
}

static int usb_write(const unsigned char *data, size_t len) {
    for (int attempt = 1; attempt <= 5; attempt++) {
        IOUSBDeviceInterface **dev = find_device();
        if (!dev) { logmsg("printer not found; waiting..."); sleep(2); continue; }
        UInt8 pipe = 0;
        IOUSBInterfaceInterface **intf = open_printer_interface(dev, &pipe);
        if (!intf) {
            logmsg("no classic bulk-out interface (attempt %d)", attempt);
            (*dev)->USBDeviceClose(dev); (*dev)->Release(dev); sleep(2); continue;
        }
        IOReturn r = (*intf)->WritePipe(intf, pipe, (void *)data, (UInt32)len);
        int rc = (r == kIOReturnSuccess) ? 0 : 1;
        if (rc) logmsg("WritePipe failed 0x%08x (attempt %d)", r, attempt);
        else    logmsg("printed ok: wrote %zu bytes (pipe %u)", len, pipe);
        (*intf)->USBInterfaceClose(intf); (*intf)->Release(intf);
        (*dev)->USBDeviceClose(dev); (*dev)->Release(dev);
        if (!rc) return 0;
        sleep(2);
    }
    logmsg("giving up after retries");
    return 1;
}

// Dump the raw configuration descriptor: every interface, every alternate setting, and
// every endpoint. macOS only creates a nub for each interface's *active* alt setting, so
// the live interface iterator can miss the classic 7/1/2 bulk interface when the printer
// is currently in IPP-USB (7/1/4) mode. The config descriptor lists them all.
static int usb_probe(void) {
    IOUSBDeviceInterface **dev = find_device();
    if (!dev) { printf("printer not found (run with sudo)\n"); return 1; }
    UInt8 nconf = 0; (*dev)->GetNumberOfConfigurations(dev, &nconf);
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg) != kIOReturnSuccess || !cfg) {
        printf("cannot read config descriptor\n"); (*dev)->Release(dev); return 1;
    }
    const unsigned char *p = (const unsigned char *)cfg;
    int total = p[2] | (p[3] << 8);                 // wTotalLength (LE)
    printf("configs=%u  bConfigurationValue=%u  bNumInterfaces=%u  wTotalLength=%d\n",
           nconf, p[5], p[4], total);
    for (int i = 0; i + 2 <= total; ) {
        int len = p[i], type = p[i + 1];
        if (len == 0) break;
        if (type == 4 && i + 9 <= total)            // interface descriptor
            printf("  IFACE num=%u alt=%u  class=%u sub=%u proto=%u  nEndpoints=%u\n",
                   p[i+2], p[i+3], p[i+5], p[i+6], p[i+7], p[i+4]);
        else if (type == 5 && i + 6 <= total) {     // endpoint descriptor
            int addr = p[i+2], attr = p[i+3] & 3;
            const char *tt = attr==2?"bulk":attr==3?"intr":attr==1?"iso":"ctrl";
            printf("      EP 0x%02x %-3s %s\n", addr, (addr & 0x80) ? "IN" : "OUT", tt);
        }
        i += len;
    }
    (*dev)->Release(dev);
    return 0;
}

static unsigned char *read_all(int fd, size_t *outlen) {
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) { cap *= 2; unsigned char *nb = realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    *outlen = len;
    return buf;
}

int main(int argc, char **argv) {
    // --- probe mode: dump the USB config descriptor (needs root) ---
    if (argc == 2 && (!strcmp(argv[1], "probe") || !strcmp(argv[1], "--probe")))
        return usb_probe();

    // --- CUPS backend mode (installed as /usr/libexec/cups/backend/hpl100x) ---
    // Deletes the socket + LaunchDaemon: CUPS invokes this as the device transport
    // stage directly. argv = job user title copies options [file]; job data on stdin.
    //
    // Detecting backend mode is subtle: for a PRINT job CUPS sets argv[0] to the DEVICE
    // URI ("hpl100x:/"), not the executable path, so a basename match fails (it ends in
    // '/'). Match the whole argv[0] AND the DEVICE_URI env var (set to the queue's URI for
    // every backend invocation) so both discovery (argv[0]=path) and printing are caught.
    const char *uri = getenv("DEVICE_URI");
    int as_backend = strstr(argv[0], "hpl100x") != NULL ||
                     (uri && strstr(uri, "hpl100x") != NULL);
    if (as_backend) {
        g_backend = 1;     // route all logmsg() to stderr (sandbox blocks the /tmp log)
        if (argc == 1) {   // discovery
            printf("direct hpl100x:/ \"HP Laser 1008a\" \"HP Laser 1003-1008 (native IOKit)\" "
                   "\"MFG:HP;MDL:HP Laser 1003-1008;\"\n");
            return 0;
        }
        int infd = 0;                                  // stdin by default
        if (argc >= 7 && argv[6][0]) { infd = open(argv[6], O_RDONLY); if (infd < 0) { logmsg("backend: cannot open %s", argv[6]); return 1; } }
        size_t len = 0; unsigned char *data = read_all(infd, &len);
        if (infd) close(infd);
        logmsg("backend: %zu bytes", len);
        int rc = (data && len) ? usb_write(data, len) : 1;
        free(data);
        return rc ? 1 : 0;                             // CUPS_BACKEND_FAILED / CUPS_BACKEND_OK
    }

    if (argc == 2) {                                   // one-shot test mode
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror("open"); return 2; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *d = malloc(sz); fread(d, 1, sz, f); fclose(f);
        int rc = usb_write(d, sz); free(d);
        return rc;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { logmsg("bind failed"); return 1; }
    listen(s, 8);
    logmsg("native IOKit usb daemon listening on 127.0.0.1:%d", PORT);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        size_t len = 0;
        unsigned char *data = read_all(c, &len);
        logmsg("received %zu QPDL bytes", len);
        if (data && len) usb_write(data, len);
        free(data); close(c);
    }
}
