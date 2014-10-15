#include <hwcomposer_drm.h>
#include "gralloc_priv.h"

struct hwc_fourcc {
    int hwc_format;
    unsigned int fourcc;
};

static const struct hwc_fourcc to_fourcc[] = {
    {HAL_PIXEL_FORMAT_RGBA_8888, DRM_FORMAT_ARGB8888},
    {HAL_PIXEL_FORMAT_RGBX_8888, DRM_FORMAT_XRGB8888},
    {HAL_PIXEL_FORMAT_BGRA_8888, DRM_FORMAT_BGRA8888},
    {HAL_PIXEL_FORMAT_RGB_888,   DRM_FORMAT_RGB888},
    {HAL_PIXEL_FORMAT_RGB_565,   DRM_FORMAT_RGB565},
    {HAL_PIXEL_FORMAT_YV12,      DRM_FORMAT_NV12},
};

static unsigned int hnd_to_fourcc(private_handle_t const *hnd)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(to_fourcc); i++)
	if (to_fourcc[i].hwc_format == hnd->format)
	   return to_fourcc[i].fourcc;

    return 0;
}

static int send_vsync_request(hwc_context_t *ctx, int disp)
{
    int ret = 0;

    drmVBlank vbl;

    vbl.request.type = (drmVBlankSeqType) (DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
    if (disp) //TODO: Use high crtc flag
        vbl.request.type = (drmVBlankSeqType) (DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT | DRM_VBLANK_SECONDARY);
    vbl.request.sequence = 1;
    vbl.request.signal = (unsigned long)&ctx->displays[disp];

    ret = drmWaitVBlank(ctx->drm_fd, &vbl);
    if (ret < 0)
        ALOGE("Failed to request vsync %d", errno);

    return ret;
}

static void vblank_handler(int fd, unsigned int frame, unsigned int sec,
        unsigned int usec, void *data)
{
    kms_display_t *kdisp = (kms_display_t *)data;
    const hwc_procs_t *procs = kdisp->ctx->cb_procs;

    if (kdisp->vsync_on) {
        int64_t ts = sec * (int64_t)1000000000 + usec * (int64_t)1000;
        procs->vsync(procs, 0, ts);
    }
}

static int init_display(hwc_context_t *ctx, int disp)
{
	kms_display_t *d = &ctx->displays[disp];
	const char *modules[] = { "i915", "radeon", "nouveau", "vmwgfx", "omapdrm", "exynos", "tilcdc", "msm", "sti" };
	int drm_fd;
	drmModeResPtr resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeModeInfoPtr mode;
	uint32_t possible_crtcs;
	private_module_t *m = NULL;

	/* open drm only once for all the displays */
	if (ctx->drm_fd < 0) {
	   /* Open DRM device */
	   for (unsigned int i = 0; i < ARRAY_SIZE(modules); i++) {
	       drm_fd = drmOpen(modules[i], NULL);
	         if (drm_fd >= 0) {
	            ALOGI("Open %s drm device (%d)\n", modules[i], drm_fd);
		    break;
		}
	   }
	   if (drm_fd < 0) {
		ALOGE("Failed to open DRM: %s\n", strerror(errno));
		return -EINVAL;
	   }

	   ctx->drm_fd = drm_fd;
	} else {
	   drm_fd = ctx->drm_fd;
	}

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		ALOGE("Failed to get resources: %s\n", strerror(errno));
		goto close;
	}

	connector = drmModeGetConnector(drm_fd, resources->connectors[disp]);
	if (!connector) {
		ALOGE("No connector for DISPLAY %d\n", disp);
		goto free_ressources;
	}

	mode = &connector->modes[0];

	encoder = drmModeGetEncoder(drm_fd, connector->encoders[0]);
	if (!encoder) {
		ALOGE("Failed to get encoder\n");
		goto free_connector;
	}

	if (!(encoder->possible_crtcs & (1 << disp)))
		goto free_encoder;

	d->con = connector;
	d->enc = encoder;
	d->crtc_id = resources->crtcs[disp];
	d->mode = mode;
	d->evctx.version = DRM_EVENT_CONTEXT_VERSION;
	d->evctx.vblank_handler = vblank_handler;
	d->ctx = ctx;

	drmModeFreeResources(resources);

	return 0;

free_encoder:
	drmModeFreeEncoder(encoder);
free_connector:
	drmModeFreeConnector(connector);
free_ressources:
	drmModeFreeResources(resources);
close:
	return -1;
}

static void destroy_display(int drm_fd, kms_display_t *d)
{
    if (d->crtc)
        drmModeFreeCrtc(d->crtc);
    if (d->enc)
        drmModeFreeEncoder(d->enc);
    if (d->con)
        drmModeFreeConnector(d->con);
    memset(d, 0, sizeof(*d));
}

static void dump_layer(hwc_layer_1_t *l)
{
    ALOGI("Layer type=%d, flags=0x%08x, handle=0x%p, tr=0x%02x, blend=0x%04x,"
        " {%d,%d,%d,%d} -> {%d,%d,%d,%d}, acquireFd=%d, releaseFd=%d",
        l->compositionType, l->flags, l->handle, l->transform, l->blending,
        l->sourceCrop.left, l->sourceCrop.top,
        l->sourceCrop.right, l->sourceCrop.bottom,
        l->displayFrame.left, l->displayFrame.top,
        l->displayFrame.right, l->displayFrame.bottom,
        l->acquireFenceFd, l->releaseFenceFd);
}

static void *event_handler (void *arg)
{
    hwc_context_t *ctx = (hwc_context_t *)arg;
    pthread_mutex_t *mutex = &ctx->ctx_mutex;
    int drm_fd = ctx->drm_fd;
    drmEventContext evctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = vblank_handler,
	.page_flip_handler = NULL,
    };
    struct pollfd pfds[1] = { { .fd = drm_fd, .events = POLLIN, .revents = POLLERR} };

    while (1) {
        int ret = poll(pfds, ARRAY_SIZE(pfds), 60000);
        if (ret < 0) {
            ALOGE("Event handler error %d", errno);
            break;
        } else if (ret == 0) {
            ALOGI("Event handler timeout");
            continue;
        }
        for (int i=0; i<ret; i++) {
            if (pfds[i].fd == drm_fd)
                drmHandleEvent(drm_fd, &evctx);
        }
    }
    return NULL;
}

static bool is_display_connected(hwc_context_t *ctx, int disp)
{
    if ((disp != HWC_DISPLAY_PRIMARY) && (disp !=HWC_DISPLAY_EXTERNAL))
	return false;

    if (!ctx->displays[disp].con)
	return false;

    if (ctx->displays[disp].con->connection == DRM_MODE_CONNECTED)
	return true;

    return false;
}

static bool set_zorder(hwc_context_t *ctx, int plane_id, int zorder)
{
    drmModeObjectPropertiesPtr properties = NULL;
    drmModePropertyPtr property = NULL;
    int i, ret;

    properties = drmModeObjectGetProperties(ctx->drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);

    if (!properties)
	return false;

    for (i = 0; i < (int) properties->count_props; ++i) {
	property = drmModeGetProperty(ctx->drm_fd, properties->props[i]);
	if (!property)
	   continue;
	if (strcmp(property->name, "zpos") == 0)
	   break;
	drmModeFreeProperty(property);
    }

    if (i == (int)properties->count_props)
	goto free_properties;

    ret = drmModeObjectSetProperty(ctx->drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, property->prop_id, zorder);
    drmModeFreeProperty(property);

free_properties:
    drmModeFreeObjectProperties(properties);
    return !!ret;
}

static int update_display(hwc_context_t *ctx, int disp,
        hwc_display_contents_1_t *display)
{
    int ret = 0;
    uint32_t width = 0, height = 0;
    uint32_t fb = 0;
    uint32_t bo[4] = { 0 };
    uint32_t pitch[4] = { 0 };
    uint32_t offset[4] = { 0 };
    hwc_layer_1_t *target = NULL;

    kms_display_t *kdisp = &ctx->displays[disp];

    if (!is_display_connected(ctx, disp))
        return 0;

    for (size_t i = 0; i < display->numHwLayers; i++) {
	hwc_layer_1_t *target = &display->hwLayers[i];
	private_handle_t const *hnd = reinterpret_cast<private_handle_t const *>(target->handle);
	unsigned int fourcc = hnd_to_fourcc(hnd);

	if (!(hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)) {
	    AERR("private_handle_t isn't using ION, hnd->flags %d", hnd->flags);
	    return -EINVAL;
	}

	if (display->hwLayers[i].acquireFenceFd != -1) {
	    ret = sync_wait(display->hwLayers[i].acquireFenceFd, 1000);
            if(ret < 0) {
                ALOGE("%s: sync_wait error!! error no = %d err str = %s",
                                    __FUNCTION__, errno, strerror(errno));
            }
            close(display->hwLayers[i].acquireFenceFd);
            display->hwLayers[i].acquireFenceFd = -1;
	}

	width = hnd->width;
	height = hnd->height;

	ret = drmPrimeFDToHandle (ctx->drm_fd, hnd->share_fd, &bo[0]);
	if (ret) {
		ALOGE("Failed to get fd for DUMB buffer %s", strerror(errno));
		return ret;
	}
	pitch[0] = width * 4; //stride

	/* force format for framebuffer because GPU claim use RGBA */
	if (display->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET)
	    fourcc = DRM_FORMAT_ARGB8888;

	ret = drmModeAddFB2(ctx->drm_fd, width, height, fourcc,
            bo, pitch, offset, &fb, 0);
	if (ret) {
		ALOGE("cannot create framebuffer (%d): %m\n",errno);
		return ret;
	}

	if (display->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET) {
		drmModeSetCrtc(ctx->drm_fd, kdisp->crtc_id, fb, 0, 0,
			&kdisp->con->connector_id, 1, kdisp->mode);
	}

	if (display->hwLayers[i].compositionType == HWC_OVERLAY) {
		int plane_id = hnd->plane_id;

		set_zorder(ctx, plane_id, i + 1);

		drmModeSetPlane(ctx->drm_fd, plane_id, kdisp->crtc_id, fb, 0,
				target->displayFrame.left,
				target->displayFrame.top,
				target->displayFrame.right - target->displayFrame.left,
				target->displayFrame.bottom - target->displayFrame.top,
				target->sourceCrop.left,
				target->sourceCrop.top,
				(target->sourceCrop.right - target->sourceCrop.left) << 16,
				(target->sourceCrop.bottom - target->sourceCrop.top) << 16);

	}
    }

    /* Clean up */
    if (kdisp->last_fb) {
	drmModeFBPtr lastFBPtr;
	uint32_t handle;
	struct drm_gem_close close_args;

	lastFBPtr = drmModeGetFB(ctx->drm_fd, kdisp->last_fb);
	handle = lastFBPtr->handle;
        drmModeRmFB(ctx->drm_fd, kdisp->last_fb);

	close_args.handle = handle;
	ret = drmIoctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_args);
	if(ret) {
		ALOGE("Failed to release buffer (Handle = 0x%x): %d\n", handle, ret);
		return ret;
	}
    }
    kdisp->last_fb = fb;

    return 0;
}

static int hwc_set (struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (! numDisplays || ! displays)
        return 0;

    hwc_display_contents_1_t *content = displays[HWC_DISPLAY_PRIMARY];
    int ret = 0;
    hwc_context_t *ctx = to_ctx(dev);
    size_t i = 0;

    if (content) {
	ret = update_display(ctx, HWC_DISPLAY_PRIMARY, content);
	if (ret)
		return ret;
    }

    content = displays[HWC_DISPLAY_EXTERNAL];
    if (content)
	return update_display(ctx, HWC_DISPLAY_EXTERNAL, content);
    return ret;
}

static int find_plane(hwc_context_t *ctx, int disp, private_handle_t *hnd)
{
   unsigned int i, j, ret = 0;
   drmModePlaneResPtr plane_res;
   int drm_fd = ctx->drm_fd;
   unsigned int fourcc = hnd_to_fourcc(hnd);

   if (!fourcc)
	return ret;

   plane_res = drmModeGetPlaneResources(drm_fd);

   for (i = 0; i < plane_res->count_planes && !ret; i++) {
	drmModePlanePtr plane;

	plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);

	if (!plane)
		continue;

	if (!(plane->possible_crtcs & (1 << disp))) {
		drmModeFreePlane(plane);
		continue;
	}

	if (ctx->used_planes & (1 << i)) {
		drmModeFreePlane(plane);
		continue;
	}

	for (j =0; j < plane->count_formats && !ret; j++) {
	    if (plane->formats[j] == fourcc) {
		ret = plane->plane_id;
		hnd->plane_id = plane->plane_id;
		ctx->used_planes |= 1 << i;
	    }
	}

	drmModeFreePlane(plane);
   }

   drmModeFreePlaneResources (plane_res);

   return ret;
}

static int prepare_display (hwc_context_t *ctx, int disp,
        hwc_display_contents_1_t *content)
{
    kms_display_t *d = &ctx->displays[disp];
    bool target_framebuffer = false;

    if (!is_display_connected(ctx, disp))
	return 0;

    for (int i = content->numHwLayers - 1;  i >= 0; i--) {
        hwc_layer_1_t &layer = content->hwLayers[i];
	private_handle_t *hnd = (private_handle_t *)layer.handle;
	int plane_id;

        if (layer.compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;

	if (target_framebuffer) {
	   layer.compositionType = HWC_FRAMEBUFFER;
	   continue;
	}

	plane_id = find_plane(ctx, disp, hnd);
	if (plane_id) {
	   layer.compositionType = HWC_OVERLAY;
	   continue;
	}

        layer.compositionType = HWC_FRAMEBUFFER;
	target_framebuffer = true;
    }

    return 0;
}

static int hwc_prepare (struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (! numDisplays || ! displays)
        return 0;

    hwc_display_contents_1_t *content = displays[HWC_DISPLAY_PRIMARY];
    hwc_context_t *ctx = to_ctx(dev);
    int ret = 0;

    ctx->used_planes = 0;

    if (content) {
	ret = prepare_display (ctx, HWC_DISPLAY_PRIMARY, content);
	if (ret)
		return ret;
    }

    content = displays[HWC_DISPLAY_EXTERNAL];
    if (content)
	ret = prepare_display (ctx, HWC_DISPLAY_EXTERNAL, content);

    return ret;
}

static int hwc_eventControl (struct hwc_composer_device_1* dev, int disp,
        int event, int enabled)
{
    hwc_context_t *ctx = to_ctx(dev);

    if (disp < 0 || disp >= HWC_NUM_DISPLAY_TYPES)
        return -EINVAL;

    switch (event) {
        case HWC_EVENT_VSYNC:
            ctx->displays[disp].vsync_on = enabled;
            if (enabled)
                send_vsync_request(ctx, disp);
            return 0;
        default:
           return -EINVAL;
    }
}

static int hwc_query (struct hwc_composer_device_1* dev, int what, int *value)
{
    hwc_context_t *ctx = to_ctx(dev);
    int refreshRate = 60;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        value[0] = 1;   //support the background layer
        break;
    case HWC_VSYNC_PERIOD:
        value[0] = 1000000000 / refreshRate;
        break;
   case HWC_DISPLAY_TYPES_SUPPORTED:
	if (is_display_connected(ctx, HWC_DISPLAY_PRIMARY))
	    value[0] = HWC_DISPLAY_PRIMARY_BIT;
        if (is_display_connected(ctx, HWC_DISPLAY_EXTERNAL))
            value[0] |= HWC_DISPLAY_EXTERNAL_BIT;
        break;
    default:
        return -EINVAL; //unsupported query
    }
    return 0;
}

static void hwc_registerProcs (struct hwc_composer_device_1* dev,
        hwc_procs_t const* procs)
{
    hwc_context_t *ctx = to_ctx(dev);

    ctx->cb_procs = procs;
}

static int hwc_getDisplayConfigs (struct hwc_composer_device_1* dev,
        int disp, uint32_t *configs, size_t *numConfigs)
{
    hwc_context_t *ctx = to_ctx(dev);

    if (*numConfigs == 0)
        return 0;

    if (is_display_connected(ctx, disp)) {
        configs[0] = HWC_DEFAULT_CONFIG;
        *numConfigs = 1;
        return 0;
    }
    return -EINVAL;
}

static int hwc_getDisplayAttributes (struct hwc_composer_device_1* dev, int disp,
        uint32_t config, const uint32_t* attributes, int32_t* values)
{
    hwc_context_t *ctx = to_ctx(dev);
    kms_display_t *d = &ctx->displays[disp];

    if (!is_display_connected(ctx, disp))
	return -EINVAL;

    if (config != HWC_DEFAULT_CONFIG)
        return -EINVAL;

    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE;  i++) {
            switch (attributes[i]) {
            case HWC_DISPLAY_VSYNC_PERIOD:
                values[i] = 1000000000 / 60;
                break;
            case HWC_DISPLAY_WIDTH:
                values[i] = d->mode->hdisplay;
                break;
            case HWC_DISPLAY_HEIGHT:
                values[i] = d->mode->vdisplay;
                break;
            case HWC_DISPLAY_DPI_X:
		values[i] = 0;
		if (d->con->mmWidth)
			values[i] = (d->mode->hdisplay * 25400) / d->con->mmWidth;
		break;
            case HWC_DISPLAY_DPI_Y:
		values[i] = 0;
		if (d->con->mmHeight)
			values[i] = (d->mode->vdisplay * 25400) / d->con->mmHeight;
                break;
            default:
                ALOGE("unknown display attribute %u\n", *attributes);
                values[i] = 0;
                return -EINVAL;
        }
    }
    return 0;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int disp, int blank)
{
    return 0;
}

static void hwc_dump(struct hwc_composer_device_1* dev, char *buff,
                                int buff_len)
{
        ALOGD("%s", __func__);
}

static int hwc_device_close(struct hw_device_t *dev)
{
    hwc_context_t *ctx = to_ctx(dev);

    if (!ctx)
        return 0;

    destroy_display(ctx->drm_fd, &ctx->displays[HWC_DISPLAY_PRIMARY]);
    destroy_display(ctx->drm_fd, &ctx->displays[HWC_DISPLAY_EXTERNAL]);

    drmClose(ctx->drm_fd);
    free(ctx);

    return 0;
}

static void init_gralloc(int drm_fd)
{
    hw_module_t *pmodule = NULL;
    private_module_t *m = NULL;
    hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&pmodule);
    m = reinterpret_cast<private_module_t *>(pmodule);
    m->drm_fd = drm_fd;
}

static int hwc_device_open(const struct hw_module_t *module, const char *name,
        struct hw_device_t **device)
{
    hwc_context_t *ctx;
    drmModeResPtr resources;
    drmModePlaneResPtr planes;
    int err = 0;
    int ret = 0;
    int drm_fd = 0;

    if (strcmp(name, HWC_HARDWARE_COMPOSER))
	return -EINVAL;
    ctx = (hwc_context_t *) calloc(1, sizeof(*ctx));

    /* Initialize the procs */
    ctx->device.common.tag		= HARDWARE_DEVICE_TAG;
    ctx->device.common.version		= HWC_DEVICE_API_VERSION_1_1;
    ctx->device.common.module		= (struct hw_module_t *)module;
    ctx->device.common.close		= hwc_device_close;

    ctx->device.prepare			= hwc_prepare;
    ctx->device.set			= hwc_set;
    ctx->device.eventControl		= hwc_eventControl;
    ctx->device.blank			= hwc_blank;
    ctx->device.query			= hwc_query;
    ctx->device.registerProcs		= hwc_registerProcs;
    ctx->device.dump			= hwc_dump;
    ctx->device.getDisplayConfigs	= hwc_getDisplayConfigs;
    ctx->device.getDisplayAttributes	= hwc_getDisplayAttributes;

    ctx->drm_fd = -1;

    /* Open Gralloc module */
    ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
			(const struct hw_module_t **)&ctx->gralloc);
    if (ret) {
        ALOGE("Failed to get gralloc module: %s\n",strerror(errno));
        return ret;
    }

    ret = init_display(ctx, HWC_DISPLAY_PRIMARY);
    if (ret) {
	if (ctx->drm_fd != -1)
	  drmClose(ctx->drm_fd);
        return -EINVAL;
    }

    init_display(ctx, HWC_DISPLAY_EXTERNAL);

    ctx->used_planes = 0;

    init_gralloc(ctx->drm_fd);

    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_JOINABLE);
    ret = pthread_create(&ctx->event_thread, &attrs, event_handler, ctx);
    if (ret) {
        ALOGE("Failed to create event thread:%s\n",strerror(ret));
        ret = -ret;
        return ret;
    }

    *device = &ctx->device.common;

    return 0;
}

static struct hw_module_methods_t hwc_module_methods = {
	open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
	common: {
		tag: HARDWARE_MODULE_TAG,
		module_api_version: HWC_MODULE_API_VERSION_0_1,
		hal_api_version: HARDWARE_HAL_API_VERSION,
		id: HWC_HARDWARE_MODULE_ID,
        name: "DRM/KMS hwcomposer module",
        author: "Benjamin Gaignard <benjamin.gaignard@linaro.org>",
        methods: &hwc_module_methods,
		dso: 0,
		reserved: {0},
    }
};
