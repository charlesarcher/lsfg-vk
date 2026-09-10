/* Copy-on-submit CS filter. RADV's original BO list is never mutated.
 *
 * gfx (ip=0): dest is unused. Drop every dest handle.
 * capture (ip=1): keep this slot's dest + one presented image. Drop extra dests
 * and every small resident BO.
 * Dest identity is (slot, drm fd, GEM handle) published at dest create.
 * Never submit bo_number=0. If dest identity is unknown, do not rewrite.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <libdrm/amdgpu_drm.h>

static int (*real_ioctl)(int, unsigned long, void *);
static int (*real_drmIoctl)(int, unsigned long, void *);
static void *libdrm_h;
static int pair_drm[256];
static uint32_t pair_h[256];
static int pair_slot[256];
static int npair;
static int capture_slot = -1;
static unsigned nlog;

static int is_amdgpu_cs(unsigned long request) {
    return _IOC_TYPE(request) == DRM_IOCTL_BASE
        && _IOC_NR(request) == (DRM_COMMAND_BASE + DRM_AMDGPU_CS);
}

static int kioctl(int fd, unsigned long request, void *arg) {
    if (real_drmIoctl)
        return real_drmIoctl(fd, request, arg);
    if (!real_ioctl)
        real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
    return real_ioctl(fd, request, arg);
}

static void cslog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "lsfg-csstrip: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static void load_dest(void) {
    npair = 0;
    capture_slot = -1;
    FILE *f = fopen("/tmp/lsfg_dest_handles", "r");
    if (f) {
        int slot, drm;
        unsigned h;
        while (npair < 256 && fscanf(f, "%d %d %u", &slot, &drm, &h) == 3) {
            pair_slot[npair] = slot;
            pair_drm[npair] = drm;
            pair_h[npair] = h;
            ++npair;
        }
        fclose(f);
    }
    f = fopen("/tmp/lsfg_capture_slot", "r");
    if (f) {
        int s;
        if (fscanf(f, "%d", &s) == 1)
            capture_slot = s;
        fclose(f);
    }
}

static int is_dest_handle(int csfd, uint32_t handle) {
    for (int i = 0; i < npair; ++i) {
        if (pair_drm[i] == csfd && pair_h[i] == handle)
            return 1;
    }
    return 0;
}

static int is_slot_dest(int csfd, uint32_t handle) {
    if (capture_slot < 0)
        return is_dest_handle(csfd, handle);
    for (int i = 0; i < npair; ++i) {
        if (pair_slot[i] == capture_slot && pair_drm[i] == csfd && pair_h[i] == handle)
            return 1;
    }
    return 0;
}

static uint64_t bo_size(int drmfd, uint32_t handle) {
    struct drm_amdgpu_gem_create_in info = {0};
    struct drm_amdgpu_gem_op op = {
        .handle = handle,
        .op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO,
        .value = (uint64_t)(uintptr_t)&info,
    };
    if (kioctl(drmfd, DRM_IOCTL_AMDGPU_GEM_OP, &op) != 0)
        return 0;
    return info.bo_size;
}

struct CsCopy {
    union drm_amdgpu_cs cs;
    uint64_t chunk_ptrs[16];
    struct drm_amdgpu_cs_chunk chunks[16];
    struct drm_amdgpu_bo_list_in bl;
    struct drm_amdgpu_bo_list_entry ent[128];
};

static __thread struct CsCopy tls;

static void *filter_cs(int fd, union drm_amdgpu_cs *orig) {
    if (!orig || orig->in.num_chunks == 0 || orig->in.num_chunks > 16)
        return orig;
    const uint64_t *chunk_ptrs = (const uint64_t *)(uintptr_t)orig->in.chunks;
    if (!chunk_ptrs)
        return orig;

    uint32_t ip = 99;
    struct drm_amdgpu_bo_list_in *bl = NULL;
    uint32_t bl_chunk = 0;
    for (uint32_t i = 0; i < orig->in.num_chunks; ++i) {
        struct drm_amdgpu_cs_chunk *ch =
            (struct drm_amdgpu_cs_chunk *)(uintptr_t)chunk_ptrs[i];
        if (!ch)
            continue;
        if (ch->chunk_id == AMDGPU_CHUNK_ID_IB) {
            const struct drm_amdgpu_cs_chunk_ib *ib =
                (const struct drm_amdgpu_cs_chunk_ib *)(uintptr_t)ch->chunk_data;
            if (ib)
                ip = ib->ip_type;
        }
        if (ch->chunk_id == AMDGPU_CHUNK_ID_BO_HANDLES) {
            bl = (struct drm_amdgpu_bo_list_in *)(uintptr_t)ch->chunk_data;
            bl_chunk = i;
        }
    }
    if (!bl || bl->bo_number == 0 || bl->bo_number > 128)
        return orig;
    const struct drm_amdgpu_bo_list_entry *ent =
        (const struct drm_amdgpu_bo_list_entry *)(uintptr_t)bl->bo_info_ptr;
    if (!ent)
        return orig;

    load_dest();
    if (npair == 0)
        return orig;

    int n_dest = 0;
    int n_slot = 0;
    int n_large = 0;
    for (uint32_t j = 0; j < bl->bo_number; ++j) {
        if (is_dest_handle(fd, ent[j].bo_handle)) {
            ++n_dest;
            if (is_slot_dest(fd, ent[j].bo_handle))
                ++n_slot;
        } else if (bo_size(fd, ent[j].bo_handle) >= (8ull << 20))
            ++n_large;
    }
    if (n_dest == 0)
        return orig;

    uint32_t nk = 0;
    if (ip == 1) {
        /* Keep only presented image; drop dest handles so the kernel does not
         * implicit-sync on Card B's read fences. */
        uint32_t best_j = 0;
        uint64_t best_sz = 0;
        int have_pres = 0;
        for (uint32_t j = 0; j < bl->bo_number; ++j) {
            if (is_dest_handle(fd, ent[j].bo_handle))
                continue;
            const uint64_t sz = bo_size(fd, ent[j].bo_handle);
            if (sz >= (8ull << 20) && sz >= best_sz) {
                best_sz = sz;
                best_j = j;
                have_pres = 1;
            }
        }
        if (have_pres)
            tls.ent[nk++] = ent[best_j];
    } else {
        for (uint32_t j = 0; j < bl->bo_number; ++j) {
            if (is_dest_handle(fd, ent[j].bo_handle))
                continue;
            tls.ent[nk++] = ent[j];
        }
    }

    if (ip == 1 || nlog < 24) {
        cslog("ip=%u orig=%u keep=%u dest=%d slotdest=%d large=%d capslot=%d csfd=%d bolist=%u\n",
            ip, bl->bo_number, nk, n_dest, n_slot, n_large, capture_slot, fd,
            orig->in.bo_list_handle);
        if (nlog < 24)
            ++nlog;
    }
    if (nk == 0 || nk == bl->bo_number)
        return orig;

    memcpy(&tls.cs, orig, sizeof(tls.cs));
    memcpy(tls.chunk_ptrs, chunk_ptrs, orig->in.num_chunks * sizeof(uint64_t));
    for (uint32_t i = 0; i < orig->in.num_chunks; ++i) {
        struct drm_amdgpu_cs_chunk *ch =
            (struct drm_amdgpu_cs_chunk *)(uintptr_t)chunk_ptrs[i];
        if (!ch)
            continue;
        tls.chunks[i] = *ch;
        tls.chunk_ptrs[i] = (uint64_t)(uintptr_t)&tls.chunks[i];
    }
    tls.bl = *bl;
    tls.bl.bo_number = nk;
    tls.bl.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
    tls.bl.bo_info_ptr = (uint64_t)(uintptr_t)tls.ent;
    tls.chunks[bl_chunk].chunk_data = (uint64_t)(uintptr_t)&tls.bl;
    tls.chunks[bl_chunk].length_dw = (uint32_t)(sizeof(tls.bl) / 4);
    tls.cs.in.chunks = (uint64_t)(uintptr_t)tls.chunk_ptrs;
    tls.cs.in.bo_list_handle = 0;
    return &tls.cs;
}

__attribute__((constructor))
static void csstrip_init(void) {
    real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
    libdrm_h = dlopen("libdrm.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!libdrm_h)
        libdrm_h = dlopen("libdrm.so.2", RTLD_NOW);
    if (libdrm_h)
        real_drmIoctl = (int (*)(int, unsigned long, void *))dlsym(libdrm_h, "drmIoctl");
    cslog("loaded ioctl=%p drmIoctl=%p\n", (void *)real_ioctl, (void *)real_drmIoctl);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = NULL;
    if (_IOC_SIZE(request) != 0)
        arg = va_arg(ap, void *);
    va_end(ap);
    if (!real_ioctl)
        real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
    if (is_amdgpu_cs(request) && arg)
        arg = filter_cs(fd, (union drm_amdgpu_cs *)arg);
    if (_IOC_SIZE(request) != 0)
        return real_ioctl(fd, request, arg);
    return real_ioctl(fd, request, NULL);
}

int drmIoctl(int fd, unsigned long request, void *arg) {
    if (!real_drmIoctl) {
        errno = ENOSYS;
        return -1;
    }
    if (is_amdgpu_cs(request) && arg)
        arg = filter_cs(fd, (union drm_amdgpu_cs *)arg);
    return real_drmIoctl(fd, request, arg);
}
