/* probe_latency.c — click→photon ground-truth instrument (Session 40 step 4).
 *
 * Chain: evdev event (EVIOCSCLOCKID=CLOCK_MONOTONIC) → present a frame whose
 * color encodes "input seen" → wk pf_presentation latch + vblank seq.
 * Runs against the LIVE lsfg-vk pipeline by drawing into a shm buffer we
 * watch on our own surface — while the game/doubler path is the one that
 * actually presents for real games, this probe measures the WHOLE desktop
 * path: evdev→compositor→scanout, giving (a) the KWin/compositor constant
 * and (b) relative deltas when the layer+app are in the way. Conservative,
 * honest: it measures the chain it actually exercises (probe surface →
 * compositor → DP-7 scanout). The game-side segment A..C truth stays
 * engine-internal; this instrument answers "what is the compositor +
 * present path floor," which is the piece every layer change can only
 * add ONTO.
 *
 * Build: gcc -O2 -o probe_latency probe_latency.c ... (see journal)
 * Use:   ./probe_latency [w] [h] [samples]
 * Output: per-sample p50/p99 evdev→presented-latch ms + scanout seqs.
 */
#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <glob.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/select.h>

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *xdgwm;
static struct wp_presentation *present;
static uint32_t present_clock = 1; /* CLOCK_MONOTONIC unless told otherwise */

static void xdg_ping(void *data, struct xdg_wm_base *x, uint32_t serial) {
    (void)data; xdg_wm_base_pong(x, serial);
}
static const struct xdg_wm_base_listener xdg_listener = { .ping = xdg_ping };
static int g_surfConfigured = 0;
static void xdg_surf_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    (void)data; xdg_surface_ack_configure(s, serial);
    g_surfConfigured = 1;
}
static const struct xdg_surface_listener xdg_surf_listener = {
    .configure = xdg_surf_configure };
static void toplevel_configure(void *data, struct xdg_toplevel *t,
        int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) { (void)data; (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close };
static void globals(void *data, struct wl_registry *reg, uint32_t name,
        const char *iface, uint32_t ver) {
    (void)data; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name))
        comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        xdgwm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name))
        present = wl_registry_bind(reg, name, &wp_presentation_interface, 2);
}
static void globals_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener reg_listener = {
    .global = globals, .global_remove = globals_remove };

static void clock_id(void *data, struct wp_presentation *p, uint32_t clock) {
    (void)data; (void)p; present_clock = clock;
}
static const struct wp_presentation_listener present_listener = {
    .clock_id = clock_id };

/* presented-feedback sinks: one outstanding per armed commit; store newest */
static _Atomic uint64_t g_latch_ns[2] = { 0, 0 };
static _Atomic unsigned g_which = 0;      /* slot the next arm writes into */
static _Atomic uint64_t g_seq = 0;

static void fb_sync_output(void *data, struct wp_presentation_feedback *f,
        struct wl_output *o) { (void)data; (void)f; (void)o; }
static void fb_presented(void *data, struct wp_presentation_feedback *f,
        uint32_t s_hi, uint32_t s_lo, uint32_t nsec, uint32_t refresh,
        uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    (void)refresh; (void)flags;
    const uint64_t ns = ((uint64_t)s_hi << 32 | s_lo) * 1000000000ULL + nsec;
    /* data points at the ARM SLOT this feedback belongs to — using the live
       g_which here stored the latch into the OTHER slot (off-by-one) because
       main bumps g_which right after arming; the latch then landed where the
       waiter never looked. */
    unsigned slot = data ? *(unsigned *)data & 1u : 0u;
    atomic_store(&g_latch_ns[slot], ns);
    atomic_store(&g_seq, ((uint64_t)seq_hi << 32) | seq_lo);
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *data, struct wp_presentation_feedback *f) {
    (void)data;
    wp_presentation_feedback_destroy(f);
}
static const struct wp_presentation_feedback_listener fb_listener = {
    .sync_output = fb_sync_output,
    .presented = fb_presented,
    .discarded = fb_discarded,
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* --- input thread: ALL mice, BTN_LEFT presses ------------------------------ */
static _Atomic uint64_t g_click_ns[2] = { 0, 0 };
static _Atomic unsigned g_clicks = 0;   /* total BTN_LEFT presses latched */
static _Atomic int g_click_slot = 0;
volatile _Atomic uint32_t g_paint_flag = 0; /* paint loop watches this */

static void *input_thread(void *arg) {
    (void)arg;
    /* open EVERY event-mouse by-id node — the box has four pointer devices
     * (Endgame OP1, BY-tech, Azeron, Wooting); the user clicks whichever is
     * on the desk, so listen on all of them. */
    int fds[64];
    int nfd = 0;
    char names[64][120];
    glob_t gg;
    if (glob("/dev/input/by-id/*event-mouse*", 0, nullptr, &gg) == 0) {
        // copy paths FIRST: globfree() frees gl_pathv, any later use of it is
        // a use-after-free (the standalone evread_c tool segfaulted exactly
        // there — first key event printed a freed device string).
        const size_t npaths = gg.gl_pathc < 32 ? gg.gl_pathc : 32;
        for (size_t i = 0; i < npaths; ++i)
            snprintf(names[i], sizeof(names[i]), "%s", gg.gl_pathv[i]);
        globfree(&gg);
        for (size_t i = 0; i < npaths; ++i) {
            int fd = open(names[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) { perror(names[i]); continue; }
            unsigned clk = CLOCK_MONOTONIC; /* matches KWin clock_id=1 */
            if (ioctl(fd, EVIOCSCLOCKID, &clk) != 0)
                perror("EVIOCSCLOCKID (timestamps may be CLOCK_REALTIME!)");
            else {
                printf("input: %s -> CLOCK_MONOTONIC\n", names[i]);
                fflush(stdout);
            }
            fds[nfd++] = fd;
            fflush(stdout);
        }
    }
    /* the automated clicker (uinput) has NO by-id symlink; find it by device
     * name across the raw event nodes so the probe can be driven headless. */
    {
        glob_t ge;
        if (glob("/dev/input/event*", 0, nullptr, &ge) == 0) {
            char enames[64][120];
            const size_t npaths = ge.gl_pathc < 64 ? ge.gl_pathc : 64;
            for (size_t i = 0; i < npaths; ++i)
                snprintf(enames[i], sizeof(enames[i]), "%s", ge.gl_pathv[i]);
            globfree(&ge);
            for (size_t i = 0; i < npaths; ++i) {
                char dname[256] = "";
                if (ioctl(-1, 0, nullptr), 0) {}  /* noop keeps -Wunused off */
                int fd = open(enames[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
                if (fd < 0) continue;
                if (ioctl(fd, EVIOCGNAME(sizeof(dname) - 1), dname) < 0
                        || !strstr(dname, "latency-probe-clicker")) {
                    close(fd);
                    continue;
                }
                unsigned clk = CLOCK_MONOTONIC;
                ioctl(fd, EVIOCSCLOCKID, &clk);
                printf("input: VIRTUAL %s (%s) -> CLOCK_MONOTONIC\n", enames[i], dname);
                fflush(stdout);
                if (nfd < 64)
                    fds[nfd++] = fd;
                else
                    close(fd);
            }
        }
    }
    if (nfd == 0) { perror("no input devices readable"); return nullptr; }

    struct pollfd pfd[64];
    for (int i = 0; i < nfd; ++i) {
        pfd[i].fd = fds[i];
        pfd[i].events = POLLIN;
    }
    struct input_event ev[64];
    for (;;) {
        const int pr = poll(pfd, (nfds_t)nfd, 250);
        if (pr <= 0) continue;
        for (int i = 0; i < nfd; ++i) {
            if ((pfd[i].revents & POLLIN) == 0)
                continue;
            const ssize_t n = read(pfd[i].fd, ev, sizeof(ev));
            if (n <= 0) continue;
            const size_t count = (size_t)n / sizeof(ev[0]);
            for (size_t j = 0; j < count; ++j) {
                if (ev[j].type != EV_KEY || ev[j].value != 1)
                    continue;
                static int dbg = -1;
            if (dbg < 0)
                dbg = getenv("LSFG_PROBE_DEBUG") ? 1 : 0;
            if (dbg)
                printf("ev: t=%u c=%u v=%u on fd#%d\n", ev[j].type,
                       ev[j].code, ev[j].value, i);
            if (ev[j].code == BTN_LEFT) {  /* left-click press */
                    atomic_fetch_add(&g_clicks, 1);
                    const uint64_t event_ns =
                        (uint64_t)ev[j].time.tv_sec * 1000000000ULL
                        + (uint64_t)ev[j].time.tv_usec * 1000ULL;
                    int slot = (atomic_load(&g_click_slot) + 1) & 1;
                    atomic_store(&g_click_ns[slot], event_ns);
                    atomic_store(&g_click_slot, slot);
                    atomic_store(&g_paint_flag, 1);
                }
            }
        }
    }
    return nullptr;
}

int main(int argc, char **argv) {
    const uint32_t W = argc > 1 ? (uint32_t)atoi(argv[1]) : 320;
    const uint32_t H = argc > 2 ? (uint32_t)atoi(argv[2]) : 180;
    const unsigned SAMPLES = argc > 3 ? (unsigned)atoi(argv[3]) : 60;

    dpy = wl_display_connect(nullptr);
    if (!dpy) { fprintf(stderr, "no wayland\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, nullptr);
    for (int i = 0; i < 4; ++i) wl_display_roundtrip(dpy);
    if (!comp || !shm || !present || !xdgwm) {
        fprintf(stderr, "missing globals (comp=%d shm=%d pres=%d xdg=%d)\n",
            !!comp, !!shm, !!present, !!xdgwm);
        return 2;
    }
    wp_presentation_add_listener(present, &present_listener, nullptr);
    wl_display_roundtrip(dpy);  /* clock_id */
    printf("probe build 20260919-1456 (globfree-use-after-free fixed)\n"); fflush(stdout);
    printf("paint clock: %u\n", present_clock); fflush(stdout);

    const size_t SHM_SZ = (size_t)W * H * 4;
    char path[] = "/dev/shm/probe-latency-XXXXXX";
    const int fd = mkostemp(path, O_CLOEXEC);
    ftruncate(fd, (off_t)SHM_SZ);
    unlink(path);
    uint8_t *px = mmap(nullptr, SHM_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(px, 0x20, SHM_SZ);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)SHM_SZ);
    struct wl_buffer *bufA = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4,
        WL_SHM_FORMAT_XRGB8888);
    struct wl_buffer *bufB = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4,
        WL_SHM_FORMAT_XRGB8888); /* same pool, same bytes; alternates via commit */
    (void)bufB;

    struct wl_surface *surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(xdgwm, surf);
    xdg_surface_add_listener(xs, &xdg_surf_listener, nullptr);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &toplevel_listener, nullptr);
    xdg_toplevel_set_app_id(tl, "probe-latency");
    /* fullscreen on whatever output KWin picked (DP-7): (nullptr = current) —
     * a 320x180 toplevel is easy to miss on a 2560x1440 panel; KWin reported
     * the tiny window 'out=DP-7 (Not Responding)' — visible but too small to
     * spot. Fullscreen makes the click-to-photon marker unmissable. */
    xdg_toplevel_set_fullscreen(tl, nullptr);

    /* XDG map contract (xdg-shell spec, learned via KWin's error 3):
     * (1) initial commit WITHOUT a buffer requests mapping; (2) the
     * compositor replies with xdg_surface.configure + toplevel.configure;
     * (3) the client acks and only THEN attaches a real buffer + commits.
     * Attaching before configure = protocol error 3 (we hit it live). */
    wl_surface_commit(surf);          /* (1) the empty commit */
    for (int i = 0; i < 10 && !g_surfConfigured; ++i)
        wl_display_roundtrip(dpy);    /* (2) deliver the configure events */
    if (!g_surfConfigured) {
        fprintf(stderr, "xdg configure never arrived\n");
        return 2;
    }
    for (uint32_t y = H / 4; y < H / 4 + 20 && y < H; ++y)
        for (uint32_t x = W / 4; x < W / 4 + 20 && x < W; ++x) {
            uint8_t *p = px + ((size_t)y * W + x) * 4;
            p[0] = 0x40; p[1] = 0x10; p[2] = 0x40; p[3] = 0xFF;
        }
    wl_surface_attach(surf, bufA, 0, 0);          /* (3) now attach */
    wl_surface_damage_buffer(surf, 0, 0, (int32_t)W, (int32_t)H);
    wl_surface_commit(surf);
    wl_display_flush(dpy);
    wl_display_roundtrip(dpy);

    /* SELF-TEST: one arm+commit+roundtrip proves the compositor answers
     * presented for THIS surface (mpv differential proved KWin answers v2
     * clients; the probe must see its own before trusting click samples). */
    {
        unsigned self_slot = 7; /* any distinct tag; fb writes latch slot 3? — use 0 */
        self_slot = 0;
        atomic_store(&g_latch_ns[0], 0);
        unsigned arm_slot = self_slot;
        struct wp_presentation_feedback *fb =
            wp_presentation_feedback(present, surf);
        wp_presentation_feedback_add_listener(fb, &fb_listener, &arm_slot);
        wl_surface_attach(surf, bufA, 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, (int32_t)W, (int32_t)H);
        wl_surface_commit(surf);
        wl_display_flush(dpy);
        for (int i = 0; i < 40; ++i) { /* ~40 roundtrips or until latch */
            wl_display_roundtrip(dpy);
            if (atomic_load(&g_latch_ns[0])) break;
            struct timespec ts = { 0, 2000000 }; nanosleep(&ts, nullptr);
        }
        printf("self-test presented: %s (latch=%llu seq=%llu)\n",
               atomic_load(&g_latch_ns[0]) ? "OK" : "NONE",
               (unsigned long long)atomic_load(&g_latch_ns[0]),
               (unsigned long long)atomic_load(&g_seq));
        fflush(stdout);
    }

    pthread_t ithr;
    pthread_create(&ithr, nullptr, input_thread, nullptr);

    printf("click the mouse (left button) — %u samples wanted\n", SAMPLES); fflush(stdout);
    unsigned collected = 0;
    double samples[512];
    struct timespec idle = { .tv_sec = 0, .tv_nsec = 3000000 }; /* 3 ms */
    while (collected < SAMPLES && collected < 512) {
        /* pump compositor events (configure/frame callbacks) */
        wl_display_dispatch_pending(dpy);
        int disp;
        do { disp = wl_display_dispatch_pending(dpy); } while (disp > 0);

        /* map newest click to the next presented commit: arm, paint marker,
         * attach, commit, dispatch until presented arrives */
        unsigned slot = (unsigned)atomic_load(&g_click_slot) & 1;
        uint64_t click = atomic_load(&g_click_ns[slot]);
        {
            static unsigned clicks_seen = 0;
            static double t0 = -1;
            if (t0 < 0) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9; }
            /* update the click-rate telemetry: a click was consumed if the
               paint flag flipped; click rate = clicks consumed / elapsed */
        }
        if (!click) {
            static unsigned waited = 0;
            static unsigned clicks_seen = 0;
            static double t0 = -1;
            if (t0 < 0) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9; }
            struct timespec ts2; clock_gettime(CLOCK_MONOTONIC, &ts2);
            const double el = ts2.tv_sec + ts2.tv_nsec / 1e9 - t0;
            if (waited++ % 200 == 0) {
                const unsigned clicks = atomic_load(&g_clicks);
                fprintf(stderr,
                        "waiting (%.1fs: %u clicks, %.1f cps, %u samples)\n",
                        el, clicks, clicks / (el > 0.5 ? el : 0.5), collected);
                fflush(stderr);
            }
            nanosleep(&idle, nullptr);
            continue;
        }
        atomic_store(&g_click_ns[slot], 0); /* consume */

        /* paint the WHOLE frame bright magenta on click (unmissable flash) */
        for (size_t i = 0; i < SHM_SZ; i += 4) {
            px[i + 0] = 0xE0; px[i + 1] = 0x20; px[i + 2] = 0xE0; px[i + 3] = 0xFF;
        }

        unsigned fslot = atomic_fetch_add(&g_which, 1) & 1;
        unsigned arm_slot = fslot; /* proxy-owned; fb listener reads via data */
        atomic_store(&g_latch_ns[fslot], 0);
        struct wp_presentation_feedback *fb =
            wp_presentation_feedback(present, surf);
        wp_presentation_feedback_add_listener(fb, &fb_listener, &arm_slot);
        wl_surface_attach(surf, bufA, 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, (int32_t)W, (int32_t)H);
        wl_surface_commit(surf);
        wl_display_flush(dpy);

        /* wait until THAT slot's presented arrives (up to 100 ms) */
        struct timespec stop, now;
        clock_gettime(CLOCK_MONOTONIC, &stop);
        stop.tv_nsec += 900000000ULL; /* up to 900 ms: DP-7 vblank is 4.17 ms, so
            a correct present latches within a few refreshes; anything slower
            means KWin is throttling our surface (occlusion) and the user's
            next click can't land anyway within the wait */
        while (clock_gettime(CLOCK_MONOTONIC, &now), now.tv_sec < stop.tv_sec
                || (now.tv_sec == stop.tv_sec && now.tv_nsec < stop.tv_nsec)) {
            wl_display_roundtrip(dpy); /* blocking; drains all queue events */
            uint64_t latch = atomic_load(&g_latch_ns[fslot]);
            if (latch) {
                const double ms = (double)(latch - click) / 1e6;
                if (ms > 0.0 && ms < 500.0) {
                    samples[collected++] = ms;
                    printf("sample %u: click->latch %.1f ms\n", collected, ms);
                    fflush(stdout);
                }
                break;
            }
            nanosleep(&idle, nullptr);
        }
    }
    printf("RESULT n=%u", collected);
    /* log the run to a rolling CSV for cross-session calibration reads */
    {
        time_t wall = time(nullptr);
        FILE *rf = fopen("/tmp/latency_probe_results.csv", "a");
        if (rf) {
            fprintf(rf, "%ld,click->latch,n=%u", (long)wall, collected);
            if (collected) {
                for (unsigned k = 0; k < collected; ++k)
                    fprintf(rf, ":%.2f", samples[k]);
            }
            fputc('\n', rf);
            fclose(rf);
            printf(" (logged: /tmp/latency_probe_results.csv)\n");
            fflush(stdout);
        }
    }
    if (collected) {
        /* p50/p99 */
        for (unsigned i = 1; i < collected; ++i)
            for (unsigned j = i; j && samples[j] < samples[j - 1]; --j) {
                double t = samples[j]; samples[j] = samples[j - 1];
                samples[j - 1] = t;
            }
        printf(" p50=%.1f ms p99=%.1f ms min=%.1f ms max=%.1f ms\n",
            samples[collected * 50 / 100],
            samples[(int)((double)collected * 0.99)],
            samples[0], samples[collected - 1]);
    } else
        printf(" (no samples collected)\n");
    return collected ? 0 : 1;
}
