/*
 * probe_pipe - sweep command bytes on the AULA F75 vendor pipe (report 0x13)
 * and print whatever comes back. Development tool: it ships in the repo
 * but the shell never loads it, and the widget never runs it.
 *
 * The battery command (0xCA) was found this way. Use it to explore whatever
 * else the keyboard exposes.
 *
 * Build:  cc -O2 -std=c11 -Wall -Wextra -o probe_pipe probe_pipe.c
 * Usage:  ./probe_pipe [start] [end]      (hex or decimal, default 0x80 0xff)
 *
 * Replies lag: an answer to command N often arrives during N+1's window, so
 * confirm anything interesting by re-running that command on its own.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AULA_VENDOR_ID   0x3554
#define AULA_PRODUCT_ID  0xfa09
#define VENDOR_REPORT_ID 0x13
#define VENDOR_LEN       20
#define REPLY_WAIT_MS    120

/* Input reports that carry what the user types. Never printed. */
static int is_user_input(uint8_t id)
{
    return id == 0x03 || id == 0x04 || id == 0x05 || id == 0x07;
}

static int open_pipe(void)
{
    struct dirent *entry;
    DIR *dir = opendir("/dev");
    int found = -1;

    if (!dir)
        return -1;

    while (found < 0 && (entry = readdir(dir))) {
        struct hidraw_devinfo info = {0};
        struct hidraw_report_descriptor desc;
        char path[64];
        uint32_t size = 0, i;
        int fd, ok = 0;

        if (strncmp(entry->d_name, "hidraw", 6) != 0
            || strlen(entry->d_name) > sizeof(path) - 6)
            continue;
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);

        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0
            || (uint16_t)info.vendor != AULA_VENDOR_ID
            || (uint16_t)info.product != AULA_PRODUCT_ID) {
            close(fd);
            continue;
        }

        /* Interface 1 only: the one declaring vendor page 0xFF02. */
        if (ioctl(fd, HIDIOCGRDESCSIZE, &size) == 0 && size > 0) {
            memset(&desc, 0, sizeof(desc));
            desc.size = size;
            if (ioctl(fd, HIDIOCGRDESC, &desc) == 0)
                for (i = 0; i + 2 < desc.size; i++)
                    if (desc.value[i] == 0x06 && desc.value[i + 1] == 0x02
                        && desc.value[i + 2] == 0xff)
                        ok = 1;
        }

        if (!ok) {
            close(fd);
            continue;
        }

        fprintf(stderr, "using %s\n", path);
        found = fd;
    }

    closedir(dir);
    return found;
}

/* Drain anything already queued so a reply is not confused with backlog. */
static void drain(int fd)
{
    uint8_t buffer[64];

    while (read(fd, buffer, sizeof(buffer)) > 0)
        ;
}

int main(int argc, char **argv)
{
    unsigned long start = 0x80, end = 0xff;
    int fd, hits = 0;
    unsigned long cmd;

    if (argc > 1) start = strtoul(argv[1], NULL, 0);
    if (argc > 2) end   = strtoul(argv[2], NULL, 0);

    fd = open_pipe();
    if (fd < 0) {
        fprintf(stderr, "no AULA F75 vendor pipe found\n");
        return 1;
    }

    printf("sweeping command bytes 0x%02lx..0x%02lx on report 0x%02x\n\n",
           start, end, VENDOR_REPORT_ID);

    for (cmd = start; cmd <= end; cmd++) {
        uint8_t out[VENDOR_LEN];
        struct pollfd watch = {.fd = fd, .events = POLLIN};
        int waited = 0;

        drain(fd);

        memset(out, 0, sizeof(out));
        out[0] = VENDOR_REPORT_ID;
        out[1] = (uint8_t)cmd;

        if (write(fd, out, sizeof(out)) < 0) {
            printf("  0x%02lx  write failed: %s\n", cmd, strerror(errno));
            continue;
        }

        /* Collect every reply that arrives inside the window, not just the
         * first - some firmware answers with a short ack then the payload. */
        while (waited < REPLY_WAIT_MS) {
            int ready = poll(&watch, 1, 20);

            waited += 20;
            if (ready <= 0)
                continue;

            for (;;) {
                uint8_t in[64];
                ssize_t n = read(fd, in, sizeof(in));
                ssize_t i;

                if (n <= 0)
                    break;
                if (is_user_input(in[0]))
                    continue;

                printf("  cmd 0x%02lx -> ", cmd);
                for (i = 0; i < n; i++)
                    printf("%02x ", in[i]);
                printf("\n");
                hits++;
            }
        }
    }

    printf("\n%d repl%s\n", hits, hits == 1 ? "y" : "ies");
    close(fd);
    return 0;
}
