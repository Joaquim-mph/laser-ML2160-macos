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

// Open the classic printer interface (class 7, protocol 1/2) and return it plus its
// bulk-OUT pipe reference.
static IOUSBInterfaceInterface **open_printer_interface(IOUSBDeviceInterface **dev, UInt8 *pipeOut) {
    (*dev)->USBDeviceOpenSeize(dev);   // best effort; ignore if already open

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

        UInt8 cls = 0, sub = 0, proto = 0, altNum = 0;
        (*intf)->GetInterfaceClass(intf, &cls);
        (*intf)->GetInterfaceSubClass(intf, &sub);
        (*intf)->GetInterfaceProtocol(intf, &proto);
        (*intf)->GetAlternateSetting(intf, &altNum);
        logmsg("  iface class=%u sub=%u proto=%u alt=%u", cls, sub, proto, altNum);
        if (cls == 7 && (proto == 1 || proto == 2)) {      // printer class, classic (not IPP-USB proto 4)
            IOReturn ir = (*intf)->USBInterfaceOpenSeize(intf);
            if (ir == kIOReturnSuccess) {
                (*intf)->SetAlternateInterface(intf, altNum);   // make sure it's active
                UInt8 n = 0; (*intf)->GetNumEndpoints(intf, &n);
                for (UInt8 pipe = 1; pipe <= n; pipe++) {
                    UInt8 dir = 0, num = 0, tt = 0, interval = 0; UInt16 mps = 0;
                    (*intf)->GetPipeProperties(intf, pipe, &dir, &num, &tt, &mps, &interval);
                    if (dir == kUSBOut && tt == kUSBBulk) {
                        *pipeOut = pipe; result = intf;
                        logmsg("  -> using bulk-out pipe %u (ep 0x%02x)", pipe, num);
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
