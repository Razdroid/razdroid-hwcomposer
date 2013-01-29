/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <hardware/hardware.h>

#include <fcntl.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hwcomposer.h>

#include <EGL/egl.h>

#include "bcm_host.h"

#define HWC_DBG 1

#ifndef ALIGN_UP(x,y)
#define ALIGN_UP(x,y)  ((x + (y)-1) & ~((y)-1))
#endif
/*****************************************************************************/

struct hwc_context_t {
    hwc_composer_device_t device;
	DISPMANX_DISPLAY_HANDLE_T disp;
};

struct hwc_layer_rd {
	hwc_layer_t *layer;
	uint32_t format;
};

typedef struct
{
    DISPMANX_DISPLAY_HANDLE_T   display;
    DISPMANX_MODEINFO_T         info;
    void                       *image;
    DISPMANX_UPDATE_HANDLE_T    update;
    DISPMANX_RESOURCE_HANDLE_T  resource;
    DISPMANX_ELEMENT_HANDLE_T   element;
    uint32_t                    vc_image_ptr;

} RECT_VARS_T;

static RECT_VARS_T  gRectVars;


static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "RazDroid HWComposer",
        author: "Viktor 'Warg' Warg",
        methods: &hwc_module_methods,
    }
};

/*****************************************************************************/
static void hwc_get_rd_layer(hwc_layer_t *src, struct hwc_layer_rd *dst){
	dst->layer = src;
	dst->format = HAL_PIXEL_FORMAT_RGB_565; //XXX: FIX THIS HACK
}

static void dump_layer(hwc_layer_t const* l) {
    LOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
            l->compositionType, l->flags, l->handle, l->transform, l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);
}

static bool hwc_can_render_layer(struct hwc_layer_rd *layer)
{
    bool ret = false;
	switch(layer->format){
	case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_RGBA_8888:
		ret=true;
		break;
	default:
		ret=false;
		break;
	}
	return ret;
}

static VC_IMAGE_TYPE_T hwc_format_to_vc_format(struct hwc_layer_rd *layer){
    VC_IMAGE_TYPE_T ret = VC_IMAGE_RGB565;
	switch(layer->format){
	case HAL_PIXEL_FORMAT_RGB_565:
		ret=VC_IMAGE_RGB565;
		break;
    case HAL_PIXEL_FORMAT_RGBX_8888:
		ret=VC_IMAGE_RGBX8888;
		break;
    case HAL_PIXEL_FORMAT_RGBA_8888:
		ret=VC_IMAGE_RGBA32;
		break;
	default:
		break;
	}
	return ret;
}

static void hwc_actually_do_stuff_with_layer(hwc_composer_device_t *dev, hwc_layer_t *layer){
    RECT_VARS_T    *vars;
	vars = &gRectVars;
	VC_RECT_T       dst_rect;
	struct hwc_layer_rd * lr = (struct hwc_layer_rd *)malloc(sizeof(struct hwc_layer_rd));
	hwc_get_rd_layer(layer, lr);
	VC_IMAGE_TYPE_T type = hwc_format_to_vc_format(lr);

	VC_DISPMANX_ALPHA_T alpha = { DISPMANX_FLAGS_ALPHA_FROM_SOURCE, 
                             120, /*alpha 0->255*/
                             0 };

	
	//layer->handle is the buffer
	//layer->sourceCrop.left/right/top/bottom is which part of the buffer to display
	//layer->displayFrame.left/right/top/bottom is WHERE it should be displayed
	//to solve this, we're going to need to make the buffer into an image, crop it, then apply to the displayFrame.
	
	int dfwidth = layer->displayFrame.left - layer->displayFrame.right;
	int dfheight = layer->displayFrame.top - layer->displayFrame.bottom;
	int srcwidth = layer->sourceCrop.left - layer->sourceCrop.right;
	int srcheight = layer->sourceCrop.top - layer->sourceCrop.bottom;

    int dfpitch = ALIGN_UP(dfwidth*2, 32);
	
	vars->resource = vc_dispmanx_resource_create( type,
                                                  dfwidth,
                                                  dfheight,
                                                  &vars->vc_image_ptr );
	vc_dispmanx_rect_set( &dst_rect, 0, 0, srcwidth, srcheight);
    int ret = vc_dispmanx_resource_write_data(  vars->resource,
												type,
												dfpitch,
												(void*)layer->handle,
												&dst_rect );

	if(ret != 0){
		if(HWC_DBG)	LOGD("vc_dispmanx_resource_write_data failed.");
		return;
	}
	vc_dispmanx_rect_set( &dst_rect, layer->displayFrame.left, layer->displayFrame.top, layer->displayFrame.left+dfwidth, layer->displayFrame.top+dfheight);
												
}

static void hwc_do_stuff_with_layer(hwc_composer_device_t *dev, hwc_layer_t *layer){
	
	if(layer->compositionType == HWC_OVERLAY){
		hwc_actually_do_stuff_with_layer(dev, layer);
	}

}

static int hwc_prepare(hwc_composer_device_t *dev, hwc_layer_list_t* list) {
    if (list && (list->flags & HWC_GEOMETRY_CHANGED)) {
        for (size_t i=0 ; i<list->numHwLayers ; i++) {
            //dump_layer(&list->hwLayers[i]);
			struct hwc_layer_rd * lr = (struct hwc_layer_rd *)malloc(sizeof(struct hwc_layer_rd));
			hwc_get_rd_layer(&list->hwLayers[i], lr);
			if(hwc_can_render_layer(lr)){
				if(HWC_DBG)	LOGD("Layer %d = OVERLAY!", i);
				list->hwLayers[i].compositionType = HWC_OVERLAY;
			}else{
				if(HWC_DBG)	LOGD("Layer %d = NOT OVERLAY!", i);
				list->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
			}
        }
    }
    return 0;
}

static int hwc_set(hwc_composer_device_t *dev,
        hwc_display_t dpy,
        hwc_surface_t sur,
        hwc_layer_list_t* list)
{
    //for (size_t i=0 ; i<list->numHwLayers ; i++) {
    //    dump_layer(&list->hwLayers[i]);
    //}
	
	if(list == NULL){	//NULL list means hwc won't run or we're powering down screen
		if(dpy && sur){	//if we have dpy and sur, hwcomposer has been disabled. swap buffers and leave.
			if(HWC_DBG)	LOGD("list == NULL");
			return eglSwapBuffers((EGLDisplay)dpy, (EGLSurface)sur) ? 0 : HWC_EGL_ERROR;
		}else{			//powering down screen, do nothing
			return 0;
		}
	}
	

    EGLBoolean success = eglSwapBuffers((EGLDisplay)dpy, (EGLSurface)sur);
    if (!success) {
		if(HWC_DBG)	LOGD("eglSwapBuffers errored.");
        return HWC_EGL_ERROR;
    }
	
	
	
	for (size_t i=0 ; i<list->numHwLayers ; i++) {
        hwc_do_stuff_with_layer(dev, &list->hwLayers[i]);
    }
	
		
    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;

        *device = &dev->device.common;
        status = 0;
		
		bcm_host_init();
		
	    dev->disp = vc_dispmanx_display_open( 0 );

    }
    return status;
}
