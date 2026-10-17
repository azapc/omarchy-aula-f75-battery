/*
 * aula_f75_battery - read AULA F75 battery level over Linux hidraw.
 *
 * The keyboard talks through a Compx 2.4G receiver (3554:fa09), which declares
 * no HID Battery System usage page. There is no power_supply node and UPower
 * never sees it, so the level has to be asked for over a vendor HID channel.
 *
 * Prints one JSON object on stdout. Exit codes separate the two empty cases,
 * because they mean opposite things to the widget:
 *   0  battery emitted
 *   1  receiver present, no reply - keyboard asleep. Hold the last reading.
 *   2  usage error, or stdout could not be written
 *   3  receiver absent - unplugged, or in wired mode under its other USB id.
 *      Drop the reading.
 *   4  receiver present but not openable - the udev rule is missing.
 *
 * The hidraw device scan (hidraw_number, monotonic_ms and the /dev walk)
 * follows hl.peripheral_battery by hlasensky, used under the MIT License:
 * https://github.com/hlasensky/omarchy-peripheral-battery
 * Its scan in turn derives from scbat by Gregoire Delattre (MIT):
 * https://github.com/gregdel/scbat
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define AULA_VENDOR_ID  0x3554
#define AULA_PRODUCT_ID 0xfa09

/* Vendor command pipe on interface 1 (usage page 0xFF02). Declared as both
 * Input and Output, which is what makes request/response possible. */
#define VENDOR_REPORT_ID   0x13
#define VENDOR_REPORT_LEN  20  /* 19 payload bytes + 1 ID byte */

/* Vendor feature report (usage page 0xFF04). Holds a static config block, not
 * the battery; kept for --info because it is useful when probing. */
#define FEATURE_REPORT_ID  0x06
#define FEATURE_REPORT_LEN 8

/*
 * Battery request/response. The high bit of the command is a read flag, so
 * 0xCA asks for what 0x4A describes.
 *
 *   13 4a 01 00 02 5a 01 00 00 00 00 00 00 00 00 00 00 00 00 bb
 *   |  |  |  |  |  |  |                                      |
 *   |  |  |  |  |  |  +- state: 0x01 on battery, 0x10 charging
 *   |  |  |  |  |  +---- percent (0x5a = 90)
 *   |  |  |  |  +------- payload byte count
 *   |  |  |  +---------- sequence, for multi-frame replies
 *   |  |  +------------- length / page
 *   |  +---------------- command echo (request & 0x7f)
 *   +------------------- report id
 *                        checksum: sum(bytes 0..18) & 0xff ---+
 */
#define BATTERY_CMD       0xCA
#define BATTERY_REPLY     0x4A
#define REPLY_COUNT_AT    4
#define BATTERY_PCT_AT    5
#define BATTERY_STATE_AT  6

#define STATE_CHARGING    0x10

/* A sleeping keyboard does not answer the first request: the write wakes the
 * radio and only a later one gets a reply. */
#define REPLY_WAIT_MS     400
#define REQUEST_ATTEMPTS  6

#define RAW_DUMP_MS        4000
#define MAX_READS_PER_WAKE 64

/*
 * The vendor pipe shares interface 1 with the keyboard's own input collections
 * (0x03 system, 0x04 NKRO keystrokes, 0x05 consumer, 0x07 consumer), so a raw
 * dump would print everything typed. --raw drops these unless --raw-all says
 * otherwise.
 */
static const uint8_t input_report_ids[] = {0x03, 0x04, 0x05, 0x07};

static int is_input_report(uint8_t id)
{
    size_t i;

    for (i = 0; i < sizeof(input_report_ids); i++)
        if (input_report_ids[i] == id)
            return 1;
    return 0;
}

static int hidraw_number(const char *name)
{
    const char *cursor;
    int number = 0;

    if (strncmp(name, "hidraw", 6) != 0 || name[6] == '\0')
        return -1;

    for (cursor = name + 6; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9')
            return -1;
        number = number * 10 + *cursor - '0';
    }

    return number;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* The receiver exposes two hidraw nodes and only interface 1 carries the pipe.
 * Match on the descriptor, since hidraw numbering shifts across replugs. */
static int has_vendor_pipe(int fd)
{
    struct hidraw_report_descriptor descriptor;
    uint32_t size = 0;
    uint32_t i;

    if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size == 0)
        return 0;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.size = size;
    if (ioctl(fd, HIDIOCGRDESC, &descriptor) < 0)
        return 0;

    /* Usage Page (Vendor 0xFF02) encodes as: 06 02 ff */
    for (i = 0; i + 2 < descriptor.size; i++)
        if (descriptor.value[i] == 0x06 && descriptor.value[i + 1] == 0x02
            && descriptor.value[i + 2] == 0xff)
            return 1;

    return 0;
}

/*
 * Is the receiver plugged in, regardless of whether we may open it? sysfs is
 * world-readable, so this answers even when /dev/hidraw* is root-only, which
 * is what separates "no dongle" from "no permission" for the caller.
 */
static int device_in_sysfs(void)
{
    static const char *base = "/sys/class/hidraw";
    struct dirent *entry;
    DIR *directory;
    int found = 0;

    directory = opendir(base);
    if (!directory)
        return 0;

    while (!found && (entry = readdir(directory))) {
        char uevent_path[256];
        char line[256];
        FILE *uevent;

        if (strncmp(entry->d_name, "hidraw", 6) != 0)
            continue;
        if (snprintf(uevent_path, sizeof(uevent_path), "%s/%s/device/uevent",
                     base, entry->d_name) >= (int)sizeof(uevent_path))
            continue;

        uevent = fopen(uevent_path, "r");
        if (!uevent)
            continue;

        /* HID_ID=0003:00003554:0000FA09 */
        while (fgets(line, sizeof(line), uevent))
            if (strstr(line, "HID_ID=")
                && strstr(line, "3554")
                && (strstr(line, "FA09") || strstr(line, "fa09"))) {
                found = 1;
                break;
            }

        fclose(uevent);
    }

    closedir(directory);
    return found;
}

static int open_device(char *path, size_t path_size)
{
    struct dirent *entry;
    DIR *directory;
    int found = -1;

    directory = opendir("/dev");
    if (!directory)
        return -1;

    while (found < 0 && (entry = readdir(directory))) {
        struct hidraw_devinfo info = {0};
        char candidate[64];
        int fd;

        if (hidraw_number(entry->d_name) < 0)
            continue;

        if (snprintf(candidate, sizeof(candidate), "/dev/%s", entry->d_name)
            >= (int)sizeof(candidate))
            continue;

        /* Writing the request needs O_RDWR; fall back so --raw still works if
         * only read access was granted. */
        fd = open(candidate, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            fd = open(candidate, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        /* hidraw_devinfo stores these as __s16, so 0xfa09 arrives negative.
         * Cast before comparing or the match never fires. */
        if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0
            || (uint16_t)info.vendor != AULA_VENDOR_ID
            || (uint16_t)info.product != AULA_PRODUCT_ID
            || !has_vendor_pipe(fd)) {
            close(fd);
            continue;
        }

        snprintf(path, path_size, "%s", candidate);
        found = fd;
    }

    closedir(directory);
    return found;
}

static void print_hex(const uint8_t *data, ssize_t length)
{
    ssize_t i;

    for (i = 0; i < length; i++)
        fprintf(stderr, "%02x ", data[i]);
    fprintf(stderr, "\n");
}

static int emit_battery(unsigned int percentage, const char *state,
                        const char *charging)
{
    if (printf("{\"connected\":true,\"pct\":%u,\"state\":\"%s\","
               "\"charging\":%s,\"id\":\"aula-f75\","
               "\"type\":\"keyboard\",\"model\":\"AULA F75\"}\n",
               percentage, state, charging) < 0)
        return -1;
    return 1;
}

static uint8_t frame_checksum(const uint8_t *frame, size_t length)
{
    unsigned int sum = 0;
    size_t i;

    for (i = 0; i + 1 < length; i++)
        sum += frame[i];
    return (uint8_t)(sum & 0xff);
}

static const char *describe_state(uint8_t state, const char **charging)
{
    if (state == STATE_CHARGING) {
        *charging = "true";
        return "charging";
    }

    /* Unobserved states report as discharging rather than guessing, so an
     * unknown value never claims the keyboard is charging. */
    *charging = "false";
    return "discharging";
}

static int parse_reply(const uint8_t *frame, ssize_t length)
{
    const char *charging;
    const char *state;
    unsigned int percentage;

    if (length < VENDOR_REPORT_LEN)
        return 0;
    if (frame[0] != VENDOR_REPORT_ID || frame[1] != BATTERY_REPLY)
        return 0;
    if (frame[VENDOR_REPORT_LEN - 1]
        != frame_checksum(frame, VENDOR_REPORT_LEN))
        return 0;
    if (frame[REPLY_COUNT_AT] < 2)
        return 0;

    percentage = frame[BATTERY_PCT_AT];
    if (percentage > 100)
        return 0;

    state = describe_state(frame[BATTERY_STATE_AT], &charging);
    return emit_battery(percentage, state, charging);
}

static int request_once(int fd)
{
    uint8_t out[VENDOR_REPORT_LEN];
    struct pollfd watch = {.fd = fd, .events = POLLIN};
    int waited = 0;

    memset(out, 0, sizeof(out));
    out[0] = VENDOR_REPORT_ID;
    out[1] = BATTERY_CMD;
    out[VENDOR_REPORT_LEN - 1] = frame_checksum(out, VENDOR_REPORT_LEN);

    if (write(fd, out, sizeof(out)) < 0)
        return 0;

    while (waited < REPLY_WAIT_MS) {
        int ready = poll(&watch, 1, 50);

        waited += 50;
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            continue;
        if (watch.revents & (POLLERR | POLLHUP | POLLNVAL))
            return 0;

        for (;;) {
            uint8_t frame[64];
            ssize_t length = read(fd, frame, sizeof(frame));
            int emitted;

            if (length <= 0)
                break;
            if (is_input_report(frame[0]))
                continue;

            emitted = parse_reply(frame, length);
            if (emitted != 0)
                return emitted;
        }
    }

    return 0;
}

static int query_battery(int fd)
{
    uint8_t drain_buffer[64];
    int attempt;

    /* Drain once per query, not per attempt. A sleeping keyboard often answers
     * later than one attempt's window, and draining between attempts would
     * discard the very reply this asked for - the same lag the command sweep
     * showed, where an answer lands during the next command's window. */
    while (read(fd, drain_buffer, sizeof(drain_buffer)) > 0)
        ;

    for (attempt = 0; attempt < REQUEST_ATTEMPTS; attempt++) {
        int result = request_once(fd);

        if (result != 0)
            return result;
    }

    return 0;
}

/* Print unsolicited vendor traffic, for working out further commands. */
static void dump_raw(int fd, int raw_all)
{
    struct pollfd watch = {.fd = fd, .events = POLLIN};
    int64_t deadline = monotonic_ms();

    if (deadline < 0)
        return;
    deadline += RAW_DUMP_MS;

    for (;;) {
        int64_t now = monotonic_ms();
        int ready;
        int read_count;

        if (now < 0 || now >= deadline)
            return;

        ready = poll(&watch, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return;
        if (watch.revents & (POLLERR | POLLHUP | POLLNVAL))
            return;

        for (read_count = 0; read_count < MAX_READS_PER_WAKE; read_count++) {
            uint8_t report[64];
            ssize_t length = read(fd, report, sizeof(report));

            if (length <= 0) {
                if (length < 0 && errno == EINTR)
                    continue;
                if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                return;
            }

            if (raw_all || !is_input_report(report[0]))
                print_hex(report, length);
        }
    }
}

static void dump_feature(int fd)
{
    uint8_t buffer[FEATURE_REPORT_LEN];

    memset(buffer, 0, sizeof(buffer));
    buffer[0] = FEATURE_REPORT_ID;
    fprintf(stderr, "feature report 0x%02x: ", FEATURE_REPORT_ID);
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(buffer)), buffer) < 0)
        fprintf(stderr, "GET_FEATURE failed: %s\n", strerror(errno));
    else
        print_hex(buffer, sizeof(buffer));
}

int main(int argc, char **argv)
{
    char path[64] = {0};
    int raw = 0;
    int raw_all = 0;
    int info = 0;
    int present = 0;
    int fd;
    int result;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            raw = 1;
        } else if (strcmp(argv[i], "--raw-all") == 0) {
            raw = 1;
            raw_all = 1;
        } else if (strcmp(argv[i], "--info") == 0) {
            info = 1;
        } else if (strcmp(argv[i], "--present") == 0) {
            present = 1;
        } else {
            fprintf(stderr,
                    "usage: %s [--info] [--raw] [--raw-all] [--present]\n",
                    argv[0]);
            return 2;
        }
    }

    fd = open_device(path, sizeof(path));
    if (fd < 0) {
        if (device_in_sysfs()) {
            if (!present)
                fprintf(stderr,
                        "aula_f75_battery: %04x:%04x is plugged in but cannot "
                        "be opened. Install the udev rule: sudo ./install.sh\n",
                        AULA_VENDOR_ID, AULA_PRODUCT_ID);
            return 4;
        }
        if (!present)
            fprintf(stderr,
                    "aula_f75_battery: no %04x:%04x receiver found "
                    "(is the dongle plugged in?)\n",
                    AULA_VENDOR_ID, AULA_PRODUCT_ID);
        return 3;
    }

    /* Presence only. Costs an open and an ioctl and sends the keyboard
     * nothing, so it is cheap enough to run on a short timer. */
    if (present) {
        close(fd);
        return 0;
    }

    if (info || raw)
        fprintf(stderr, "aula_f75_battery: using %s\n", path);
    if (info)
        dump_feature(fd);

    if (raw) {
        fprintf(stderr, "listening on report 0x%02x for %d ms "
                        "(keystroke reports suppressed%s)\n",
                VENDOR_REPORT_ID, RAW_DUMP_MS,
                raw_all ? " - DISABLED by --raw-all" : "");
        dump_raw(fd, raw_all);
        close(fd);
        return 1;
    }

    result = query_battery(fd);
    close(fd);

    if (result < 0)
        return 2;
    if (result == 0) {
        fprintf(stderr, "aula_f75_battery: no battery reply "
                        "(keyboard asleep or off?)\n");
        return 1;
    }

    return 0;
}
