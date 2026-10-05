# BO-list scope check — Mesa 26.2.3

Source: fresh `git clone --depth 1 --branch mesa-26.2.3`, verified `git describe` = `mesa-26.2.3`.
Question: which BOs reach the global list, and is the list per VkDevice or per winsys?

## 1. Every non-WSI allocation is made resident

`src/amd/vulkan/radv_device_memory.c:307-310`:

```c
   if (!wsi_info) {
      result = device->ws->buffer_make_resident(device->ws, mem->bo, true);
      if (result != VK_SUCCESS)
         goto fail;
   }
```

This is unconditional for every allocation except WSI images. The import branch
(`:174-186`) and the export branch (`:218`) both fall through to it, so
**imported, exported and plain BOs all become resident.**

## 2. Only `is_local` BOs are exempt

`src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c:1210-1215`:

```c
   /* Do not add the BO to the global list if it's a local BO because the
    * kernel maintains a list for us.
    */
   if (bo->base.is_local)
      return VK_SUCCESS;
```

`is_local` is set only at `radv_amdgpu_bo.c:604-608`:

```c
   if ((initial_domain & RADEON_DOMAIN_VRAM_GTT) && (flags & RADEON_FLAG_NO_INTERPROCESS_SHARING) &&
       ((ws->perftest & RADV_PERFTEST_LOCAL_BOS) || (flags & RADEON_FLAG_PREFER_LOCAL_BO))) {
      bo->base.is_local = true;
```

`RADEON_FLAG_NO_INTERPROCESS_SHARING` is required. A BO we must hand to another
process cannot set it, so **any buffer that crosses a process boundary is
non-local and lands on the global list** — imported or exported.

## 3. The list is per VkDevice

The list lives in the winsys (`radv_amdgpu_winsys.h:57-64`), and the winsys is
built inside `radv_CreateDevice`:

```c
radv_device.c:1460   result = radv_create_winsys(device);
radv_device.c:1346   result = radv_amdgpu_winsys_create(fd, &pdev->info, ..., &device->ws);
```

so each `VkDevice` gets its own `ws` and therefore its own `global_bo_list`.
Every submit then walks it (`radv_amdgpu_cs.c:902-908`):

```c
radv_amdgpu_copy_global_bo_list(struct radv_amdgpu_winsys *ws, struct drm_amdgpu_bo_list_entry *handles)
{
   for (uint32_t i = 0; i < ws->global_bo_list.count; i++) {
      handles[i].bo_handle = ws->global_bo_list.bos[i]->bo_handle;
      handles[i].bo_priority = ws->global_bo_list.bos[i]->priority;
   }
   return ws->global_bo_list.count;
}
```

## Verdict

- Per **VkDevice**: confirmed.
- Membership: **all non-WSI BOs**, including exported ones.

## What this rules out

The two-hop design — game device copies swapchain -> layer-owned image, layer
exports it, a copy device imports it and writes the udmabuf — does NOT avoid the
stall, because the layer-owned *exported* image is itself a non-local BO and
therefore on the copy device's global list. The copy device's submits would
carry it.

What still holds: a buffer imported on a **separate copy-only VkDevice** is not
on the *game* device's list, so the game's submits do not carry it. That was the
original motivation and it is correct. The failure we measured
(`MAPPING_ERROR` on `0000:04:00.0`) was a different cause: the copy device was
reading the game's VRAM swapchain images, which no second device on that card
can map. The fix is for the copy to read a buffer it owns, never the game's
images.