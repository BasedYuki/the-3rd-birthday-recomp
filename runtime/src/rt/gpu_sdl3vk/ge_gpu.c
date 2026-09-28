/* GPU GE rasterizer: full GPU rendering with persistent framebuffers (see README.md).
 *
 * AI disclosure: original Vulkan backend (NOT a port of PPSSPP's GPU), written with
 * substantial assistance from an LLM (Anthropic Claude). It reproduces PSP/GE pixel rules
 * derived from PPSSPP's software renderer (via ge.c), so it is GPLv2+. See CREDITS.md.
 *
 * Architecture: ge.c keeps doing vertex decode + T&L + clipping + primitive acceptance
 * (the parts validated against PPSSPP), then hands SCREEN-SPACE primitives to this
 * backend through the GeGpuHooks seam. Every primitive renders on the GPU — there is no
 * software-rasterizer fallback; unmappable blend modes are approximated (see
 * build_state). GE framebuffers are PERSISTENT GPU images (color RGBA8 + shared D16
 * depth keyed by zbp): no guest-VRAM upload/readback per flush. Guest VRAM is read only
 * when a target is first created (or invalidated by a CPU write: movie decode, DMA),
 * and written only on the rare paths that genuinely consume it (CLUT load from VRAM,
 * target eviction). Presentation blits the GPU image directly to the swapchain.
 *
 * Batches accumulate across GE lists and are submitted only when something consumes the
 * target: a retarget, a present, self-sampling (render-to-texture feedback), buffer
 * exhaustion. A submit is GPU-only (record + submit + fence); there is no pixel
 * conversion on the CPU in the steady state.
 *
 * Faithfulness notes (vs ge.c, the reference):
 *  - Coverage: x/y pre-snapped to the PSP 28.4 grid; Vulkan rasterizes at pixel centers
 *    with a top-left fill rule, matching raster_tri's integer edges.
 *  - Interpolation: all varyings noperspective; u*rw, v*rw, rw interpolate affinely and
 *    divide per pixel, exactly like the software inner loop.
 *  - Culling: draw_prim already reorders vertices for cull mode/strip parity; raster_tri
 *    keeps positive-cross (clockwise in Vulkan's framebuffer convention), so front face
 *    is CLOCKWISE with back-face culling.
 *  - Textures decode through ge.c's own sampler (ge_decode_tex_rgba); CLUT/swizzle is
 *    reference-correct by construction. Render-to-texture binds the target image
 *    directly when the address/stride match (a snapshot copy for feedback loops).
 *  - Approximated (no software fallback): doubled DST-alpha blend factors (halved),
 *    min/max/absdiff blend equations (factors ignored / subtract), two distinct FIX
 *    constants (dst rounded to ZERO/ONE), partial-byte write masks (>= 0x80 disables
 *    the channel), ordered dithering (skipped), lines/points (1px quads, not DDA).
 *    Doubled SRC-alpha factors are exact (premultiplied in the shader).
 */
#include "ge_gpu.h"
#include "sdl3vk.h"

#define _CRT_SECURE_NO_WARNINGS
#include "../recomp.h"
#include "../ge_shared.h"

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define FB_W 512            /* full PSP framebuffer stride */
#define FB_H 272
#define DRAW_W 480          /* pixels the GE can actually write */
/* Internal resolution: every target (and its depth) is s_sc times the PSP size in each
 * direction; geometry, scissors and blits scale with it, while guest VRAM (readback/upload)
 * and texel addressing stay at 1x. SW/SH are the scaled image size. */
static int s_sc = 1;
#define SW ((uint32_t)(FB_W * s_sc))
#define SH ((uint32_t)(FB_H * s_sc))
#define MAX_VERTS  524288
#define MAX_BATCH  4096
#define MAX_PIPES  192
#define MAX_TEX    1024
#define MAX_TGT    8
#define MAX_DEP    4

static const uint32_t k_vert_spv[] =
#include "psp_vert.inc"
;
static const uint32_t k_frag_spv[] =
#include "psp_frag.inc"
;

/* ---- captured vertex (must match psp.vert input layout) ------------------------------ */
typedef struct { float x, y, z, rw, u, v, fog; uint32_t rgba; } GpuVert;

/* fragment push constants (must match psp.frag PC block) */
typedef struct { int32_t cfg[4]; float texenv[4]; float fogcol[4]; float texsize[4]; int32_t depal[4]; } PushPC;
enum { F_TEX = 1, F_RGBA = 2, F_DBL = 4, F_FOG = 8, F_PERSP = 16, F_CLEAR = 32,
       F_NEAREST = 64, F_SA2X = 128, F_SA2XI = 256, F_DEPAL = 512 };

typedef struct {
    uint8_t blend_on, srcf, dstf, eq;      /* VkBlendFactor / VkBlendOp values */
    uint8_t cull;                          /* VkCullModeFlagBits */
    uint8_t ztest, zwrite, zfunc;          /* VkCompareOp */
    uint8_t cmask;                         /* VkColorComponentFlags */
} PipeKey;

typedef struct {
    PipeKey key;
    int32_t sx, sy, sw, sh;                /* scissor rect */
    float   bconst[4];                     /* blend constants (FIX factors) */
    PushPC  pc;
    VkDescriptorSet dset;
    uint32_t first, count;
} Batch;

typedef struct {
    uint64_t key, hash;                    /* state key + content hash; 0,0 = empty */
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
    VkDescriptorSet set;
    int w, h;
    int pending;                           /* referenced by not-yet-submitted batches */
    uint64_t lru;
} TexEnt;

typedef struct { PipeKey key; VkPipeline pipe; int used; } PipeEnt;

/* shared depth buffer, keyed by guest zbuf placement */
typedef struct {
    int used;
    uint32_t zba, zstride;                 /* key: zbp & 0x1FFFFF, stride */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
} DepthEnt;

/* persistent GE render target */
typedef struct {
    int used;
    uint32_t fba, stride, fmt;             /* key: framebuffer addr (masked), stride px, psm */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    VkDescriptorSet set_n, set_l;          /* sampled (nearest / linear) for RTT + blit src */
    DepthEnt *dep;
    VkFramebuffer fb;                      /* for (this color, this->dep) */
    int gpu_valid;                         /* GPU contents are the truth (newer than VRAM) */
    uint64_t lru;
    uint64_t render_gen;                   /* bumped per submit that rendered into this */
    uint64_t clean_gen;                    /* render_gen when guest VRAM last matched */
    uint32_t used_h;                       /* rows rendered since the last VRAM seed (0 = none) */
    int superseded;                        /* a newer target at the same address rendered since */
} Target;

/* Bytes of guest VRAM a target has actually rendered: only these rows can differ from VRAM.
 * (Using the full 272 rows made small effect buffers overlap textures stored right after them:
 * those draws took the readback path every frame.) */
static uint32_t tgt_len(const void *tp) {
    const Target *t = (const Target *)tp;
    return t->stride * t->used_h * (t->fmt == 3 ? 4u : 2u);
}
/* A target whose GPU contents are the current truth for its address range. */
static int tgt_live(const void *tp) {
    const Target *t = (const Target *)tp;
    return t->used && t->gpu_valid && !t->superseded;
}

/* ---- backend state -------------------------------------------------------------------- */
static int s_ready = 0;
static GeState  *s_ge;
static uint16_t *s_zbuf;

static VkPhysicalDevice s_pdev;
static VkDevice   s_dev;
static VkQueue    s_queue;
static VkCommandPool s_pool;
static VkCommandBuffer s_cmd;


static VkRenderPass s_rp;

static VkBuffer s_vbuf, s_xfer;            /* vertices; pixel transfer staging (1MB) */
static VkDeviceMemory s_vbuf_m, s_xfer_m;
static GpuVert *s_vmap;
static void    *s_xfer_map;

static VkDescriptorSetLayout s_dlayout;
static VkDescriptorPool s_dpool_tex, s_dpool_fix;
static VkPipelineLayout s_playout;
static VkShaderModule s_vs, s_fs;

static TexEnt  s_tex[MAX_TEX];
static int     s_tex_n = 0;
static VkImage s_white; static VkDeviceMemory s_white_mem;
static VkImageView s_white_view; static VkSampler s_white_smp;
static VkDescriptorSet s_white_set;

/* snapshot copy of a target for self-sampling (feedback) draws */
static VkImage s_snapimg; static VkDeviceMemory s_snapimg_mem;
static VkImageView s_snap_view; static VkImageLayout s_snap_layout;
static VkSampler s_smp_n, s_smp_l;         /* shared nearest/linear samplers (clamp) */
static VkDescriptorSet s_snap_n, s_snap_l;
static Target *s_snap_src = NULL;          /* what the snapshot currently holds */

/* Palettes for "depal" draws (a render target sampled as a CLUT16/CLUT32 texture): each
 * row of this image holds one decoded palette (entry i at x = i), bound as set 1. Rows are
 * handed out in a ring so pending batches keep the palette they were built with. */
#define CLUT_W    1024
#define CLUT_ROWS 64
static VkImage s_clutimg; static VkDeviceMemory s_clutimg_mem;
static VkImageView s_clut_view; static VkImageLayout s_clut_layout;
static VkDescriptorSet s_clut_set;
static int s_clut_row = -1;                /* row holding the palette below */
static uint64_t s_clut_row_hash = 0;
static unsigned long s_cnt_depal = 0;
/* Readbacks drain the GPU, so the stats line lists which call sites (source lines) caused them
 * in each 5 s window. */
#define RB_SITES 16
static struct { int line; unsigned long n; } s_rb_site[RB_SITES];
static uint64_t s_snap_srcgen = 0;

static PipeEnt s_pipes[MAX_PIPES];
static int s_pipe_n = 0;

static Target   s_tgts[MAX_TGT];
static DepthEnt s_deps[MAX_DEP];
static Target  *s_cur = NULL;              /* target of the pending batches */
static uint64_t s_lru = 1;

static Batch    s_batch[MAX_BATCH];
static uint32_t s_nbatch = 0, s_nverts = 0;

static int s_log = 0;
static unsigned long s_cnt_submit = 0, s_cnt_tri = 0, s_cnt_spr = 0, s_cnt_line = 0;
static unsigned long s_cnt_present_gpu = 0, s_cnt_present_cpu = 0, s_cnt_rtt = 0;
static unsigned long s_cnt_readback = 0, s_cnt_upload = 0, s_cnt_dirty = 0;
static unsigned long s_cnt_snap = 0, s_cnt_texup = 0, s_cnt_xferblit = 0;

static uint32_t s_texscratch[512 * 512];
static uint32_t s_pxscratch[FB_W * FB_H];

#define VKC(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "gegpu: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

/* ---- small helpers -------------------------------------------------------------------- */

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage;
    VKC(vkCreateBuffer(s_dev, &bci, NULL, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
    if (map) VKC(vkMapMemory(s_dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(s_dev, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindImageMemory(s_dev, *img, *mem, 0));
    return 1;
}

static int make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView *view) {
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(s_dev, &vci, NULL, view));
    return 1;
}

/* Transition an image whose layout we track to `want` (full barrier; correctness over
 * fine-grained sync — these happen a handful of times per frame). */
static void to_layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.oldLayout = *cur; mb.newLayout = want;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = aspect;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    *cur = want;
}

/* Host time spent in a few hot paths, reported with the 5 s stats line (microseconds). */
static uint64_t s_t_wait, s_t_texdec, s_t_readback, s_t_upload;
static unsigned long s_n_wait;
static uint64_t now_us(void) {
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

/* Command buffers. Work is submitted without waiting: the single queue executes submissions
 * in order and every layout change is an ALL_COMMANDS barrier, so GPU-side dependencies hold
 * across submissions. The CPU only waits when it needs results (readback, gpu_idle) or before
 * reusing a command buffer, staging range or vertex range the GPU may still be reading. (Each
 * draw flush used to wait for the GPU: ~8 round trips per frame, over a quarter of the frame
 * time at 4x resolution, which dropped frames and starved the audio in fights.) */
#define CMD_RING 32
static VkCommandBuffer s_cmds[CMD_RING];
static VkFence  s_fences[CMD_RING];
static int      s_inflight[CMD_RING];
static int      s_ci = 0;

static int cmd_begin(void) {
    s_cmd = s_cmds[s_ci];
    if (s_inflight[s_ci]) {
        const uint64_t t0 = now_us();
        VKC(vkWaitForFences(s_dev, 1, &s_fences[s_ci], VK_TRUE, UINT64_MAX));
        VKC(vkResetFences(s_dev, 1, &s_fences[s_ci]));
        s_inflight[s_ci] = 0;
        s_t_wait += now_us() - t0; s_n_wait++;
    }
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(s_cmd, &bi));
    return 1;
}

/* Submit the command buffer begun by cmd_begin without waiting for it. */
static int cmd_submit(void) {
    VKC(vkEndCommandBuffer(s_cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    VKC(vkQueueSubmit(s_queue, 1, &si, s_fences[s_ci]));
    s_inflight[s_ci] = 1;
    s_ci = (s_ci + 1) % CMD_RING;
    return 1;
}

/* Wait until the GPU has finished everything submitted so far. */
static void gpu_idle(void) {
    const uint64_t t0 = now_us();
    vkQueueWaitIdle(s_queue);
    s_t_wait += now_us() - t0; s_n_wait++;
}

/* Submit and wait: only where the CPU reads the results (readback into guest VRAM). */
static int cmd_submit_wait(void) {
    if (!cmd_submit()) return 0;
    gpu_idle();
    return 1;
}

/* Upload staging: a ring in one host-visible buffer. Every range is consumed by the submission
 * recorded right after it is filled; when the ring wraps, the GPU is drained first so no pending
 * copy still reads the range being overwritten. */
#define UP_SIZE (32u << 20)
static VkBuffer s_up; static VkDeviceMemory s_up_m; static uint8_t *s_up_map;
static uint32_t s_up_off = 0;
static uint32_t up_alloc(uint32_t n) {
    n = (n + 255u) & ~255u;
    if (s_up_off + n > UP_SIZE) { gpu_idle(); s_up_off = 0; }
    const uint32_t o = s_up_off;
    s_up_off += n;
    return o;
}

/* ---- guest framebuffer <-> RGBA8 (must match ge.c unpack_color / pack_fb exactly) ----- */

static uint32_t fb_unpack(uint32_t raw, uint32_t fmt) {
    uint32_t r, g, b, a;
    switch (fmt & 3) {
        case 0: r=(raw&0x1F)*255/31; g=((raw>>5)&0x3F)*255/63; b=((raw>>11)&0x1F)*255/31; a=255; break;
        case 1: r=(raw&0x1F)*255/31; g=((raw>>5)&0x1F)*255/31; b=((raw>>10)&0x1F)*255/31; a=(raw&0x8000)?255:0; break;
        case 2: r=(raw&0xF)*17; g=((raw>>4)&0xF)*17; b=((raw>>8)&0xF)*17; a=((raw>>12)&0xF)*17; break;
        default: return raw;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t fb_pack(uint32_t px, uint32_t fmt) {
    uint32_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF, a = px >> 24;
    switch (fmt & 3) {
        case 0: return ((r>>3)&0x1F) | (((g>>2)&0x3F)<<5) | (((b>>3)&0x1F)<<11);
        case 1: return ((r>>3)&0x1F) | (((g>>3)&0x1F)<<5) | (((b>>3)&0x1F)<<10) | (a>=128?0x8000u:0);
        case 2: return ((r>>4)&0xF) | (((g>>4)&0xF)<<4) | (((b>>4)&0xF)<<8) | (((a>>4)&0xF)<<12);
        default: return px;
    }
}

/* ---- pipelines ------------------------------------------------------------------------ */

static VkPipeline pipe_create(const PipeKey *k) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = s_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_fs; st[1].pName = "main";

    VkVertexInputBindingDescription bind = { 0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription at[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       16 },
        { 2, 0, VK_FORMAT_R32_SFLOAT,          24 },
        { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      28 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;   vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = at;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    /* raster_tri keeps POSITIVE cross product in y-down screen coords; Vulkan's signed
     * area has the opposite sign convention, so those triangles are Vulkan-"clockwise". */
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->ztest;
    ds.depthWriteEnable = k->zwrite;
    ds.depthCompareOp = (VkCompareOp)k->zfunc;

    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = k->blend_on;
    ba.srcColorBlendFactor = (VkBlendFactor)k->srcf;
    ba.dstColorBlendFactor = (VkBlendFactor)k->dstf;
    ba.colorBlendOp = (VkBlendOp)k->eq;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;    /* PSP: dst alpha = clamp(sa + da) */
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = k->cmask;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dn.dynamicStateCount = 3; dn.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb; pci.pDynamicState = &dn;
    pci.layout = s_playout; pci.renderPass = s_rp;

    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    if (r != VK_SUCCESS) fprintf(stderr, "gegpu: pipeline create failed: %d\n", (int)r);
    return p;
}

static VkPipeline pipe_get(const PipeKey *k) {
    for (int i = 0; i < s_pipe_n; i++)
        if (!memcmp(&s_pipes[i].key, k, sizeof(*k))) return s_pipes[i].pipe;
    if (s_pipe_n >= MAX_PIPES) return VK_NULL_HANDLE;
    VkPipeline p = pipe_create(k);
    if (!p) return VK_NULL_HANDLE;
    s_pipes[s_pipe_n].key = *k; s_pipes[s_pipe_n].pipe = p; s_pipe_n++;
    return p;
}

/* ---- descriptors ------------------------------------------------------------------------ */

static VkDescriptorSet make_descriptor(VkImageView view, VkSampler smp, VkDescriptorPool pool) {
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dlayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(s_dev, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo dii = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = set; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    return set;
}

/* ---- batch submission (GPU only, no VRAM I/O) ------------------------------------------- */

static uint32_t s_flushgen = 1;            /* bumped per submit: invalidates state template */

static void batches_reset(void) {
    s_nbatch = 0;            /* s_nverts keeps growing: the vertex buffer is a ring (ensure_room) */
    s_flushgen++;
    for (int i = 0; i < s_tex_n; i++) s_tex[i].pending = 0;
}

/* Render all pending batches into s_cur. Leaves the color image SHADER_READ_ONLY. */
static int submit_pending(void) {
    if (!s_nbatch || !s_cur) { if (s_nbatch == 0) batches_reset(); return 1; }
    Target *t = s_cur;
    if (!cmd_begin()) { batches_reset(); return 0; }

    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_layout(s_cmd, t->dep->img, VK_IMAGE_ASPECT_DEPTH_BIT, &t->dep->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = s_rp; rbi.framebuffer = t->fb;
    rbi.renderArea.extent.width = SW; rbi.renderArea.extent.height = SH;
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = { 0, 0, (float)SW, (float)SH, 0.0f, 1.0f };
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &zero);

    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 1, 1, &s_clut_set, 0, NULL);
    VkPipeline cur = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < s_nbatch; i++) {
        Batch *b = &s_batch[i];
        VkPipeline p = pipe_get(&b->key);
        if (!p) continue;
        if (p != cur) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); cur = p; }
        VkRect2D sc = { { b->sx * s_sc, b->sy * s_sc }, { (uint32_t)(b->sw * s_sc), (uint32_t)(b->sh * s_sc) } };
        vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdSetBlendConstants(s_cmd, b->bconst);
        vkCmdPushConstants(s_cmd, s_playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(b->pc), &b->pc);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1, &b->dset, 0, NULL);
        vkCmdDraw(s_cmd, b->count, 1, b->first, 0);
    }
    vkCmdEndRenderPass(s_cmd);
    /* render pass final layouts (see init): color -> SHADER_READ_ONLY, depth stays */
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    int ok = cmd_submit();
    t->gpu_valid = 1;
    t->render_gen++;
    s_cnt_submit++;
    /* rows these draws can have touched: lowest vertex, clamped to the scissor */
    for (uint32_t i = 0; i < s_nbatch; i++) {
        const Batch *b = &s_batch[i];
        float my = 0.0f;
        for (uint32_t v = b->first; v < b->first + b->count; v++)
            if (s_vmap[v].y > my) my = s_vmap[v].y;
        uint32_t bh = my >= (float)FB_H ? FB_H : (uint32_t)ceilf(my);
        const uint32_t lim = (uint32_t)(b->sy + b->sh);
        if (bh > lim) bh = lim;
        if (bh > t->used_h) t->used_h = bh;
    }
    /* the newest rendering at an address wins over other formats' targets there */
    t->superseded = 0;
    for (int i = 0; i < MAX_TGT; i++)
        if (&s_tgts[i] != t && s_tgts[i].used && s_tgts[i].fba == t->fba) s_tgts[i].superseded = 1;
    if (s_log)
        fprintf(stderr, "GEGPU submit #%lu batches=%u verts=%u tgt=0x%08x/%u fmt=%u\n",
                s_cnt_submit, s_nbatch, s_nverts, t->fba, t->stride, t->fmt);
    batches_reset();
    return ok;
}

static void stats_tick(void) {
    static unsigned long last_ms = 0;
    unsigned long now = (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC);
    if (now - last_ms < 5000) return;
    last_ms = now;
    fprintf(stderr, "GEGPU stats: submits=%lu tris=%lu spr=%lu lines=%lu rtt=%lu snap=%lu "
            "present[gpu=%lu cpu=%lu] upload=%lu readback=%lu texup=%lu dirty=%lu xferblit=%lu depal=%lu pipes=%d texs=%d\n",
            s_cnt_submit, s_cnt_tri, s_cnt_spr, s_cnt_line, s_cnt_rtt, s_cnt_snap,
            s_cnt_present_gpu, s_cnt_present_cpu, s_cnt_upload, s_cnt_readback, s_cnt_texup,
            s_cnt_dirty, s_cnt_xferblit, s_cnt_depal, s_pipe_n, s_tex_n);
    fprintf(stderr, "GEGPU time (5 s): gpu waits %lu = %.0f ms, tex decode %.0f ms, readback %.0f ms, upload %.0f ms\n",
            s_n_wait, s_t_wait / 1000.0, s_t_texdec / 1000.0, s_t_readback / 1000.0, s_t_upload / 1000.0);
    s_n_wait = 0; s_t_wait = s_t_texdec = s_t_readback = s_t_upload = 0;
    if (s_rb_site[0].line) {
        fprintf(stderr, "GEGPU readbacks by site (5 s):");
        for (int i = 0; i < RB_SITES && s_rb_site[i].line; i++)
            fprintf(stderr, " L%d=%lu", s_rb_site[i].line, s_rb_site[i].n);
        fprintf(stderr, "\n");
        memset(s_rb_site, 0, sizeof(s_rb_site));
    }
}

/* ---- targets ----------------------------------------------------------------------------- */

static int target_readback_impl(Target *t);
static int target_upload_impl(Target *t);
static int target_readback_at(Target *t, int line) {
    if (t && t->gpu_valid && t->clean_gen != t->render_gen)
        for (int i = 0; i < RB_SITES; i++)
            if (s_rb_site[i].line == line || !s_rb_site[i].line) { s_rb_site[i].line = line; s_rb_site[i].n++; break; }
    const uint64_t t0 = now_us(); int r = target_readback_impl(t); s_t_readback += now_us() - t0; return r;
}
#define target_readback(t) target_readback_at((t), __LINE__)
static int target_upload(Target *t) {
    const uint64_t t0 = now_us(); int r = target_upload_impl(t); s_t_upload += now_us() - t0; return r;
}

/* 1x staging image between a scaled target and guest VRAM (render scale > 1 only). */
static VkImage s_rsimg; static VkDeviceMemory s_rsimg_mem;
static VkImageLayout s_rs_layout = VK_IMAGE_LAYOUT_UNDEFINED;

/* Record a nearest-filtered blit between a scaled target image and the 1x staging image
 * (down = target -> staging). Leaves the source TRANSFER_SRC and the destination
 * TRANSFER_DST. */
static void rs_blit(Target *t, int down) {
    VkImage src = down ? t->img : s_rsimg, dst = down ? s_rsimg : t->img;
    VkImageLayout *sl = down ? &t->layout : &s_rs_layout, *dl = down ? &s_rs_layout : &t->layout;
    to_layout(s_cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, dl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {0};
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1].x = (int32_t)(down ? SW : FB_W); bl.srcOffsets[1].y = (int32_t)(down ? SH : FB_H);
    bl.srcOffsets[1].z = 1;
    bl.dstOffsets[1].x = (int32_t)(down ? FB_W : SW); bl.dstOffsets[1].y = (int32_t)(down ? FB_H : SH);
    bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(s_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
}

/* Write the target's GPU contents back to guest VRAM (eviction, CLUT-from-VRAM). */
static int target_readback_impl(Target *t) {
    if (!t->gpu_valid) return 1;
    if (t == s_cur && s_nbatch) submit_pending();
    if (t->clean_gen == t->render_gen) return 1;   /* VRAM already current */
    if (!cmd_begin()) return 0;
    VkImage from = t->img;
    if (s_sc > 1) {
        rs_blit(t, 1);
        to_layout(s_cmd, s_rsimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_rs_layout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        from = s_rsimg;
    } else {
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = FB_W; c.imageExtent.height = FB_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    if (!cmd_submit_wait()) return 0;
    const uint32_t *src = (const uint32_t *)s_xfer_map;
    uint32_t wb = t->stride < DRAW_W ? t->stride : DRAW_W;
    for (uint32_t y = 0; y < t->used_h && y < FB_H; y++) {   /* rows below were never rendered */
        if (t->fmt == 3) {
            memcpy((uint32_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 4),
                   src + y * FB_W, wb * 4);
        } else {
            uint16_t *dst = (uint16_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 2);
            for (uint32_t x = 0; x < wb; x++) dst[x] = (uint16_t)fb_pack(src[y * FB_W + x], t->fmt);
        }
    }
    t->clean_gen = t->render_gen;
    s_cnt_readback++;
    return 1;
}

/* Fill the target's color image from guest VRAM (creation / CPU-dirty reacquire). */
static int target_upload_impl(Target *t) {
    uint32_t *dst = s_pxscratch;
    for (uint32_t y = 0; y < FB_H; y++) {
        if (t->fmt == 3) {
            memcpy(dst + y * FB_W, (const uint32_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 4),
                   t->stride * 4);
        } else {
            const uint16_t *src = (const uint16_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 2);
            for (uint32_t x = 0; x < t->stride; x++) dst[y * FB_W + x] = fb_unpack(src[x], t->fmt);
        }
        for (uint32_t x = t->stride; x < FB_W; x++) dst[y * FB_W + x] = 0;
    }
    const uint32_t uo = up_alloc(FB_W * FB_H * 4);
    memcpy(s_up_map + uo, s_pxscratch, FB_W * FB_H * 4);
    if (!cmd_begin()) return 0;
    VkImage to = s_sc > 1 ? s_rsimg : t->img;
    to_layout(s_cmd, to, VK_IMAGE_ASPECT_COLOR_BIT, s_sc > 1 ? &s_rs_layout : &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.bufferOffset = uo;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = FB_W; c.imageExtent.height = FB_H; c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_up, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    if (s_sc > 1) rs_blit(t, 0);             /* 1x VRAM image -> scaled target */
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit()) return 0;
    t->gpu_valid = 1;
    t->render_gen++;                       /* image content changed (snapshot reuse check) */
    t->clean_gen = t->render_gen;          /* VRAM is the source: in sync by definition */
    t->used_h = 0;                         /* nothing newer than VRAM yet */
    s_cnt_upload++;
    return 1;
}

static void target_destroy(Target *t) {
    if (!t->used) return;
    gpu_idle();                            /* in-flight submissions may still use its image */
    if (s_snap_src == t) s_snap_src = NULL;
    if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
    if (t->set_n) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_n);
    if (t->set_l) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_l);
    if (t->view) vkDestroyImageView(s_dev, t->view, NULL);
    if (t->img) vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem) vkFreeMemory(s_dev, t->mem, NULL);
    memset(t, 0, sizeof(*t));
}

static DepthEnt *depth_acquire(uint32_t zba, uint32_t zstride) {
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && s_deps[i].zba == zba && s_deps[i].zstride == zstride)
            return &s_deps[i];
    DepthEnt *d = NULL;
    for (int i = 0; i < MAX_DEP; i++) if (!s_deps[i].used) { d = &s_deps[i]; break; }
    if (!d) {   /* evict slot 0 arbitrarily (depth contents are transient frame data) */
        vkDeviceWaitIdle(s_dev);
        d = &s_deps[0];
        vkDestroyImageView(s_dev, d->view, NULL);
        vkDestroyImage(s_dev, d->img, NULL);
        vkFreeMemory(s_dev, d->mem, NULL);
        /* targets holding this depth must drop their framebuffers */
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used && s_tgts[i].dep == d) {
                vkDestroyFramebuffer(s_dev, s_tgts[i].fb, NULL);
                s_tgts[i].fb = VK_NULL_HANDLE; s_tgts[i].dep = NULL;
            }
        memset(d, 0, sizeof(*d));
    }
    if (!make_image(SW, SH, VK_FORMAT_D16_UNORM,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &d->img, &d->mem)) return NULL;
    if (!make_view(d->img, VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, &d->view)) return NULL;
    d->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    d->zba = zba; d->zstride = zstride; d->used = 1;
    /* initialize from the (CPU) software z-buffer once — games clear depth before use,
     * this just avoids garbage on the very first frame */
    const uint32_t uo = up_alloc(FB_W * FB_H * 2);
    memcpy(s_up_map + uo, s_zbuf, FB_W * FB_H * 2);
    if (cmd_begin()) {
        to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c = {0};
        c.bufferOffset = uo;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = FB_W; c.imageExtent.height = FB_H; c.imageExtent.depth = 1;
        if (s_sc == 1) {
            vkCmdCopyBufferToImage(s_cmd, s_up, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        } else {                           /* scaled depth has no 1x source: start cleared */
            VkClearDepthStencilValue z0 = { 0.0f, 0 };
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vkCmdClearDepthStencilImage(s_cmd, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z0, 1, &rng);
        }
        to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        cmd_submit();
    }
    return d;
}

static Target *target_acquire(void) {
    uint32_t fba = s_ge->fbp & 0x001FFFFFu;
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    if (stride > 512) stride = 512;
    uint32_t fmt = s_ge->fbfmt & 3;
    uint32_t zba = s_ge->zbp & 0x001FFFFFu;
    uint32_t zstride = s_ge->zbw ? s_ge->zbw : stride;
    if (zstride > 512) zstride = 512;

    /* One target per (address, stride, format). Effects reuse small scratch buffers in two
     * formats alternately (the Overdive jump flips 0x154000 between 4444/64 and 5551/128 up
     * to 20 times a second); converting through guest VRAM on every flip (readback, destroy,
     * re-create, upload) stalled the game and broke up the audio. The other format's target
     * is kept and marked superseded once this one renders. SR_GPU_REINTERPRET_COPY=1 restores
     * the VRAM round trip for games that really read one format's pixels as another. */
    static int reint_copy = -1;
    if (reint_copy < 0) { const char *e = getenv("SR_GPU_REINTERPRET_COPY"); reint_copy = e && e[0] == '1'; }
    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].fba == fba && s_tgts[i].stride == stride && s_tgts[i].fmt == fmt) {
            t = &s_tgts[i]; break;
        }
    if (!t && reint_copy)
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used && s_tgts[i].fba == fba) { t = &s_tgts[i]; break; }

    if (t && (t->stride != stride || t->fmt != fmt)) {
        /* same address reinterpreted: preserve pixels through VRAM */
        static int nlog = 0;
        if (nlog < 30) {
            nlog++;
            fprintf(stderr, "GEGPU reinterpret: fba=%06x stride %u->%u fmt %u->%u (dirty=%d)\n",
                    (unsigned)fba, (unsigned)t->stride, (unsigned)stride, (unsigned)t->fmt, (unsigned)fmt,
                    t->gpu_valid && t->clean_gen != t->render_gen);
        }
        target_readback(t);
        if (t == s_cur) { submit_pending(); s_cur = NULL; }
        target_destroy(t);
        t = NULL;
    }

    if (!t) {
        for (int i = 0; i < MAX_TGT; i++) if (!s_tgts[i].used) { t = &s_tgts[i]; break; }
        if (!t) {   /* LRU eviction, preserving contents */
            Target *old = NULL;
            for (int i = 0; i < MAX_TGT; i++)
                if (s_tgts[i].used && (!old || s_tgts[i].lru < old->lru) && &s_tgts[i] != s_cur)
                    old = &s_tgts[i];
            target_readback(old);
            if (old == s_cur) { submit_pending(); s_cur = NULL; }
            target_destroy(old);
            t = old;
        }
        memset(t, 0, sizeof(*t));
        t->fba = fba; t->stride = stride; t->fmt = fmt; t->used = 1;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!make_image(SW, SH, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &t->img, &t->mem)) { t->used = 0; return NULL; }
        if (!make_view(t->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &t->view)) return NULL;
        t->set_n = make_descriptor(t->view, s_smp_n, s_dpool_fix);
        t->set_l = make_descriptor(t->view, s_smp_l, s_dpool_fix);
        if (!target_upload(t)) return NULL;   /* seed from guest VRAM */
    } else if (!t->gpu_valid) {
        if (!target_upload(t)) return NULL;   /* CPU dirtied VRAM (movie/DMA): re-seed */
    }

    DepthEnt *d = depth_acquire(zba, zstride);
    if (!d) return NULL;
    if (t->dep != d || !t->fb) {
        if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
        VkImageView views[2] = { t->view, d->view };
        VkFramebufferCreateInfo fbc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbc.renderPass = s_rp; fbc.attachmentCount = 2; fbc.pAttachments = views;
        fbc.width = SW; fbc.height = SH; fbc.layers = 1;
        if (vkCreateFramebuffer(s_dev, &fbc, NULL, &t->fb) != VK_SUCCESS) return NULL;
        t->dep = d;
    }
    t->lru = s_lru++;
    return t;
}

/* Ensure s_cur points at the GE's current target, submitting pending work on a switch. */
static int begin_target(void) {
    uint32_t fba = s_ge->fbp & 0x001FFFFFu;
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    if (stride > 512) stride = 512;
    if (s_cur && s_cur->used && s_cur->fba == fba &&
        s_cur->fmt == (s_ge->fbfmt & 3) && s_cur->stride == stride && s_cur->gpu_valid) {
        s_cur->lru = s_lru++;
        return 1;
    }
    if (s_nbatch) submit_pending();
    s_cur = target_acquire();
    return s_cur != NULL;
}

/* ---- texture cache (RAM textures, decoded by ge.c's reference sampler) ------------------- */

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t tex_hash(void) {
    uint32_t bpp_num;
    switch (s_ge->tex_fmt) {
        case 3: case 7: bpp_num = 32; break;
        case 4: bpp_num = 4;  break;
        case 5: bpp_num = 8;  break;
        default: bpp_num = 16; break;
    }
    uint32_t stride = s_ge->tex_bufw ? s_ge->tex_bufw : (uint32_t)s_ge->tex_w;
    uint64_t bytes = ((uint64_t)stride * (uint64_t)s_ge->tex_h * bpp_num) >> 3;
    if (bytes > 4u << 20) bytes = 4u << 20;
    const uint8_t *p = (const uint8_t *)SR_HOST(s_ge->tex_addr);
    uint64_t h = 1469598103934665603ull;
    h = fnv64(h, &bytes, sizeof(bytes));
    /* VRAM textures are often drawn into by the CPU a little at a time (the boot "saves data
     * automatically" notice is text written into a mostly black 512x512 image): sparse samples
     * miss that and the stale first upload shows (black). Hash all of it, 8 bytes at a time. */
    if ((s_ge->tex_addr & 0x0F000000u) == 0x04000000u) {
        const uint64_t *w = (const uint64_t *)p;
        for (uint64_t i = 0; i < bytes / 8; i++) h = (h ^ w[i]) * 0x100000001b3ull + (h >> 29);
        return h;
    }
    uint64_t step = bytes / 64; if (step < 16) step = 16;
    for (uint64_t off = 0; off + 16 <= bytes; off += step) h = fnv64(h, p + off, 16);
    if (bytes >= 16) h = fnv64(h, p + bytes - 16, 16);
    return h;
}

/* Palette identity for CLUT textures. This must be part of the cache KEY, not just the
 * content hash: particle systems (explosions, fire) draw one greyscale texture through
 * many palettes in a single frame. Keyed without the CLUT those draws alias one cache
 * entry and ping-pong it — a full CPU decode + GPU upload + queue wait on EVERY draw. */
static uint64_t clut_hash(void) {
    uint64_t h = 1469598103934665603ull;
    h = fnv64(h, s_ge->clutram, sizeof(s_ge->clutram));
    h = fnv64(h, &s_ge->clut_fmt, sizeof(s_ge->clut_fmt));
    return h;
}

/* Decode the current CLUT into a palette row for a depal draw and return the row. The row
 * is reused while the palette is unchanged; a wrap of the ring first renders the pending
 * batches, which may still reference the rows being overwritten. */
static int clut_row_get(void) {
    uint64_t h = clut_hash();
    if (s_clut_row >= 0 && h == s_clut_row_hash) return s_clut_row;
    int row = s_clut_row + 1;
    if (row >= CLUT_ROWS) { submit_pending(); row = 0; }
    const uint32_t uo = up_alloc(CLUT_W * 4);
    uint32_t *px = (uint32_t *)(s_up_map + uo);
    const uint32_t epsm = s_ge->clut_fmt & 3;
    for (uint32_t i = 0; i < CLUT_W; i++) {
        uint32_t raw;
        if (epsm == 3) memcpy(&raw, &s_ge->clutram[(i * 4) & 2047], 4);
        else { uint16_t r16; memcpy(&r16, &s_ge->clutram[(i * 2) & 2047], 2); raw = r16; }
        px[i] = fb_unpack(raw, epsm);
    }
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, s_clutimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_clut_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.bufferOffset = uo;
    bic.imageOffset.y = row;
    bic.imageExtent.width = CLUT_W; bic.imageExtent.height = 1; bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_up, s_clutimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    to_layout(s_cmd, s_clutimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_clut_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cmd_submit();
    s_clut_row = row;
    s_clut_row_hash = h;
    return row;
}

static uint64_t s_texlru = 1;

/* Evict the least-recently-used quarter of the cache (the working set survives; the
 * old full-clear rebuilt EVERY texture every frame once a scene exceeded the cap). */
static void tex_evict_lru(void) {
    vkDeviceWaitIdle(s_dev);
    int goal = s_tex_n - MAX_TEX / 4;
    while (s_tex_n > goal) {
        int v = 0;
        for (int i = 1; i < s_tex_n; i++)
            if (s_tex[i].lru < s_tex[v].lru) v = i;
        TexEnt *e = &s_tex[v];
        vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
        vkDestroySampler(s_dev, e->smp, NULL);
        vkDestroyImageView(s_dev, e->view, NULL);
        vkDestroyImage(s_dev, e->img, NULL);
        vkFreeMemory(s_dev, e->mem, NULL);
        *e = s_tex[--s_tex_n];
    }
}

static int tex_upload(VkImage img, const uint32_t *px, int w, int h) {
    const uint32_t uo = up_alloc((uint32_t)w * (uint32_t)h * 4);
    memcpy(s_up_map + uo, px, (size_t)w * (size_t)h * 4);
    if (!cmd_begin()) return 0;
    VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED;
    to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, &lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = {0};
    bic.bufferOffset = uo;
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)w; bic.imageExtent.height = (uint32_t)h; bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, &lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return cmd_submit();
}

static int tex_make(const uint32_t *px, int w, int h, int linear, int clamp_u, int clamp_v,
                    VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkSampler *smp) {
    if (!make_image((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img, mem))
        return 0;
    if (!tex_upload(*img, px, w, h)) return 0;
    if (!make_view(*img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, view)) return 0;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VKC(vkCreateSampler(s_dev, &sci, NULL, smp));
    return 1;
}

static VkDescriptorSet tex_get(void) {
    int w = s_ge->tex_w, h = s_ge->tex_h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return s_white_set;
    int linear  = (s_ge->tex_filter & 1) || ((s_ge->tex_filter >> 8) & 1);
    int clamp_u = s_ge->tex_wrap & 1, clamp_v = (s_ge->tex_wrap >> 8) & 1;
    uint64_t key = (uint64_t)s_ge->tex_addr | ((uint64_t)s_ge->tex_fmt << 32)
                 | ((uint64_t)(uint32_t)w << 36) | ((uint64_t)(uint32_t)h << 46)
                 | ((uint64_t)s_ge->tex_swizzle << 56) | ((uint64_t)(unsigned)linear << 57)
                 | ((uint64_t)(unsigned)clamp_u << 58) | ((uint64_t)(unsigned)clamp_v << 59);
    key ^= (uint64_t)s_ge->tex_bufw << 16;
    if (s_ge->tex_fmt >= 4 && s_ge->tex_fmt <= 7)
        key ^= clut_hash() | 1;        /* distinct entry per (texture, palette) pair */
    uint64_t hash = tex_hash();
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        if (e->key != key) continue;
        e->lru = s_texlru++;
        if (e->hash == hash) { e->pending = 1; return e->set; }
        /* same texture state, new contents: update in place (cache stays bounded) */
        if (e->pending) submit_pending();
        { const uint64_t t0 = now_us(); ge_decode_tex_rgba(s_texscratch); s_t_texdec += now_us() - t0; }
        if (!tex_upload(e->img, s_texscratch, e->w, e->h)) return s_white_set;
        e->hash = hash;
        e->pending = 1;
        s_cnt_texup++;
        return e->set;
    }

    if (s_tex_n >= MAX_TEX) {
        submit_pending();   /* batches reference sets about to be freed */
        tex_evict_lru();
    }
    { const uint64_t t0 = now_us(); ge_decode_tex_rgba(s_texscratch); s_t_texdec += now_us() - t0; }
    TexEnt *e = &s_tex[s_tex_n];
    memset(e, 0, sizeof(*e));
    if (!tex_make(s_texscratch, w, h, linear, clamp_u, clamp_v, &e->img, &e->mem, &e->view, &e->smp))
        return s_white_set;
    e->set = make_descriptor(e->view, e->smp, s_dpool_tex);
    if (!e->set) return s_white_set;
    e->key = key; e->hash = hash;
    e->w = w; e->h = h;
    e->pending = 1;
    e->lru = s_texlru++;
    s_cnt_texup++;
    s_tex_n++;
    return e->set;
}

/* ---- capture --------------------------------------------------------------------------- */

static float snap16(float v) { return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f); }

static void put_vert(float x, float y, float z, float rw, float u, float v, float fog,
                     int r, int g, int b, int a) {
    GpuVert *o = &s_vmap[s_nverts++];
    o->x = snap16(x); o->y = snap16(y); o->z = z; o->rw = rw;
    o->u = u; o->v = v; o->fog = fog;
    uint32_t cr = (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    uint32_t cg = (uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    uint32_t cb = (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    uint32_t ca = (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    o->rgba = cr | (cg << 8) | (cb << 16) | (ca << 24);
}

/* PSP blend factor -> VkBlendFactor + shader premultiply flags. Never fails: factors
 * with no VK equivalent are approximated (see file header). */
static int map_factor(uint32_t f, int src_side, uint32_t fixed, int *need_const, int *premul) {
    switch (f & 0xF) {
        case 0: return src_side ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
        case 1: return src_side ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 2: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 4: return VK_BLEND_FACTOR_DST_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 6:   /* 2*src alpha: exact on the src side (shader premultiplies rgb) */
            if (src_side) { *premul = F_SA2X; return VK_BLEND_FACTOR_ONE; }
            return VK_BLEND_FACTOR_SRC_ALPHA;                  /* dst side: halved approx */
        case 7:
            if (src_side) { *premul = F_SA2XI; return VK_BLEND_FACTOR_ONE; }
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_ALPHA;              /* 2*dst alpha approx */
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:
            if ((fixed & 0xFFFFFF) == 0)        return VK_BLEND_FACTOR_ZERO;
            if ((fixed & 0xFFFFFF) == 0xFFFFFF) return VK_BLEND_FACTOR_ONE;
            *need_const = 1;
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
    }
}

/* Build pipeline key + push constants for the current GE state. Always succeeds. */
static void build_state(int persp, int sprite, Batch *b) {
    GeState *g = s_ge;
    PipeKey *k = &b->key;
    memset(b, 0, sizeof(*b));

    int clear = g->clear;
    int premul = 0;

    /* scissor, clamped to the drawable region */
    int sx1 = g->scis_x1 > 0 ? g->scis_x1 : 0, sy1 = g->scis_y1 > 0 ? g->scis_y1 : 0;
    int sx2 = g->scis_x2 < DRAW_W - 1 ? g->scis_x2 : DRAW_W - 1;
    int sy2 = g->scis_y2 < FB_H - 1 ? g->scis_y2 : FB_H - 1;
    if (sx1 > sx2 || sy1 > sy2) { b->sw = 0; return; }   /* nothing drawable */
    b->sx = sx1; b->sy = sy1; b->sw = sx2 - sx1 + 1; b->sh = sy2 - sy1 + 1;

    /* color write mask (partial-byte masks approximate: >= 0x80 disables the channel) */
    if (clear) {
        k->cmask = ((g->clear_mode & 0x100) ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT) : 0)
                 | ((g->clear_mode & 0x200) ? VK_COLOR_COMPONENT_A_BIT : 0);
    } else {
        uint32_t mr = g->mask_rgb & 0xFF, mg = (g->mask_rgb >> 8) & 0xFF, mb = (g->mask_rgb >> 16) & 0xFF;
        uint32_t ma = (uint32_t)g->mask_alpha & 0xFF;
        k->cmask = (mr >= 0x80 ? 0 : VK_COLOR_COMPONENT_R_BIT) | (mg >= 0x80 ? 0 : VK_COLOR_COMPONENT_G_BIT)
                 | (mb >= 0x80 ? 0 : VK_COLOR_COMPONENT_B_BIT) | (ma >= 0x80 ? 0 : VK_COLOR_COMPONENT_A_BIT);
    }

    /* blending */
    b->bconst[3] = 1.0f;
    if (!clear && g->blend_enable) {
        uint32_t eq = (g->blend_mode >> 8) & 7;
        int nc_a = 0, nc_b = 0;
        int sf = map_factor(g->blend_mode & 0xF, 1, g->blend_fixa, &nc_a, &premul);
        int df = map_factor((g->blend_mode >> 4) & 0xF, 0, g->blend_fixb, &nc_b, &premul);
        if (nc_a && nc_b && (g->blend_fixa & 0xFFFFFF) != (g->blend_fixb & 0xFFFFFF)) {
            /* one constant register in VK: keep src exact, round dst to ZERO/ONE */
            uint32_t fb_ = g->blend_fixb;
            uint32_t lum = (fb_ & 0xFF) + ((fb_ >> 8) & 0xFF) + ((fb_ >> 16) & 0xFF);
            df = lum >= 384 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            nc_b = 0;
        }
        uint32_t fixed = nc_a ? g->blend_fixa : g->blend_fixb;
        b->bconst[0] = (float)(fixed & 0xFF) / 255.0f;
        b->bconst[1] = (float)((fixed >> 8) & 0xFF) / 255.0f;
        b->bconst[2] = (float)((fixed >> 16) & 0xFF) / 255.0f;
        k->blend_on = 1;
        k->srcf = (uint8_t)sf; k->dstf = (uint8_t)df;
        switch (eq) {
            case 1:  k->eq = VK_BLEND_OP_SUBTRACT; break;
            case 2:  k->eq = VK_BLEND_OP_REVERSE_SUBTRACT; break;
            case 3:  k->eq = VK_BLEND_OP_MIN; break;          /* factors ignored: approx */
            case 4:  k->eq = VK_BLEND_OP_MAX; break;
            case 5:  k->eq = VK_BLEND_OP_SUBTRACT; break;     /* absdiff: approx */
            default: k->eq = VK_BLEND_OP_ADD; break;
        }
    }

    /* depth */
    static const uint8_t zmap[8] = {
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    if (clear) {
        /* clear mode (sprites AND triangles): bit 10 = unconditional depth fill, no test */
        k->ztest = k->zwrite = (g->clear_mode & 0x400) ? 1 : 0;
        k->zfunc = VK_COMPARE_OP_ALWAYS;
    } else {
        k->ztest = g->ztest_enable ? 1 : 0;
        k->zwrite = (k->ztest && !g->zwrite_disable) ? 1 : 0;
        k->zfunc = k->ztest ? zmap[g->ztest & 7] : VK_COMPARE_OP_ALWAYS;
    }

    /* culling: draw_prim already reordered vertices; rasterizer keeps positive winding */
    k->cull = (!sprite && g->cull_enable && !clear) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;

    /* fragment config */
    int textured = g->tex_enable && !clear && g->tex_addr;
    int flags = premul;
    if (clear) flags |= F_CLEAR;
    if (persp && !(clear && sprite)) flags |= F_PERSP;   /* minz/maxz discard */
    if (persp && g->fog_enable && !clear) flags |= F_FOG;
    b->pc.texsize[0] = b->pc.texsize[1] = 1.0f;
    if (textured) {
        flags |= F_TEX;
        if (g->tex_func & 0x100)   flags |= F_RGBA;
        if (g->tex_func & 0x10000) flags |= F_DBL;
        int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
        if (!linear) flags |= F_NEAREST;
        b->pc.texsize[0] = (float)g->tex_w;
        b->pc.texsize[1] = (float)g->tex_h;
    }
    b->pc.cfg[1] = (!clear && g->atest_enable) ? (int32_t)g->atest : 1 /* func ALWAYS */;
    b->pc.cfg[2] = (int32_t)g->minz;
    b->pc.cfg[3] = (int32_t)g->maxz;
    b->pc.texenv[0] = (float)(g->tex_env & 0xFF) / 255.0f;
    b->pc.texenv[1] = (float)((g->tex_env >> 8) & 0xFF) / 255.0f;
    b->pc.texenv[2] = (float)((g->tex_env >> 16) & 0xFF) / 255.0f;
    b->pc.fogcol[0] = (float)(g->fog_color & 0xFF) / 255.0f;
    b->pc.fogcol[1] = (float)((g->fog_color >> 8) & 0xFF) / 255.0f;
    b->pc.fogcol[2] = (float)((g->fog_color >> 16) & 0xFF) / 255.0f;

    /* texture binding: a render target sampled directly (render-to-texture), or the
     * RAM-texture cache, or white for untextured */
    b->dset = s_white_set;
    if (textured) {
        Target *src = NULL;
        if ((g->tex_addr & 0x0F000000u) == 0x04000000u) {
            uint32_t ta = g->tex_addr & 0x001FFFFFu;
            for (int i = 0; i < MAX_TGT; i++) {
                Target *ti = &s_tgts[i];
                if (!tgt_live(ti)) continue;
                if (ta >= ti->fba && ta < ti->fba + tgt_len(ti)) { src = ti; break; }
            }
        }
        if (src) {
            static int nortt = -1;
            if (nortt < 0) { const char *nv = getenv("SR_GPU_NORTT");
                             nortt = (nv && nv[0] && strcmp(nv, "0") != 0) ? 1 : 0; }
            int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
            uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
            uint32_t toff = (g->tex_addr & 0x001FFFFFu) - src->fba;
            s_cnt_rtt++;
            /* GPU-direct: bind the target image, addressing any pixel-aligned sub-rect
             * through the texel offset in texsize.zw — no readback, no CPU decode.
             * tex_fmt must EQUAL the target's psm: reinterpreting one 16-bit format as
             * another scrambles channels/alpha and must go through VRAM pack/unpack.
             * SR_GPU_NORTT=1 forces the readback path for all draws (debug bisect). */
            /* A target read through a palette of the same texel width ("depal": CLUT32 over
             * an 8888 buffer, CLUT16 over a 16-bit one) also stays on the GPU: the shader
             * rebuilds the raw texel and looks it up in a palette row (clut_row_get). */
            int depal = (g->tex_fmt == 7 && src->fmt == 3) || (g->tex_fmt == 6 && src->fmt != 3);
            /* A depal read may also use a different row width than the target was rendered
             * with (the Overdive effect renders 128 wide and reads the same bytes 64 wide):
             * the shader then maps each texel through its linear VRAM offset (depal[3]). */
            const int remap = depal && g->tex_bufw && g->tex_bufw != src->stride;
            if (!nortt && (g->tex_fmt == src->fmt || depal) && !g->tex_swizzle &&
                (g->tex_bufw == src->stride || g->tex_bufw == 0 || remap) && (toff % bpp_s) == 0) {
                uint32_t pix = toff / bpp_s;
                if (depal) {
                    flags |= F_DEPAL;
                    b->pc.depal[0] = (int32_t)(src->fmt | ((g->clut_fmt & 3) << 4) | (linear << 8)
                                               | (src->stride << 12));
                    b->pc.depal[1] = (int32_t)(((g->clut_fmt >> 2) & 0x1F) | (((g->clut_fmt >> 8) & 0xFF) << 8)
                                               | ((((g->clut_fmt >> 16) & 0x1F) << 4) << 16));
                    b->pc.depal[2] = clut_row_get();
                    b->pc.depal[3] = remap ? (int32_t)g->tex_bufw : 0;
                    s_cnt_depal++;
                }
                if (src == s_cur) {
                    /* feedback loop: sample a snapshot copy; reuse the previous snapshot
                     * while nothing new has rendered into the target */
                    if (s_nbatch) submit_pending();
                    if (s_snap_src != src || s_snap_srcgen != src->render_gen) {
                        if (cmd_begin()) {
                            to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            VkImageCopy ic = {0};
                            ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            ic.srcSubresource.layerCount = 1;
                            ic.dstSubresource = ic.srcSubresource;
                            ic.extent.width = SW; ic.extent.height = SH; ic.extent.depth = 1;
                            vkCmdCopyImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           s_snapimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
                            to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            cmd_submit();
                        }
                        s_snap_src = src;
                        s_snap_srcgen = src->render_gen;
                        s_cnt_snap++;
                    }
                    b->dset = linear ? s_snap_l : s_snap_n;
                } else {
                    if (s_nbatch && src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        submit_pending();
                    if (src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && cmd_begin()) {
                        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        cmd_submit();
                    }
                    b->dset = linear ? src->set_l : src->set_n;
                }
                /* texel coords map 1:1 onto the 512x272 image, shifted by the sub-rect */
                b->pc.texsize[0] = (float)FB_W;
                b->pc.texsize[1] = (float)FB_H;
                b->pc.texsize[2] = (float)(pix % src->stride);
                b->pc.texsize[3] = (float)(pix / src->stride);
            } else {
                /* incompatible stride/format reinterpretation: VRAM + decoder (rare) */
                static int nlog = 0;
                if (nlog < 30) {
                    nlog++;
                    fprintf(stderr, "RTT slow: tex=%06x fmt=%u swz=%u bufw=%u %ux%u | tgt fba=%06x fmt=%u stride=%u cur=%d\n",
                            (unsigned)(g->tex_addr & 0x1FFFFFu), (unsigned)g->tex_fmt, (unsigned)g->tex_swizzle,
                            (unsigned)g->tex_bufw, (unsigned)g->tex_w, (unsigned)g->tex_h,
                            (unsigned)src->fba, (unsigned)src->fmt, (unsigned)src->stride, src == s_cur);
                }
                target_readback(src);
                b->dset = tex_get();
            }
        } else {
            b->dset = tex_get();
        }
    }
    b->pc.cfg[0] = (int32_t)((g->tex_func & 7) | ((uint32_t)flags << 8));
}

/* ---- built-state template (state rebuilt per CHANGE, not per primitive) ------------------ */

typedef struct {
    uint32_t clear, clear_mode;
    int blend_enable; uint32_t blend_mode, fixa, fixb;
    uint32_t mask_rgb; int mask_alpha;
    int scis[4];
    int ztest_enable, zwrite_disable; uint32_t ztest, minz, maxz;
    int cull_enable; uint32_t cull;
    int tex_enable; uint32_t tex_addr, tex_bufw, tex_fmt, tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h; int tex_swizzle;
    uint32_t clut_fmt, clut_gen;
    int atest_enable; uint32_t atest;
    int fog_enable; uint32_t fog_color;
    int persp, sprite;
    uint32_t flushgen;
} StateSnap;
static StateSnap s_snapst;
static Batch     s_tmpl;
static int       s_tmpl_ok = 0;

static void snap_fill(StateSnap *o, int persp, int sprite) {
    GeState *g = s_ge;
    memset(o, 0, sizeof(*o));
    o->clear = (uint32_t)g->clear; o->clear_mode = g->clear_mode;
    o->blend_enable = g->blend_enable; o->blend_mode = g->blend_mode;
    o->fixa = g->blend_fixa; o->fixb = g->blend_fixb;
    o->mask_rgb = g->mask_rgb; o->mask_alpha = g->mask_alpha;
    o->scis[0] = g->scis_x1; o->scis[1] = g->scis_y1; o->scis[2] = g->scis_x2; o->scis[3] = g->scis_y2;
    o->ztest_enable = g->ztest_enable; o->zwrite_disable = g->zwrite_disable;
    o->ztest = g->ztest; o->minz = g->minz; o->maxz = g->maxz;
    o->cull_enable = g->cull_enable; o->cull = g->cull;
    o->tex_enable = g->tex_enable; o->tex_addr = g->tex_addr; o->tex_bufw = g->tex_bufw;
    o->tex_fmt = g->tex_fmt; o->tex_wrap = g->tex_wrap; o->tex_func = g->tex_func;
    o->tex_env = g->tex_env; o->tex_filter = g->tex_filter;
    o->tex_w = g->tex_w; o->tex_h = g->tex_h; o->tex_swizzle = g->tex_swizzle;
    o->clut_fmt = g->clut_fmt; o->clut_gen = g->clut_gen;
    o->atest_enable = g->atest_enable; o->atest = g->atest;
    o->fog_enable = g->fog_enable; o->fog_color = g->fog_color;
    o->persp = persp; o->sprite = sprite;
    o->flushgen = s_flushgen;
}

static void state_get(int persp, int sprite, Batch *out) {
    StateSnap sn; snap_fill(&sn, persp, sprite);
    if (s_tmpl_ok && !memcmp(&sn, &s_snapst, sizeof(sn))) { *out = s_tmpl; return; }
    begin_target();                       /* build_state may bind sibling targets */
    build_state(persp, sprite, &s_tmpl);
    snap_fill(&s_snapst, persp, sprite);  /* re-fill: build_state may have submitted */
    s_tmpl_ok = 1;
    *out = s_tmpl;
}

static void append(Batch *b, uint32_t first, uint32_t count) {
    if (b->sw <= 0 || count == 0) return;
    if (s_nbatch) {
        Batch *last = &s_batch[s_nbatch - 1];
        if (last->first + last->count == first &&
            !memcmp(&last->key, &b->key, sizeof(b->key)) &&
            last->sx == b->sx && last->sy == b->sy && last->sw == b->sw && last->sh == b->sh &&
            !memcmp(last->bconst, b->bconst, sizeof(b->bconst)) &&
            !memcmp(&last->pc, &b->pc, sizeof(b->pc)) && last->dset == b->dset) {
            last->count += count;
            return;
        }
    }
    b->first = first; b->count = count;
    s_batch[s_nbatch++] = *b;
}

static void ensure_room(uint32_t verts) {
    if (s_nbatch >= MAX_BATCH - 1) submit_pending();
    if (s_nverts + verts > MAX_VERTS) {
        /* vertex ring full: submissions in flight may still read the start of the buffer */
        submit_pending();
        gpu_idle();
        s_nverts = 0;
    }
}

/* ---- primitive hooks ---------------------------------------------------------------------- */

static int hook_tri(const GeVtx *A, const GeVtx *B, const GeVtx *C, int persp) {
    if (!s_ready) return 0;
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    Batch b;
    state_get(persp, 0, &b);
    if (nocull) b.key.cull = VK_CULL_MODE_NONE;
    if (!begin_target()) return 1;        /* target alloc failed: drop draw, stay on GPU */
    ensure_room(3);
    uint32_t first = s_nverts;
    /* flat shading: provoking vertex is the LAST one on the PSP; clear triangles always
     * use C's color (raster_tri clear branch) */
    int flat = !s_ge->shade_gouraud || s_ge->clear;
    const GeVtx *ca = flat ? C : A;
    const GeVtx *cb = flat ? C : B;
    put_vert(A->x, A->y, A->z, A->rw, A->u, A->v, A->fog, ca->r, ca->g, ca->b, ca->a);
    put_vert(B->x, B->y, B->z, B->rw, B->u, B->v, B->fog, cb->r, cb->g, cb->b, cb->a);
    put_vert(C->x, C->y, C->z, C->rw, C->u, C->v, C->fog, C->r, C->g, C->b, C->a);
    append(&b, first, 3);
    s_cnt_tri++;
    return 1;
}

/* SR_GPU_SKIPTEX=<hex vram offset>: drop textured draws sampling that address (bisect a
 * post-effect pass without touching the game). */
static int skip_draw(void) {
    static long skip = -2;
    if (skip == -2) { const char *e = getenv("SR_GPU_SKIPTEX"); skip = e ? strtol(e, NULL, 16) : -1; }
    return skip >= 0 && s_ge->tex_enable && (long)(s_ge->tex_addr & 0x001FFFFFu) == skip;
}

static int hook_sprite(const GeVtx *p0, const GeVtx *p1, int persp) {
    if (!s_ready) return 0;
    if (skip_draw()) return 1;
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return 1;

    float xa = floorf(fminf(p0->x, p1->x)), xb = floorf(fmaxf(p0->x, p1->x));
    float ya = floorf(fminf(p0->y, p1->y)), yb = floorf(fmaxf(p0->y, p1->y));
    if (xb <= xa || yb <= ya) return 1;

    float u0 = p0->u, v0 = p0->v, u1 = p1->u, v1 = p1->v;
    if (persp) {
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    /* The quad's corners carry the vertex UVs, so each pixel centre interpolates to
     * u0 + (x - xa + 0.5) * du: the same pixel-centre convention as triangles. */
    float ua = u0, ub = u1;
    float va = v0, vb = v1;

    float z = p1->z, fog = p1->fog;
    int r = p1->r, g = p1->g, bb_ = p1->b, a = p1->a;

    ensure_room(6);
    uint32_t first = s_nverts;
    put_vert(xa, ya, z, 1.0f, ua, va, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xb, yb, z, 1.0f, ub, vb, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    append(&b, first, 6);
    s_cnt_spr++;
    return 1;
}

/* Lines as 1-pixel-wide quads (the software DDA's pixel set, approximately). */
static int hook_line(const GeVtx *A, const GeVtx *B, int persp) {
    if (!s_ready) return 0;
    Batch b;
    state_get(persp, 1, &b);              /* sprite semantics: no culling */
    if (!begin_target()) return 1;

    float dx = B->x - A->x, dy = B->y - A->y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;   /* half-width perpendicular */
    float ex = dx / len * 0.5f, ey = dy / len * 0.5f;    /* endpoint extension */
    float ax = A->x + 0.5f, ay = A->y + 0.5f;            /* DDA truncates; sample centers */
    float bx = B->x + 0.5f, by = B->y + 0.5f;

    /* flat shading uses B (the provoking vertex) */
    int flat = !s_ge->shade_gouraud;
    const GeVtx *cA = flat ? B : A;

    ensure_room(6);
    uint32_t first = s_nverts;
    put_vert(ax - ex - nx, ay - ey - ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex + nx, by + ey + ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    append(&b, first, 6);
    s_cnt_line++;
    return 1;
}

static int hook_point(const GeVtx *A, int persp) {
    if (!s_ready) return 0;
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return 1;
    float x0 = floorf(A->x), y0 = floorf(A->y);
    ensure_room(6);
    uint32_t first = s_nverts;
    put_vert(x0,        y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    append(&b, first, 6);
    s_cnt_line++;
    return 1;
}

/* ---- CPU writes to VRAM (movie decode, DMA): invalidate overlapping targets --------------- */

static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (!s_ready) return;
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = addr & 0x001FFFFFu;
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        uint32_t flen = t->stride * FB_H * bpp_t;
        if (a0 < t->fba + flen && t->fba < a0 + bytes) {
            if (t == s_cur && s_nbatch) submit_pending();
            t->gpu_valid = 0;             /* next use re-seeds from guest VRAM */
            s_cnt_dirty++;
        }
    }
}

/* CPU reads of VRAM (DMA memcpy source): materialize overlapping GPU-resident targets first. */
static void hook_vram_read(uint32_t addr, uint32_t bytes) {
    if (!s_ready) return;
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = addr & 0x001FFFFFu;
    int pending = 1;
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!tgt_live(t)) continue;
        if (a0 < t->fba + tgt_len(t) && t->fba < a0 + bytes) {
            if (pending) { submit_pending(); pending = 0; }
            target_readback(t);
        }
    }
}

/* ---- GE block transfer: GPU-side image blit ------------------------------------------------ */

/* Find the target whose address range contains VRAM offset `a` (gpu_valid not required). */
static Target *target_containing(uint32_t a) {
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || t->superseded) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        if (a >= t->fba && a < t->fba + t->stride * FB_H * bpp_t) return t;
    }
    return NULL;
}

/* Perform the block transfer as a vkCmdCopyImage between two framebuffer targets. Returns 1
 * on success (guest VRAM left stale for the destination, like any rendered-to target); 0
 * falls back to the CPU copy in ge.c (readback + memmove + dirty-invalidate). */
static int hook_xfer(uint32_t startdata) {
    if (!s_ready) return 0;
    uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
    uint32_t dstBase = (s_ge->xf_dst & 0xFFFFF0u) | ((s_ge->xf_dstw & 0xFF0000u) << 8);
    if ((srcBase & 0x0F000000u) != 0x04000000u || (dstBase & 0x0F000000u) != 0x04000000u)
        return 0;                                       /* RAM endpoint: CPU path */
    uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
    uint32_t dstStride = s_ge->xf_dstw & 0x7F8u;
    uint32_t bpp = (startdata & 1) ? 4 : 2;
    Target *src = target_containing(srcBase & 0x001FFFFFu);
    Target *dst = target_containing(dstBase & 0x001FFFFFu);
    if (!src || !dst || src == dst || !src->gpu_valid) return 0;
    uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
    /* raw-byte copy must match the converted RGBA images texel-for-texel */
    if (src->fmt != dst->fmt || bpp_s != bpp) return 0;
    if (src->stride != srcStride || dst->stride != dstStride) return 0;
    uint32_t soff = ((srcBase & 0x001FFFFFu) - src->fba) / bpp;
    uint32_t doff = ((dstBase & 0x001FFFFFu) - dst->fba) / bpp;
    uint32_t sx = (s_ge->xf_spos & 0x3FFu) + soff % src->stride;
    uint32_t sy = ((s_ge->xf_spos >> 10) & 0x3FFu) + soff / src->stride;
    uint32_t dx = (s_ge->xf_dpos & 0x3FFu) + doff % dst->stride;
    uint32_t dy = ((s_ge->xf_dpos >> 10) & 0x3FFu) + doff / dst->stride;
    uint32_t w = (s_ge->xf_size & 0x3FFu) + 1, h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
    if (sx + w > FB_W || dx + w > FB_W || sy + h > FB_H || dy + h > FB_H) return 0;
    if (!dst->gpu_valid && !target_upload(dst)) return 0;   /* seed rows we don't overwrite */
    if (s_nbatch && (s_cur == src || s_cur == dst)) submit_pending();
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.srcOffset.x = (int32_t)(sx * s_sc); ic.srcOffset.y = (int32_t)(sy * s_sc);
    ic.dstOffset.x = (int32_t)(dx * s_sc); ic.dstOffset.y = (int32_t)(dy * s_sc);
    ic.extent.width = w * s_sc; ic.extent.height = h * s_sc; ic.extent.depth = 1;
    vkCmdCopyImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit()) return 0;
    dst->render_gen++;                       /* content changed (snapshot/present tracking) */
    if (dy + h > dst->used_h) dst->used_h = dy + h > FB_H ? FB_H : dy + h;
    dst->lru = s_lru++;
    s_cnt_xferblit++;
    return 1;
}

/* ---- GE sync points ------------------------------------------------------------------------ */

void gegpu_flush(const char *reason) {
    if (!s_ready) return;
    if (reason && strcmp(reason, "listend") == 0) { stats_tick(); return; }
    if (reason && strcmp(reason, "xfersrc") == 0) {
        /* GE block transfer is about to READ guest memory: materialize any GPU-resident
         * target overlapping the source rows into guest VRAM (the transfer itself runs on
         * the CPU in ge.c; the destination range is invalidated via vram_dirty after). */
        submit_pending();
        uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
        if ((srcBase & 0x0F000000u) != 0x04000000u) return;
        uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
        uint32_t srcY = (s_ge->xf_spos >> 10) & 0x3FFu;
        uint32_t h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
        uint32_t a0 = srcBase & 0x001FFFFFu;
        uint32_t bytes = (srcY + h) * (srcStride ? srcStride : 512u) * 4u;  /* conservative */
        for (int i = 0; i < MAX_TGT; i++) {
            Target *t = &s_tgts[i];
            if (!tgt_live(t)) continue;
            if (a0 < t->fba + tgt_len(t) && t->fba < a0 + bytes)
                target_readback(t);
        }
        return;
    }
    if (reason && strcmp(reason, "syncall") == 0) {
        /* debug dumps: materialize every GPU-resident target into guest VRAM */
        submit_pending();
        for (int i = 0; i < MAX_TGT; i++)
            if (tgt_live(&s_tgts[i])) target_readback(&s_tgts[i]);
        return;
    }
    if (reason && strcmp(reason, "loadclut") == 0) {
        /* the CLUT loader is about to READ guest VRAM: materialize any target there */
        uint32_t ca = s_ge->clut_addr & 0x001FFFFFu;
        for (int i = 0; i < MAX_TGT; i++) {
            Target *t = &s_tgts[i];
            if (!tgt_live(t)) continue;
            if (ca < t->fba + tgt_len(t) && t->fba < ca + 2048)
                target_readback(t);
        }
        return;
    }
    submit_pending();
}

/* Present: hand the GPU image straight to the swapchain. Returns 0 if this address is
 * not GPU-resident (CPU-written movie frames, pre-GPU content) — caller uses the
 * guest-VRAM path. */
int gegpu_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    (void)stride;
    if (!s_ready) return -1;
    stats_tick();
    uint32_t fba = fbaddr & 0x001FFFFFu;
    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (tgt_live(&s_tgts[i]) && s_tgts[i].fba == fba) { t = &s_tgts[i]; break; }
    if (!t || !t->gpu_valid || (int)t->fmt != (fmt & 3)) { s_cnt_present_cpu++; return -1; }
    if (t == s_cur && s_nbatch) submit_pending();

    static int safe = -1;
    if (safe < 0) { const char *sv = getenv("SR_GPU_SAFE"); safe = (sv && sv[0] && strcmp(sv, "0") != 0) ? 1 : 0; }
    if (safe) target_readback(t);         /* keep guest VRAM current (A/B compare, debug) */

    if (t->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        if (!cmd_begin()) return -1;
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (!cmd_submit()) return -1;
    }
    s_cnt_present_gpu++;
    return sdl3vk_present_image((void *)t->img);
}

/* ---- init ----------------------------------------------------------------------------------- */

static const GeGpuHooks k_hooks = {
    hook_tri, hook_sprite, hook_line, hook_point, gegpu_flush, hook_vram_dirty, hook_xfer, hook_vram_read,
};

int gegpu_init(void) {
    Sdl3VkInfo vi;
    if (!sdl3vk_get_vk(&vi)) { fprintf(stderr, "gegpu: sdl3vk not initialized\n"); return 0; }
    s_pdev  = (VkPhysicalDevice)vi.physical;
    s_dev   = (VkDevice)vi.device;
    s_queue = (VkQueue)vi.queue;
    s_ge    = ge_state_ptr();
    s_zbuf  = ge_zbuf_ptr();
    {
        const char *lg = getenv("SR_GPU_LOG");
        s_log = (lg && lg[0] && strcmp(lg, "0") != 0) ? 1 : 0;
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vi.queue_family;
    VKC(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = CMD_RING;
    VKC(vkAllocateCommandBuffers(s_dev, &cbi, s_cmds));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (int i = 0; i < CMD_RING; i++) VKC(vkCreateFence(s_dev, &fci, NULL, &s_fences[i]));
    s_cmd = s_cmds[0];

    /* render pass: LOAD/STORE both attachments; color ends SHADER_READ_ONLY so finished
     * targets are always samplable/presentable, depth stays an attachment */
    VkAttachmentDescription at[2] = {0};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1] = at[0];
    at[1].format = VK_FORMAT_D16_UNORM;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference zref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &zref;
    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpc.attachmentCount = 2; rpc.pAttachments = at;
    rpc.subpassCount = 1; rpc.pSubpasses = &sub;
    VKC(vkCreateRenderPass(s_dev, &rpc, NULL, &s_rp));

    if (!make_buffer(MAX_VERTS * sizeof(GpuVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     &s_vbuf, &s_vbuf_m, (void **)&s_vmap)) return 0;
    if (!make_buffer(1u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &s_xfer, &s_xfer_m, &s_xfer_map)) return 0;
    if (!make_buffer(UP_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &s_up, &s_up_m, (void **)&s_up_map)) return 0;

    VkDescriptorSetLayoutBinding db = {0};
    db.binding = 0; db.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db.descriptorCount = 1; db.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 1; dlc.pBindings = &db;
    VKC(vkCreateDescriptorSetLayout(s_dev, &dlc, NULL, &s_dlayout));

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEX };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = MAX_TEX; dpc.poolSizeCount = 1; dpc.pPoolSizes = &dps;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;   /* LRU eviction */
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_tex));
    VkDescriptorPoolSize dpsf = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_TGT + 4 };
    dpc.maxSets = 2 * MAX_TGT + 4; dpc.pPoolSizes = &dpsf;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_fix));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushPC) };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout sets[2] = { s_dlayout, s_dlayout };   /* texture, depal palettes */
    plc.setLayoutCount = 2; plc.pSetLayouts = sets;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(s_dev, &plc, NULL, &s_playout));

    VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smc.codeSize = sizeof(k_vert_spv); smc.pCode = k_vert_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_vs));
    smc.codeSize = sizeof(k_frag_spv); smc.pCode = k_frag_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_fs));

    /* shared clamp samplers (render-target sampling + snapshot) */
    {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_n));
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_l));
    }

    /* 1x1 white texture for untextured draws */
    {
        uint32_t white = 0xFFFFFFFFu;
        if (!tex_make(&white, 1, 1, 0, 1, 1, &s_white, &s_white_mem, &s_white_view, &s_white_smp))
            return 0;
        s_white_set = make_descriptor(s_white_view, s_white_smp, s_dpool_fix);
        if (!s_white_set) return 0;
    }

    s_sc = sdl3vk_render_scale();
    if (s_sc > 1) {
        if (!make_image(FB_W, FB_H, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &s_rsimg, &s_rsimg_mem)) return 0;
    }
    fprintf(stderr, "gegpu: internal resolution %ux%u (scale %d)\n", DRAW_W * s_sc, FB_H * s_sc, s_sc);

    /* snapshot image for feedback (self-sampling) draws */
    if (!make_image(SW, SH, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &s_snapimg, &s_snapimg_mem)) return 0;
    if (!make_view(s_snapimg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_view)) return 0;
    s_snap_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_snap_n = make_descriptor(s_snap_view, s_smp_n, s_dpool_fix);
    s_snap_l = make_descriptor(s_snap_view, s_smp_l, s_dpool_fix);
    if (!s_snap_n || !s_snap_l) return 0;

    /* palette rows for depal draws; row contents are defined before any draw reads them */
    if (!make_image(CLUT_W, CLUT_ROWS, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &s_clutimg, &s_clutimg_mem)) return 0;
    if (!make_view(s_clutimg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &s_clut_view)) return 0;
    s_clut_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, s_clutimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_clut_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait()) return 0;
    s_clut_set = make_descriptor(s_clut_view, s_smp_n, s_dpool_fix);
    if (!s_clut_set) return 0;

    s_ready = 1;
    ge_set_gpu_hooks(&k_hooks);
    fprintf(stderr, "gegpu: full GPU GE active (persistent targets, no software fallback)\n");
    return 1;
}

void gegpu_shutdown(void) {
    if (!s_ready) return;
    submit_pending();
    ge_set_gpu_hooks(NULL);
    s_ready = 0;
    vkDeviceWaitIdle(s_dev);
    /* process exit follows immediately; Vulkan objects die with the device */
}
