/* probe_vk.c — click→photon THROUGH a Vulkan swapchain, with optional lsfg-vk
 * layer injection for the DOUBLED leg (Session 40).
 *
 * How it measures:
 *   - renders a trivial Vulkan swapchain (2 images, immediate/mailbox) at ~240 Hz
 *     on the Wayland surface of this client (VK_KHR_wayland_surface)
 *   - a uinput click (uclick binary, timestamped to /tmp/latency_probe_inputs.log)
 *     sets the NEXT committed frame's color to magenta (one frame only)
 *   - each commit arms a wp_presentation_feedback; the 'presented' latch is the
 *     photon anchor (KWin CLOCK_MONOTONIC, same clock as evdev stamps)
 *   - we pair a click t0 with the latch of the FIRST magenta commit after t0
 *     => click→photon. baseline leg: layer OFF; doubled leg: layer ON
 *     (LSFGVK_PROFILE=furmark-oneway forced).
 * build:
 *   gcc -O2 -o /tmp/probe_vk tools/latency/probe_vk.c  \
 *       $(pkg-config --cflags --libs wayland-client vulkan) -lpthread
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"
#undef nullptr  /* protocol headers may define it; keep C style explicit */
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>

static struct wl_display* dpy;
static struct wl_compositor* comp;
static struct wl_shm* shm;
static struct xdg_wm_base* xdgwm;
static struct wp_presentation* present;
static uint32_t presentClock = 0;

static void pres_clock(void* d, struct wp_presentation* p, uint32_t c) {
    (void)d; (void)p; presentClock = c;
}
static const struct wp_presentation_listener pres_listener = { .clock_id = pres_clock };

static int g_surfConfigured = 0;
static void xdg_surf_conf(void* d, struct xdg_surface* s, uint32_t serial) {
    (void)d; xdg_surface_ack_configure(s, serial); g_surfConfigured = 1;
}
static const struct xdg_surface_listener xdg_surf_listener = { .configure = xdg_surf_conf };
static int32_t g_tlW = 0, g_tlH = 0; /* last toplevel configure size; 0 = client-decides */
static void xdg_tl_conf(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) {
    (void)d; (void)t; (void)s;
    if (w > 0) g_tlW = w;
    if (h > 0) g_tlH = h;   /* S40: real-game resolution — honor compositor's size */
}
static const struct xdg_toplevel_listener tl_listener = { .configure = xdg_tl_conf };

static struct wl_output* g_outputs[8];
static int32_t g_outScale[8];
static uint32_t g_outName[8];
static int g_nOut = 0;
static void out_geometry(void*, struct wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
static void out_mode(void*, struct wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
static void out_done(void*, struct wl_output*) {}
struct out_track { struct wl_output* o; int32_t scale; };
static void out_scale2(void* data, struct wl_output*, int32_t scale) {
    ((struct out_track*)data)->scale = scale;
}
static const struct wl_output_listener out_listener2 = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale2,
};
static struct out_track g_tracks[8];
static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) xdgwm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name)) {
        present = wl_registry_bind(r, name, &wp_presentation_interface, 2);
        wp_presentation_add_listener(present, &pres_listener, nullptr);
    }
    else if (strcmp(iface, wl_output_interface.name) == 0 && g_nOut < 8) {
        struct wl_output* o = wl_registry_bind(r, name, &wl_output_interface,
            ver < 3 ? ver : 3);
        g_outputs[g_nOut] = o;
        g_outName[g_nOut] = name;
        g_tracks[g_nOut].o = o; g_tracks[g_nOut].scale = 0;
        wl_output_add_listener(o, &out_listener2, &g_tracks[g_nOut]);
        ++g_nOut;
    }
}
static void global_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { .global = global, .global_remove = global_remove };

/* --- presented feedback (newest latch) --- */
static _Atomic uint64_t g_tag[4] = {0,0,0,0};  /* per-slot latch value  */
static _Atomic uint64_t g_latch[4] = {0,0,0,0};
static void fb_presented(void* data, struct wp_presentation_feedback* f,
        uint32_t s_hi, uint32_t s_lo, uint32_t nsec, uint32_t refresh,
        uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    (void)refresh; (void)flags;
    uint64_t ns = ((uint64_t)s_hi << 32 | s_lo) * 1000000000ULL + nsec;
    unsigned slot = (unsigned)(uintptr_t)data & 3u;
    atomic_store(&g_latch[slot], ns);
    atomic_store(&g_tag[slot], ((uint64_t)seq_hi << 32) | seq_lo);
    wp_presentation_feedback_destroy(f);
}
static void fb_disc(void* data, struct wp_presentation_feedback* f) {
    (void)data; wp_presentation_feedback_destroy(f);
}
static void fb_sync(void* data, struct wp_presentation_feedback* f, struct wl_output* o) { (void)data;(void)f;(void)o; }
static const struct wp_presentation_feedback_listener fb_listener =
    { .sync_output = fb_sync, .presented = fb_presented, .discarded = fb_disc };

static _Atomic unsigned g_plantSlot = 2;
static _Atomic uint64_t  g_plantNs  = 0;     /* click injection moment  */

/* --- dedicated input-pivot thread state (write side) --- */
static int g_inFds[64];
static int g_inNfd = 0;
static _Atomic unsigned g_clickHead = 0;  /* writer index */
static _Atomic unsigned g_clickTail = 0;  /* reader index */
static uint64_t g_clickRing[64];
static pthread_mutex_t g_clickMtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_inRun = 1;
static _Atomic int g_esc = 0;      /* S40+: user ESC = user kill switch */

static void* input_thread_fn(void* arg) {
    (void)arg;
    struct pollfd pfd[64];
    for (int i = 0; i < g_inNfd; ++i) { pfd[i].fd = g_inFds[i]; pfd[i].events = POLLIN; }
    struct input_event ev[64];
    while (atomic_load(&g_inRun)) {
        /* S40+: re-glob every ~2 s — later-spawned devices (uclick's
           dedicated key uinput node, hotplug) must become readable or
           the ESC kill switch never sees its events. No more than 64. */
        static unsigned reglobCount = 0;
        if ((++reglobCount % 8u) == 0 && g_inNfd < 60) {
            glob_t gr;
            if (glob("/dev/input/event*", 0, nullptr, &gr) == 0) {
                for (size_t q = 0; q < gr.gl_pathc && g_inNfd < 62; ++q) {
                    struct stat st;
                    if (stat(gr.gl_pathv[q], &st) != 0) continue;
                    bool have = false;
                    for (int e = 0; e < g_inNfd; ++e) {
                        struct stat se;
                        if (fstat(g_inFds[e], &se) == 0 &&
                            se.st_dev == st.st_dev &&
                            se.st_ino == st.st_ino) { have = true; break; }
                    }
                    if (have) continue;
                    int fd = open(gr.gl_pathv[q],
                        O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                    if (fd >= 0) {
                        g_inFds[g_inNfd++] = fd;
                        printf("input: +device %s\n", gr.gl_pathv[q]);
                    }
                }
                globfree(&gr);
            }
        }
        for (int i = 0; i < g_inNfd; ++i) { pfd[i].fd = g_inFds[i]; pfd[i].events = POLLIN; }
        const int pr = poll(pfd, (nfds_t)g_inNfd, 250);
        if (pr <= 0) continue;
        for (int i = 0; i < g_inNfd; ++i) {
            if (!(pfd[i].revents & POLLIN)) continue;
            ssize_t n;
            while ((n = read(g_inFds[i], ev, sizeof(ev))) > 0) {
                for (size_t j = 0; j < (size_t)n / sizeof(ev[0]); ++j) {
                    const int code = ev[j].code;
                    static unsigned keyDbg = 0;
                    if (ev[j].type == EV_KEY && ++keyDbg <= 10)
                        printf("EV type=%u code=%d val=%d\n",
                            ev[j].type, code, ev[j].value);
                    if (ev[j].type == EV_KEY && ev[j].value == 1) {
                        if (code == 1 /* KEY_ESC: user kill switch */) {
                            atomic_store(&g_esc, 1);
                            atomic_store(&g_inRun, 0);
                            printf("ESC pressed: ending probe NOW\n");
                            continue;
                        }
                    }
                    if (ev[j].type == EV_KEY && ev[j].value == 1
                            && (code == BTN_LEFT || code == 16 /* KEY_Q (uclick --key) */)) {
                        static unsigned totalClicks = 0;
                        ++totalClicks;
                        if (totalClicks == 1)
                            printf("first click seen\n");
                        else {
                            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                            printf("click #%u armed @%.3fs\n", totalClicks,
                                ts.tv_sec % 1000 + ts.tv_nsec / 1e9);
                        }
                        /* S40 strict A/B: use the KERNEL's evdev stamp
                           (EVIOCSCLOCKID=CLOCK_MONOTONIC), identical to the
                           baseline instrument's anchor — not the userspace
                           read time. */
                        const uint64_t t0 = (uint64_t)ev[j].time.tv_sec * 1000000000ULL
                            + (uint64_t)ev[j].time.tv_usec * 1000ULL;
                        unsigned head2 = atomic_load(&g_clickHead);
                        unsigned tail2 = atomic_load(&g_clickTail);
                        unsigned next = (head2 + 1) & 63;
                        if (next != tail2) {   /* drop on overflow (never expected) */
                            g_clickRing[head2] = t0;
                            atomic_store(&g_clickHead, next);
                            struct timespec ts2; clock_gettime(CLOCK_MONOTONIC, &ts2);
                            printf("ring push head=%u @%.3fs\n", next,
                                ts2.tv_sec % 1000 + ts2.tv_nsec / 1e9);
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}


/* ---- Session 40 dbl-ledger reader (app-side scanout anchor) ----
   Layout must match lsfg-vk-common/ipc/latency_ledger.hpp byte-exact. */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#define LEDGER_MAGIC ((uint64_t)0x4c44424c45444745ULL)   /* LDBLEDGER */
#define LAYER_MAGIC  ((uint64_t)0x4c44424c41594552ULL)   /* LDBLAYER */
#define LEDGER_FILE  (1024u * 128u)
#define LAYER_FILE   4096u
static uint64_t* g_ledgerMap = nullptr;   /* hdr: [magic][slot hdr+2 rows*4] */
static uint64_t* g_layerMap  = nullptr;   /* hdr: [magic][head][rows..] */
static int g_ledgerFd = -1, g_layerFd = -1;
static void ledger_reader_init(void) {
    /* S42p (fd-leak fix): idempotent attach. The ring SINKS are
       created once by the layer at first publish; before that this
       fn was re-called EVERY present (the caller's attachedOnce
       guard only latches when BOTH sinks already exist) and each
       successful shm_open+mmap leaked the previous cycle's fd and
       mapping — 2 fds per present at 240 fps hammered the 1024
       soft limit in ~4 s, EMFILE surfaced as vkGetSemaphoreFdKHR
       "error -13", and the whole external path wedged. Attach
       each sink exactly once; never re-open while attached. */
    struct stat st;
    if (g_ledgerFd < 0) {
        int fd = shm_open("/lsfg-dbl-ledger", O_RDONLY, 0);
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= LEDGER_FILE) {
            void* m = mmap(nullptr, LEDGER_FILE, PROT_READ, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED && *(uint64_t*)m == LEDGER_MAGIC) {
                printf("dbl-ledger: app sink attached\n");
                g_ledgerMap = (uint64_t*)m; g_ledgerFd = fd;
            } else close(fd);
        } else if (fd >= 0) close(fd);
    }
    if (g_layerFd < 0) {
        int fd = shm_open("/lsfg-dbl-layer", O_RDONLY, 0);
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= LAYER_FILE) {
            void* m = mmap(nullptr, LAYER_FILE, PROT_READ, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED && *(uint64_t*)m == LAYER_MAGIC) {
                printf("dbl-ledger: layer sink attached\n");
                g_layerMap = (uint64_t*)m; g_layerFd = fd;
            } else close(fd);
        } else if (fd >= 0) close(fd);
    }
}
/* newest layer row (capTs slot published AFTER COMMIT ARM: the row matching our
   magenta frame is the newest capTs >= commit-arm time) */
static int layer_first_after(uint64_t t0, uint64_t* capTs, uint64_t* fidx) {
    if (!g_layerMap) return 0;
    const uint64_t head = g_layerMap[1];   /* 1-based next-write */
    if (!head) return 0;
    const uint64_t COUNT = head - 1;
    const uint64_t back = COUNT > 254 ? 254 : COUNT;
    for (uint64_t k = head - back; k < head; ++k) {
        const uint64_t* row = (const uint64_t*)((const char*)g_layerMap
            + 16 + (k % 254) * 16);
        if (!row[0]) continue;
        if (row[0] >= t0) { *capTs = row[0]; *fidx = row[1]; return 1; }
    }
    return 0;
}
/* scan the ledger ring for the FIRST row with captureTs >= ts0 (chronological);
   returns presentedNs via *presented. Window: 512 newest rows. */
static int ledger_pair(uint64_t ts0, uint64_t* presented) {
    if (!g_ledgerMap) return 0;
    const uint64_t slot = g_ledgerMap[1]; /* next-write (1-based, monotonic) */
    if (slot == 0) return 0;
    uint64_t* hdr = g_ledgerMap;
    const uint64_t COUNT = slot - 1;
    const uint64_t back = COUNT > 512 ? 512 : COUNT;
    for (uint64_t k = slot - back; k < slot; ++k) {
        const uint64_t* row = hdr + 2 + (k % 4095) * 4;
        if (row[0] == 0) continue;                 /* stale slot */
        if (row[0] >= ts0) { *presented = row[1]; return row[1] != 0; }
        (void)k;
    }
    return 0;
}


/* --- S40 "CLICK!" overlay: 5x7 bitmap font, "CLICK! <n>" where <n> is the
   click counter (up to 3 digits); row-major, bit4=leftmost. --- */
#define TXT_COLS 10        /* max glyphs painted: C L I C K ! sp d d d */
#define TXT_GW 6           /* 5 px glyph + 1 px gap */
#define TXT_GH 7
#define TXT_SCALE 8
static const unsigned char GLYPH57[24][TXT_GH] = {   /* 0..5 = CLICK! glyphs, 6=space, 10..19 digits */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* 0: C */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* 1: L */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, /* 2: I */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* 3: C */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* 4: K */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* 5: ! */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 6: space */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 7 + */
    {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F}, /* 8 - */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 9 + */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 10: 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 11: 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 12: 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 13: 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 14: 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 15: 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 16: 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 17: 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 18: 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 19: 9 */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 20 + */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 21 + */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 22 + */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 23 + */
};
#define TEXT_GLYPHS 7   /* C L I C K ! ␣ */
#define TXTW (TXT_COLS * TXT_GW * TXT_SCALE)   /* 480 px */
#define TXTH (TXT_GH * TXT_SCALE)              /*  56 px */
#define TXTB (TXTW * TXTH * 4u)
/* compose: paint "CLICK! <n>" — white glyphs over (r,g,b) backdrop;
   clickNo clamps to 3 digits. */
static void txt_compose(unsigned char *map, float r, float g, float b, unsigned clickNo)
{
    unsigned digits[3]; unsigned nd = 0;
    unsigned n = clickNo > 999u ? 999u : clickNo;
    do { unsigned d = n % 10u; digits[nd++] = 10u + d? 10u + d : 10u; n /= 10u; if (nd >= 3) break; } while (n);
    if (nd == 0) { digits[nd++] = 10u; }
    /* glyph plan: 6 text glyphs + space + nd digits */
    unsigned glyphIdx[TXT_COLS]; unsigned nglyph = 0;
    glyphIdx[nglyph++] = 0; glyphIdx[nglyph++] = 1; glyphIdx[nglyph++] = 2;
    glyphIdx[nglyph++] = 0; glyphIdx[nglyph++] = 4; glyphIdx[nglyph++] = 5;
    glyphIdx[nglyph++] = 6;               /* space */
    for (int k = (int)nd - 1; k >= 0; --k) glyphIdx[nglyph++] = digits[k];
    for (unsigned gi = nglyph; gi < TXT_COLS; ++gi) glyphIdx[gi] = 6; /* trailing space */
    for (uint32_t y = 0; y < TXTH; ++y)
        for (uint32_t x = 0; x < TXTW; ++x) {
            const uint32_t gx = x / TXT_SCALE, gy = y / TXT_SCALE;
            const uint32_t col = gx / TXT_GW;
            const uint32_t incol = gx % TXT_GW;
            unsigned char *px = map + ((size_t)y * TXTW + x) * 4;
            unsigned gi = glyphIdx[col > 9 ? 9 : col];
            unsigned char on = 0;
            if (incol < 5 && col < TXT_COLS) on = (GLYPH57[gi][gy] >> (4 - incol)) & 1u;
            if (on) { px[0]=0xff; px[1]=0xff; px[2]=0xff; px[3]=0xff; }
            else    { px[0]=(unsigned char)(b*255.0f); px[1]=(unsigned char)(g*255.0f);
                      px[2]=(unsigned char)(r*255.0f); px[3]=0xff; }
        }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    /* optional side-log: LSFGVK_PROBE_LOG=/path — lets a second reader (agent)
       inspect a human's run after the fact (^C-safe: line-buffered writes). */
    {
        const char* logpath = getenv("LSFGVK_PROBE_LOG");
        if (logpath) {
            FILE* lf = fopen(logpath, "a");
            if (lf) {
                dup2(fileno(lf), fileno(stdout));
                setvbuf(stdout, nullptr, _IOLBF, 0);
            }
        }
    }
    const unsigned wantSamples = argc > 1 ? (unsigned)atoi(argv[1]) : 20;

    dpy = wl_display_connect(nullptr);
    if (!dpy) { fprintf(stderr, "wl_display_connect failed\n"); return 1; }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, nullptr);
    wl_display_roundtrip(dpy);
    if (!comp || !xdgwm || !present) { fprintf(stderr, "missing globals\n"); return 2; }

    /* surface, mapped per the proven contract */
    struct wl_surface* surf = wl_compositor_create_surface(comp);
    struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(xdgwm, surf);
    xdg_surface_add_listener(xs, &xdg_surf_listener, nullptr);
    struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_listener, nullptr);
    xdg_toplevel_set_app_id(tl, "probe-vk");
    xdg_toplevel_set_fullscreen(tl, nullptr);  /* un-occludable: compositor must
        cycle our buffers (occluded = QueuePresent stalls forever, proven) */
    xdg_toplevel_set_title(tl, "probe-vk");
    /* Session 40: put BOTH legs on the SAME display the overlay uses (DP-7,
     * 240 Hz): click->photon must be measured where the doubler actually
     * presents. The 240 Hz / scale-1 output is chosen via wl_output scale
     * events (DP-7 scale=1; HDMI-A-3 scale=1.5 per KWin). */
    {
        wl_display_roundtrip(dpy); wl_display_roundtrip(dpy); /* drains scale events */
        struct wl_output* target = nullptr;
        for (int i = 0; i < g_nOut; ++i) {
            printf("wl_output[%d] scale=%d%s\n", i, g_tracks[i].scale,
                g_tracks[i].scale == 1 ? "  <- 240 Hz target (DP-7)" : "");
            if (g_tracks[i].scale == 1 && target == nullptr)
                target = g_outputs[i];
        }
        const bool nofs = getenv("LSFGVK_PROBE_WINDOWED") != nullptr;
        if (target && !nofs) {
            printf("fullscreen to scale-1 output\n");
            xdg_toplevel_set_fullscreen(tl, target);
            wl_surface_commit(surf);
            for (int i = 0; i < 10 && !g_surfConfigured; ++i) wl_display_roundtrip(dpy);
            printf("fullscreen on DP-7 (scale-1 output) armed\n");
        } else {
            printf("WARNING: no scale-1 output found; staying windowed\n");
        }
    }
    wl_surface_commit(surf);
    for (int i = 0; i < 10 && !g_surfConfigured; ++i) wl_display_roundtrip(dpy);
    if (!g_surfConfigured) { fprintf(stderr, "xdg configure never arrived\n"); return 3; }

    /* --- Vulkan init --- */
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "probe-vk"; ai.apiVersion = VK_API_VERSION_1_1;
    const char* instExts[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instExts;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance failed\n"); return 4; }

    uint32_t ndev = 0; vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    if (!ndev) { fprintf(stderr, "no devices\n"); return 5; }
    VkPhysicalDevice devs[8]; vkEnumeratePhysicalDevices(inst, &ndev, devs);
    /* pick the RENDER GPU explicitly (MESA_VK_DEVICE_SELECT is honored too) */
    VkPhysicalDevice chosen = devs[0];
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        vkGetPhysicalDeviceProperties2(devs[i], &p2);
        if (p2.properties.vendorID == 0x1002 && p2.properties.deviceID == 0x7550) chosen = devs[i];
    }
    {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(chosen, &p);
        printf("render device: %s\n", p.deviceName);
    }
    uint32_t qfamN = 0; vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qfamN, nullptr);
    VkQueueFamilyProperties qfams[8]; vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qfamN, qfams);
    uint32_t gfxFam = UINT32_MAX;
    for (uint32_t i = 0; i < qfamN; ++i)
        if (qfams[i].queueCount && (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { gfxFam = i; break; }
    if (gfxFam == UINT32_MAX) { fprintf(stderr, "no gfx queue\n"); return 6; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = gfxFam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExts[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExts;
    VkDevice dev;
    if (vkCreateDevice(chosen, &dci, nullptr, &dev) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); return 7; }
    VkQueue queue; vkGetDeviceQueue(dev, gfxFam, 0, &queue);

    /* --- Wayland surface via WSI + swapchain --- */
    VkWaylandSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    sci.display = dpy; sci.surface = surf;
    VkSurfaceKHR vsurf;
    if (vkCreateWaylandSurfaceKHR(inst, &sci, nullptr, &vsurf) != VK_SUCCESS) { fprintf(stderr, "vkCreateWaylandSurface failed\n"); return 8; }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(chosen, gfxFam, vsurf, &supported);
    if (!supported) { fprintf(stderr, "surface not supported on gfx family\n"); return 9; }

    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(chosen, vsurf, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = 320; ext.height = 180; }
    /* S40: a real game renders at the compositor-configured size (fullscreen =
       display res). The toplevel configure carries that size; use it. */
    if (g_tlW > 0 && g_tlH > 0) { ext.width = (uint32_t)g_tlW; ext.height = (uint32_t)g_tlH; }
    /* User directive (S40): render at the DISPLAY resolution like a real game —
       the dating value of a 320x180 synthetic (sub-ms blit) vs a real 2560x1440
       workload. Fullscreen present extent comes from the compositor = the
       display size; keep it unless LSFGVK_PROBE_SIZE overrides. */
    {
        const char *sz = getenv("LSFGVK_PROBE_SIZE");
        if (sz) { unsigned w = 0, h = 0; if (sscanf(sz, "%ux%u", &w, &h) == 2 && w && h) { ext.width = w; ext.height = h; } }
    }
    printf("swapchain extent %ux%u minImages=%u\n", ext.width, ext.height, caps.minImageCount);

    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swci.surface = vsurf;
    swci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = ext;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    /* S40: INHERIT + non-opaque clears defeat DIRECT SCANOUT (the wedge
   shape dies when KWin composites instead of handing our buffer to
   the display engine) */
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    /* FIFO: the compositor paces us; occlusion stalls are bounded (frame
     * wall-clock ~4 ms at 240 Hz) and buffer cycling continues even when the
     * window is partly occluded. IMMEDIATE froze inside vkQueuePresentKHR
     * when the compositor stopped consuming the surface (occluded). */
    /* S40 strict A/B: MAILBOX when supported — the FIFO ETIME spin traced
       in decay runs lives in Mesa's WSI FIFO pacing; MAILBOX removes the
       pending-queue pacing without changing the click semantics. */
    {
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR gpm =
            (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
                vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        uint32_t nmodes = 0;
        if (gpm) gpm(chosen, vsurf, &nmodes, nullptr);
        VkPresentModeKHR modes[8];
        int mailbox = 0;
        if (gpm && nmodes && nmodes <= 8) {
            gpm(chosen, vsurf, &nmodes, modes);
            for (uint32_t m = 0; m < nmodes; ++m) if (modes[m] == VK_PRESENT_MODE_MAILBOX_KHR) mailbox = 1;
        }
        swci.presentMode = (mailbox && getenv("LSFGVK_PROBE_FIFO") == nullptr)
            ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }
    VkSwapchainKHR swap;
    if (vkCreateSwapchainKHR(dev, &swci, nullptr, &swap) != VK_SUCCESS) { fprintf(stderr, "CreateSwapchain failed\n"); return 10; }
    uint32_t nimg = 0; vkGetSwapchainImagesKHR(dev, swap, &nimg, nullptr);
    VkImage imgs[8]; vkGetSwapchainImagesKHR(dev, swap, &nimg, imgs);
    printf("swapchain images %u, present clock %u", nimg, presentClock);
    if (presentClock != 1) {                         /* CLOCK_MONOTONIC */
        printf(" — FATAL: compositor latch clock != CLOCK_MONOTONIC;"
               " click->photon math would be invalid\n");
        return 9;
    }
    printf(" (=CLOCK_MONOTONIC, same clock as click t0)\n");

    /* memory-free fill: use vkCmdClearColorImage (no vertex pipeline needed) */
    VkCommandPool cpool;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = gfxFam;
    vkCreateCommandPool(dev, &pci, nullptr, &cpool);
    VkCommandBuffer cbs[8];
    VkFence cbsFences[8];   /* SIGNALED: guard each cb reset (pending cb
                              reset = UB → RADV wedged after the first
                              magenta commit (hold path resets in <2 ms)) */
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = nimg;
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (uint32_t i = 0; i < 8; ++i)
        vkCreateFence(dev, &fci, nullptr, &cbsFences[i]);
    /* fences start UNSIGNALED (no signaled-初始 flag exists); the guard waits
       would time out forever — pulse them: one empty submit each (idle queue). */
    {
        VkSubmitInfo ssi = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        for (uint32_t i = 0; i < 8; ++i)
            vkQueueSubmit(queue, 1, &ssi, cbsFences[i]);
        vkQueueWaitIdle(queue);
        for (uint32_t i = 0; i < 8; ++i)
            vkResetFences(dev, 1, &cbsFences[i]);
        /* re-signal once more: the CORRECT initial state is signaled so the
           first reset passes */
        for (uint32_t i = 0; i < 8; ++i)
            vkQueueSubmit(queue, 1, &ssi, cbsFences[i]);
        vkQueueWaitIdle(queue);   /* fences now all SIGNALED */
    }

    /* swapchain image layout → GENERAL once for clearImage */
    {
        vkResetCommandBuffer(cbs[0], 0);
        VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cbs[0], &bbi);
        for (uint32_t i = 0; i < nimg; ++i) {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = imgs[i]; b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
            vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,0,0,0,1,&b);
            VkClearColorValue clear = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } }; /* S40: black default */
            vkCmdClearColorImage(cbs[0], imgs[i], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
        }
        vkEndCommandBuffer(cbs[0]);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cbs[0];
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    #define NSWSEMS 8
    VkSemaphore acqSems[NSWSEMS]; VkSemaphore presSems[NSWSEMS];
    VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < NSWSEMS; ++i) {
        vkCreateSemaphore(dev, &semci, nullptr, &acqSems[i]);
        vkCreateSemaphore(dev, &semci, nullptr, &presSems[i]);
    }


    /* --- S40: "CLICK!" overlay staging buffer (host-visible, coherent) --- */
    VkBuffer txtBuf; VkDeviceMemory txtMem; unsigned char *txtMap = nullptr;
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = TXTB; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bci, nullptr, &txtBuf) != VK_SUCCESS) { fprintf(stderr, "txt buffer\n"); return 11; }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, txtBuf, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(chosen, &mp);
        for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
            if ((mr.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mai.memoryTypeIndex = t; break; }
        if (vkAllocateMemory(dev, &mai, nullptr, &txtMem) != VK_SUCCESS) { fprintf(stderr, "txt mem\n"); return 12; }
        vkBindBufferMemory(dev, txtBuf, txtMem, 0);
        vkMapMemory(dev, txtMem, 0, TXTB, 0, (void**)&txtMap);
        txt_compose(txtMap, 0.0f, 0.0f, 0.0f, 0u);   /* initial bg: black */
        printf("CLICK! overlay %ux%u staged\n", TXTW, TXTH);
    }
    /* --- input thread: SAME discovery as probe_latency (all mice + virtual clicker) --- */
    int fds[64]; int nfd = 0;  /* copies; handed to the input thread below */
    glob_t gg;
    if (glob("/dev/input/by-id/*event-mouse*", 0, nullptr, &gg) == 0) {
        char names[64][128];
        size_t n = gg.gl_pathc < 32 ? gg.gl_pathc : 32;
        for (size_t i = 0; i < n; ++i) snprintf(names[i], 128, "%s", gg.gl_pathv[i]);
        globfree(&gg);
        for (size_t i = 0; i < n; ++i) {
            int fd = open(names[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            fds[nfd++] = fd;
        }
    }
    glob_t ge;
    if (glob("/dev/input/event*", 0, nullptr, &ge) == 0) {
        char enames[64][128];
        size_t n = ge.gl_pathc < 32 ? ge.gl_pathc : 32;
        for (size_t i = 0; i < n; ++i) snprintf(enames[i], 128, "%s", ge.gl_pathv[i]);
        globfree(&ge);
        for (size_t i = 0; i < n; ++i) {
            char nm[256] = "";
            int fd = open(enames[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, EVIOCGNAME(255), nm) < 0 || !strstr(nm, "latency-probe-clicker")) { close(fd); continue; }
            printf("virtual clicker: %s\n", enames[i]);
            fds[nfd++] = fd;
        }
    }
    printf("input devices armed: %d\n", nfd);
    /* hand the armed fds to a DEDICATED pump thread (the in-line pump inside the
       present loop starved: with the loop pacing at ~250 fps the pump rarely got
       polled between present-hosting stalls — proven: 1 read of 10 clicks). */
    g_inNfd = nfd;
    for (int i = 0; i < nfd; ++i) g_inFds[i] = fds[i];
    pthread_t inthr;
    pthread_create(&inthr, nullptr, input_thread_fn, nullptr);

    /* --- main loop: render + feedback + click pairing --- */
    printf("click the mouse (or run uclick) — %u samples wanted\n", wantSamples);
    double samples[512]; unsigned collected = 0;
    uint64_t thisClickT2 = 0;            /* pending pairing (declared outer scope) */
    uint64_t clickT[64]; unsigned nClickT = 0;
    for (uint32_t i = 0; i < nfd; ++i) {
        unsigned clk = CLOCK_MONOTONIC;
        ioctl(fds[i], EVIOCSCLOCKID, &clk);
    }

    uint32_t imgIdx = 0; (void)imgIdx;
    struct pollfd pfd[64];
    for (int i = 0; i < nfd; ++i) { pfd[i].fd = fds[i]; pfd[i].events = POLLIN; }
    struct input_event ev[64];

    uint64_t frameSeq = 0;
    double tStartMs = now_ms();
    for (;;) {
        if (atomic_load(&g_esc)) {   /* S40+: user ESC = immediate end */
            printf("probe: ESC kill, wrapping up\n");
            break;
        }
        { static int attachedOnce = 0;
          if (!attachedOnce && g_ledgerMap && g_layerMap) attachedOnce = 1;
          if (!attachedOnce) ledger_reader_init(); }
                /* pump input: consume from the dedicated input thread's ring */
        while (atomic_load(&g_clickTail) != atomic_load(&g_clickHead)) {
            const unsigned tail = atomic_load(&g_clickTail);
            uint64_t t0;
            pthread_mutex_lock(&g_clickMtx);
            t0 = g_clickRing[tail];
            pthread_mutex_unlock(&g_clickMtx);
            if (nClickT < 64) clickT[nClickT++] = t0;
            atomic_store(&g_clickTail, (tail + 1) & 63);
        }

        /* acquire + draw + commit */
        uint32_t idx = 0;
        /* S40: use the FENCE form with a 0-ns timeout polled by the host —
           acq-sem waits can wedge inside RADV's syncobj path (gdb main-frames
           deep in libvulkan_radeon in every 'decay' run); a fence keeps the
           wait on the host where nothing can hide it. */
        VkResult ac = vkAcquireNextImageKHR(dev, swap, 100'000'000ULL /*100 ms cap*/,
            VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
        if (ac == VK_TIMEOUT || ac == VK_NOT_READY) {
            static unsigned acqFail = 0;
            ++acqFail;
            if (acqFail % 240 == 0)
                printf("acquire spinning: %u timeouts (33 ms cap each)\n", acqFail);
            {   /* keep WSI queues serviced while parked */
                wl_display_dispatch_pending(dpy);
                struct timespec pz = { 0, 4'000'000 };
                nanosleep(&pz, nullptr);
            }
            continue;
        }
        if (ac != VK_SUCCESS && ac != VK_SUBOPTIMAL_KHR) {
            printf("acquire FAILED ac=%d\n", ac);
            continue;
        }

        int magenta = 0;
        uint64_t thisClickT = 0;
        static uint64_t magentaUntil = 0;
        static float clearR = 1.0f, clearG = 0.02f, clearB = 1.0f; /* unique per-click color */
        static unsigned nClickSeq = 0;   /* wall-clock ns until which we hold */
        static uint64_t lastPaceNs = 0;     /* pacing anchor for the frame loop */
        if (nClickT > 0 && magentaUntil == 0) {
            /* newest click becomes THIS frame's paint instant. The flash is
               HELD ~120 ms so it is perceivable at any frame rate; the
               MEASUREMENT pairs with the FIRST magenta commit only. */
            thisClickT = clickT[--nClickT];
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            magentaUntil = ts.tv_sec * 1000000000ULL + 25000000ULL; /* S40: 25 ms hold — long holds overcycled the FIFO in-flight */
            /* paint the UNIQUE click color. User requirement (S40): each
               click paints its own distinct color so the measurement is
               provably looking at THAT click's frame, never a stale one. */
            const float clickHue = (float)(nClickSeq % 12) / 12.0f;  /* 12 hues */
            clearR = 0.5f + 0.5f * (sinf(clickHue * 6.2831853f) * 0.5f + 0.5f);
            clearG = 0.5f + 0.5f * (sinf(clickHue * 6.2831853f + 2.0943951f) * 0.5f + 0.5f);
            clearB = 0.5f + 0.5f * (sinf(clickHue * 6.2831853f + 4.1887902f) * 0.5f + 0.5f);
            nClickSeq++;
            {   /* wait for the previous use of this cb before reset: reset of a
                   PENDING cb is UB and RADV wedged exactly there (probe hold) */
                VkFence f = cbsFences[idx];
                if (vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX) /* S40: INFINITE — the cb is already queued (GPU clear <0.2 ms); a capped wait that TIMEOUTS led to a 'continue' = an ACQUIRED-BUT-NEVER-PRESENTED image (lost present) → the FIFO drains and the acquire starves */ /* S40: generous — a tight cap turned in-flight cbs into LOST PRESENTS (acquired-but-never-presented images starve the acquire) */ != VK_SUCCESS)
                    continue;   /* still in flight: present the previous image */
                vkResetFences(dev, 1, &f);
            }
        vkResetCommandBuffer(cbs[idx], 0);
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(cbs[idx], &bbi);
            VkClearColorValue clear = { .float32 = { clearR, clearG, clearB, 1.0f } };
            vkCmdClearColorImage(cbs[idx], imgs[idx], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
                {   /* S40 "CLICK!" overlay: centered copy onto the frame */
                    txt_compose(txtMap, clear.float32[0], clear.float32[1], clear.float32[2], nClickSeq);
                    VkBufferImageCopy cpy = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
                        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                        .imageOffset = { (int32_t)(ext.width > TXTW ? (ext.width - TXTW) / 2 : 0),
                                         (int32_t)(ext.height > TXTH ? (ext.height - TXTH) / 2 : 0), 0 },
                        .imageExtent = { ext.width > TXTW ? TXTW : ext.width,
                                         ext.height > TXTH ? TXTH : ext.height, 1 } };
                    vkCmdCopyBufferToImage(cbs[idx], txtBuf, imgs[idx], VK_IMAGE_LAYOUT_GENERAL, 1, &cpy);
                }
            vkEndCommandBuffer(cbs[idx]);
            /* fence-acquire: the CB needs no sem wait; exclusivity comes
               from the acquire (KWin cannot present an acquired image). */
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr;
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            si.pWaitDstStageMask = &st;
            si.commandBufferCount = 1; si.pCommandBuffers = &cbs[idx];
            /* S40 WEDGE FIX 2: the last presSems[] signal fires but NOTHING waits it; each stale (gen,val) parks RADV's present-internal retire wait (decoded in the parked strace: timeout=infinite, point=0xd0000000d, handle spins). Present waits nothing (ordering = cbsFences + acquire), so the cb signals nothing. */
            si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
            vkQueueSubmit(queue, 1, &si, cbsFences[idx]); /* S40: EVERY in-loop cb signals its slot fence — a held-path VK_NULL submit left the next guard's wait unsatisfiable (infinite park, T9) */
            magentaUntil = 0;   /* user spec: ONE colored frame then back to black */
            magenta = 1;
        } else if (magentaUntil) {
            /* S40: (dead by design — arm now one-shots) plain black frame */
            {   VkFence f = cbsFences[idx];
                if (vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
                    continue;
                vkResetFences(dev, 1, &f);
            }
            vkResetCommandBuffer(cbs[idx], 0);
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(cbs[idx], &bbi);
            VkClearColorValue clear = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
            vkCmdClearColorImage(cbs[idx], imgs[idx], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
            vkEndCommandBuffer(cbs[idx]);
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr;
            si.commandBufferCount = 1; si.pCommandBuffers = &cbs[idx];
            si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
            vkQueueSubmit(queue, 1, &si, cbsFences[idx]);
        } else {
            /* background frame: reset the slot back to dim (cheap clear command) */
            {   /* wait for the previous use of this cb before reset: reset of a
               PENDING cb is UB and RADV wedged exactly there (probe hold) */
            VkFence f = cbsFences[idx];
            if (vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX) /* S40: INFINITE — the cb is already queued (GPU clear <0.2 ms); a capped wait that TIMEOUTS led to a 'continue' = an ACQUIRED-BUT-NEVER-PRESENTED image (lost present) → the FIFO drains and the acquire starves */ /* S40: generous — a tight cap turned in-flight cbs into LOST PRESENTS (acquired-but-never-presented images starve the acquire) */ != VK_SUCCESS)
                continue;   /* still in flight: present the previous image */
            vkResetFences(dev, 1, &f);
        }
        vkResetCommandBuffer(cbs[idx], 0);
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(cbs[idx], &bbi);
            /* S40 UX: black screen by default (the 'identical content'
               damage fear was disproven — swapchain flips retire fine) */
            VkClearColorValue clear = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
            vkCmdClearColorImage(cbs[idx], imgs[idx], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 });
                {   /* S40 "CLICK!" overlay: centered copy onto the frame */
                    txt_compose(txtMap, clear.float32[0], clear.float32[1], clear.float32[2], nClickSeq);
                    VkBufferImageCopy cpy = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
                        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                        .imageOffset = { (int32_t)(ext.width > TXTW ? (ext.width - TXTW) / 2 : 0),
                                         (int32_t)(ext.height > TXTH ? (ext.height - TXTH) / 2 : 0), 0 },
                        .imageExtent = { ext.width > TXTW ? TXTW : ext.width,
                                         ext.height > TXTH ? TXTH : ext.height, 1 } };
                    vkCmdCopyBufferToImage(cbs[idx], txtBuf, imgs[idx], VK_IMAGE_LAYOUT_GENERAL, 1, &cpy);
                }
            vkEndCommandBuffer(cbs[idx]);
            /* fence-acquire: the CB needs no sem wait; exclusivity comes
               from the acquire (KWin cannot present an acquired image). */
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr;
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            si.pWaitDstStageMask = &st;
            si.commandBufferCount = 1; si.pCommandBuffers = &cbs[idx];
            /* S40 WEDGE FIX 2: the last presSems[] signal fires but NOTHING waits it; each stale (gen,val) parks RADV's present-internal retire wait (decoded in the parked strace: timeout=infinite, point=0xd0000000d, handle spins). Present waits nothing (ordering = cbsFences + acquire), so the cb signals nothing. */
            si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
            vkQueueSubmit(queue, 1, &si, cbsFences[idx]);
        }

        /* restore-dim for the NEXT use of this slot is handled by the bg repaint */

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        /* S40 WEDGE FIX: the present waits NO semaphore — RADV's WSI
           internalized our binary signaled sem into a TIMELINE wait with
           a CORRUPT point (mem-read of the args: handle 13, point
           0xdda31200000055b4 — pointer bytes) → instant ETIME spin at
           timeout=0 → present parks forever. Ordering is already
           guaranteed by cbsFences (cb complete before use) + acquire
           exclusivity; the sem added nothing but the wedge. */
        pi.waitSemaphoreCount = 0; pi.pWaitSemaphores = nullptr;
        pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
        unsigned slot = idx & 3u;
        if (magenta) printf("magenta commit (slot %u)\n", slot);
        atomic_store(&g_latch[slot], 0);
        /* S40 DISCRIMINATOR: LSFGVK_PROBE_NOFB=1 stops wp_presentation
           arming once samples are collected — isolates whether the
           post-sample acquire-starve is KWin's feedback channel. */
        if (getenv("LSFGVK_PROBE_NOFB") == nullptr || collected == 0) {
            struct wp_presentation_feedback* fb = wp_presentation_feedback(present, surf);
            wp_presentation_feedback_add_listener(fb, &fb_listener, (void*)(uintptr_t)slot);
        }
        if (magenta) atomic_store(&g_plantSlot, slot);
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        (void)pr;
        wl_display_flush(dpy);
        /* dispatch until THIS commit's presented arrives (KWin replies one frame
           later at vblank); the FIFO barrier pacing needs us to keep dispatching
           or Mesa's internal queue wedges. bounded ~50 ms. */
        /* S40: single roundtrip per commit, exactly like lsfg-vk-app's
           drainPresentFeedback — the 12-iteration loop wedged Mesa's WSI
           internal pacing in every decayed run (and the flush keeps the
           FIFO bursts rolling). */
        wl_display_roundtrip(dpy);
        frameSeq++;
        if (frameSeq == 1) printf("render loop ALIVE (first frame presented, slot %u)\n", slot);
        if (frameSeq % 240 == 0) {
            struct timespec rn; clock_gettime(CLOCK_MONOTONIC, &rn);
            const uint64_t nowNs = rn.tv_sec * 1000000000ULL + rn.tv_nsec;
            static uint64_t lastTickNs = 0; static unsigned lastSeq = 0;
            unsigned latches = 0;
            for (unsigned k = 0; k < 4; ++k) if (atomic_load(&g_latch[k])) ++latches;
            const double fps = lastTickNs ? (double)(frameSeq - lastSeq) * 1e9 / (double)(nowNs - lastTickNs) : 0.0;
            printf("tick %u fps=%.0f latches=%u clickT=%u ring(tail=%u head=%u) magUntil=%llu\n",
                frameSeq, fps, latches, nClickT, atomic_load(&g_clickTail),
                atomic_load(&g_clickHead),
                (unsigned long long)(magentaUntil ? 1u : 0u));
            lastTickNs = nowNs; lastSeq = frameSeq;
        }

        if (magenta && thisClickT) {
            printf("pair-attempt click%s=%llu\n", "", (unsigned long long)thisClickT);
            /* Preferred pairing: the APP's scanout ledger (doubled picture is
               presented on the app's overlay surface; the probe's own surface
               sits occluded and KWin posts no presented-feedback for it). */
            uint64_t capTs = 0, fidx = 0;
            /* the click's OWN frame = FIRST row captured at/after the click */
            if (layer_first_after(thisClickT, &capTs, &fidx)) {
                printf("pair-layer: click=%llu firstCapTs=%llu fidx=%llu\n",
                    (unsigned long long)thisClickT, (unsigned long long)capTs, (unsigned long long)fidx);
                uint64_t presented = 0;
                /* scan a few dispatch cycles: the app's drain lags ~1 frame */
                for (int k = 0; k < 3 && !ledger_pair(capTs, &presented); ++k) {
                    struct timespec tw = { 0, 2'500'000 };
                    nanosleep(&tw, nullptr);
                    if (!ledger_pair(capTs, &presented) && k == 2)
                        printf("pair-layer: NO ledger row for capTs=%llu\n", (unsigned long long)capTs);
                }
                if (presented) {
                    const double ms = (double)(presented - thisClickT) / 1e6;
                    if (ms > 0.0 && ms < 500.0 && collected < wantSamples && collected < 512) {
                        samples[collected++] = ms;
                        printf("sample %u: click->photon %.2f ms (colorId=%u, ledger slot=%llu fidx=%llu)\n",
                            collected, ms, nClickSeq - 1, (unsigned long long)capTs, (unsigned long long)fidx);
                    }
                    thisClickT = 0;
                    atomic_store(&g_latch[slot], 0);
                }
            }
            /* fallback: own-surface feedback (baseline mode, no layer) */
            uint64_t latch = atomic_load(&g_latch[slot]);
            if (!latch) {   /* S40: KWin delivers 'presented' with the NEXT
               output frame(s); bounded recheck — roundtrip (dispatches the
               frame) ×3 with one 5 ms snooze; catches ~2 vblank periods
               without re-arming anything. */
                for (int k = 0; k < 3 && !latch; ++k) {
                    wl_display_roundtrip(dpy);
                    latch = atomic_load(&g_latch[slot]);
                    if (!latch) {
                        struct timespec tw = { 0, 5'000'000 };
                        nanosleep(&tw, nullptr);
                    }
                }
                wl_display_flush(dpy);
            }
            printf("magenta latch=%llu\n", (unsigned long long)latch);
            if (latch && collected < wantSamples && collected < 512) {
                const double ms = (double)(latch - thisClickT) / 1e6;
                if (ms > 0.0 && ms < 500.0) {
                    samples[collected++] = ms;
                    printf("sample %u: click->latch %.2f ms (surface)\n", collected, ms);
                }
                thisClickT = 0;
            }
            else if (!latch) thisClickT = thisClickT; /* wait next cycles */
        }
        /* PACE like a real client: a game ships ~60-240 fps; this probe at
           ~1000 fps starves the layer/app Release path (ring ring depth 4 at
           970 fps in-flight frames = selectFreeSlot 'no slot' freeze) and the
           acquire side then spins VK_TIMEOUT forever (observed after the very
           first pair). 8 ms between frames when idle; 2 ms while flashing. */
        do {
            struct timespec pz; clock_gettime(CLOCK_MONOTONIC, &pz);
            const uint64_t nowNs = pz.tv_sec * 1000000000ULL + pz.tv_nsec;
            const uint64_t target = magentaUntil ? 8 : 8;
            /* S40: hold pacing == bg pacing: at 2 ms the cb (≈4-8 ms flight)
               can't complete between hold frames → the guard's 8 ms fence
               wait times out → 'continue' skips the present → all swapchain
               images end up acquired-unpresented → acquire starves (the
               post-'hold expired' wedge). 8 ms odd frames keep the cb
               retired. */
            if (lastPaceNs && nowNs - lastPaceNs < target * 1000000ULL) {
                /* S40 FINAL LOCK: dispatch while waiting — Mesa's WSI owns a
                   PRIVATE wayland queue whose callbacks deliver buffer
                   retirement; sleeping without dispatching starves it and
                   wedges FIFO presents (the entire decay mechanic).
                   wl_display_dispatch_pending routes bytes to their queues. */
                wl_display_dispatch_pending(dpy);
                const uint64_t rest2 = target * 1000000ULL - (nowNs - lastPaceNs);
                if (rest2 < target * 1000000ULL) {
                    struct timespec rs = { rest2 / 1000000000ULL, rest2 % 1000000000ULL };
                    nanosleep(&rs, nullptr);
                }
            } else {
                wl_display_dispatch_pending(dpy);   /* pacing-exempt dispatch */
            }
            lastPaceNs = nowNs;
        } while (0);
        if (collected >= wantSamples) break;
        /* instrument runtime cap (wall clock), not frame-count: the user needs
           TIME to click; the old frame-count guard (~2000 frames ≈ 8 s at
           uncapped rates) killed the probe before a human could click
           (observed on Charles's run: 'no chance to click, it comes and goes'). */
        if (frameSeq % 240 == 0) {
            struct timespec rn; clock_gettime(CLOCK_MONOTONIC, &rn);
            const double elapsed = rn.tv_sec * 1000.0 + rn.tv_nsec / 1e6 - tStartMs;
            if (elapsed > 120000.0) break;  /* 120 s hard cap */
        }
    }

    printf("RESULT n=%u", collected);
    if (collected > 1) {
        for (unsigned i = 1; i < collected; ++i)
            for (unsigned j = i; j && samples[j] < samples[j - 1]; --j) {
                double t = samples[j]; samples[j] = samples[j - 1]; samples[j - 1] = t;
            }
        printf(" p50=%.2f ms p99=%.2f ms min=%.2f ms max=%.2f ms",
            samples[collected / 2],
            samples[(unsigned)((double)collected * 0.99)],
            samples[0], samples[collected - 1]);
    }
    printf("\n");
    return collected ? 0 : 1;
}
