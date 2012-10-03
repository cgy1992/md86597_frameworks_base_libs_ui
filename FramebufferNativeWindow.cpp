/* 
**
** Copyright 2007 The Android Open Source Project
**
** Licensed under the Apache License Version 2.0(the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing software 
** distributed under the License is distributed on an "AS IS" BASIS 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#define LOG_TAG "FramebufferNativeWindow"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <utils/threads.h>
#include <utils/RefBase.h>

#include <ui/Rect.h>
#include <ui/FramebufferNativeWindow.h>

#include <EGL/egl.h>

#include <pixelflinger/format.h>
#include <pixelflinger/pixelflinger.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <private/ui/android_natives_priv.h>

#include <sys/ioctl.h>
#include <fcntl.h>

// ----------------------------------------------------------------------------
namespace android {
// ----------------------------------------------------------------------------

class NativeBuffer 
    : public EGLNativeBase<
        android_native_buffer_t, 
        NativeBuffer, 
        LightRefBase<NativeBuffer> >
{
public:
    NativeBuffer(int w, int h, int f, int u) : BASE() {
        android_native_buffer_t::width  = w;
        android_native_buffer_t::height = h;
        android_native_buffer_t::format = f;
        android_native_buffer_t::usage  = u;
    }
private:
    friend class LightRefBase<NativeBuffer>;    
    ~NativeBuffer() { }; // this class cannot be overloaded
};


/*
 * This implements the (main) framebuffer management. This class is used
 * mostly by SurfaceFlinger, but also by command line GL application.
 * 
 * In fact this is an implementation of android_native_window_t on top of
 * the framebuffer.
 * 
 * Currently it is pretty simple, it manages only two buffers (the front and 
 * back buffer).
 * 
 */

FramebufferNativeWindow::FramebufferNativeWindow() 
    : BASE(), fbDev(0), grDev(0), mUpdateOnDemand(false)
{
    hw_module_t const* module;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
        int stride;
        int err;
        err = framebuffer_open(module, &fbDev);
        LOGE_IF(err, "couldn't open framebuffer HAL (%s)", strerror(-err));
        
        err = gralloc_open(module, &grDev);
        LOGE_IF(err, "couldn't open gralloc HAL (%s)", strerror(-err));

        // bail out if we can't initialize the modules
        if (!fbDev || !grDev)
            return;
        
        mUpdateOnDemand = (fbDev->setUpdateRect != 0);
        
        // initialize the buffer FIFO
        mNumBuffers = 2;
        mNumFreeBuffers = 2;
        mBufferHead = mNumBuffers-1;
        buffers[0] = new NativeBuffer(
                fbDev->width, fbDev->height, fbDev->format, GRALLOC_USAGE_HW_FB);
        buffers[1] = new NativeBuffer(
                fbDev->width, fbDev->height, fbDev->format, GRALLOC_USAGE_HW_FB);
        
        err = grDev->alloc(grDev,
                fbDev->width, fbDev->height, fbDev->format, 
                GRALLOC_USAGE_HW_FB, &buffers[0]->handle, &buffers[0]->stride);

        LOGE_IF(err, "fb buffer 0 allocation failed w=%d, h=%d, err=%s",
                fbDev->width, fbDev->height, strerror(-err));

        err = grDev->alloc(grDev,
                fbDev->width, fbDev->height, fbDev->format, 
                GRALLOC_USAGE_HW_FB, &buffers[1]->handle, &buffers[1]->stride);

        LOGE_IF(err, "fb buffer 1 allocation failed w=%d, h=%d, err=%s",
                fbDev->width, fbDev->height, strerror(-err));

        const_cast<uint32_t&>(android_native_window_t::flags) = fbDev->flags; 
        const_cast<float&>(android_native_window_t::xdpi) = fbDev->xdpi;
        const_cast<float&>(android_native_window_t::ydpi) = fbDev->ydpi;
        const_cast<int&>(android_native_window_t::minSwapInterval) = 
            fbDev->minSwapInterval;
        const_cast<int&>(android_native_window_t::maxSwapInterval) = 
            fbDev->maxSwapInterval;
    } else {
        LOGE("Couldn't get gralloc module");
    }

    android_native_window_t::setSwapInterval = setSwapInterval;
    android_native_window_t::dequeueBuffer = dequeueBuffer;
    android_native_window_t::lockBuffer = lockBuffer;
    android_native_window_t::queueBuffer = queueBuffer;
    android_native_window_t::query = query;
    android_native_window_t::perform = perform;
    //[BEGIN] add by ethaen
     android_native_window_t::fd = getDeviceFD();
    //END

    //[END]

// [BEGIN] skyviia modify: fix lock display issue if framework reboot without unlock display.
// fix me!
#ifdef SV886X
//        if (ioctl(android_native_window_t::fd, SKYFB_UNLOCK_DISPLAY) == -1) {
//            LOGE("unlock display failed !\n");
//        } else {
//            LOGI("unlock display OK !\n");
//        }
#endif
// [END]

}

FramebufferNativeWindow::~FramebufferNativeWindow() 
{
    if (grDev) {
        if (buffers[0] != NULL)
            grDev->free(grDev, buffers[0]->handle);
        if (buffers[1] != NULL)
            grDev->free(grDev, buffers[1]->handle);
        gralloc_close(grDev);
    }

    if (fbDev) {
        framebuffer_close(fbDev);
    }
}

status_t FramebufferNativeWindow::setUpdateRectangle(const Rect& r) 
{
    if (!mUpdateOnDemand) {
        return INVALID_OPERATION;
    }
    return fbDev->setUpdateRect(fbDev, r.left, r.top, r.width(), r.height());
}

status_t FramebufferNativeWindow::compositionComplete()
{
    if (fbDev->compositionComplete) {
        return fbDev->compositionComplete(fbDev);
    }
    return INVALID_OPERATION;
}

int FramebufferNativeWindow::setSwapInterval(
        android_native_window_t* window, int interval) 
{
    framebuffer_device_t* fb = getSelf(window)->fbDev;
    return fb->setSwapInterval(fb, interval);
}

int FramebufferNativeWindow::dequeueBuffer(android_native_window_t* window, 
        android_native_buffer_t** buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    framebuffer_device_t* fb = self->fbDev;

    // wait for a free buffer
    while (!self->mNumFreeBuffers) {
        self->mCondition.wait(self->mutex);
    }
    // get this buffer
    self->mNumFreeBuffers--;
    int index = self->mBufferHead++;
    if (self->mBufferHead >= self->mNumBuffers)
        self->mBufferHead = 0;

    *buffer = self->buffers[index].get();

    return 0;
}

int FramebufferNativeWindow::lockBuffer(android_native_window_t* window, 
        android_native_buffer_t* buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);

    // wait that the buffer we're locking is not front anymore
    while (self->front == buffer) {
        self->mCondition.wait(self->mutex);
    }

    return NO_ERROR;
}

int FramebufferNativeWindow::queueBuffer(android_native_window_t* window, 
        android_native_buffer_t* buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    framebuffer_device_t* fb = self->fbDev;
    buffer_handle_t handle = static_cast<NativeBuffer*>(buffer)->handle;
    int res = fb->post(fb, handle);
    self->front = static_cast<NativeBuffer*>(buffer);
    self->mNumFreeBuffers++;
    self->mCondition.broadcast();
    return res;
}

int FramebufferNativeWindow::query(android_native_window_t* window,
        int what, int* value) 
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    framebuffer_device_t* fb = self->fbDev;
    switch (what) {
        case NATIVE_WINDOW_WIDTH:
            *value = fb->width;
            return NO_ERROR;
        case NATIVE_WINDOW_HEIGHT:
            *value = fb->height;
            return NO_ERROR;
        case NATIVE_WINDOW_FORMAT:
            *value = fb->format;
            return NO_ERROR;
    }
    *value = 0;
    return BAD_VALUE;
}

int FramebufferNativeWindow::perform(android_native_window_t* window,
        int operation, ...)
{
    switch (operation) {
        case NATIVE_WINDOW_SET_USAGE:
        case NATIVE_WINDOW_CONNECT:
        case NATIVE_WINDOW_DISCONNECT:
            break;
        default:
            return NAME_NOT_FOUND;
    }
    return NO_ERROR;
}

// [BEGIN] skyviia modify:
#ifdef SV886X
void FramebufferNativeWindow::mapDisp2Params(void) {
  /*  remove by ethan 
     if (android_native_window_t::fd >= 0
        && mFbAddr[0] != 0 && mFbAddr[1] != 0) {
        struct skyfb_api_display_parm param;
	    param.display = SKYFB_DISP2;
	    param.input_format = INPUT_FORMAT_ARGB;
	    param.start_x = 0; //(sky_get_graphic_device_info.width - display2_width) / 2; //Original Point X position
	    param.start_y = 0; //(sky_get_graphic_device_info.height - display2_height) / 2; //Original Point Y position
	    param.width_in = mFb[mIndex].width; //display2_width;
	    param.height_in = mFb[mIndex].height; //display2_height;
	    param.stride = mFb[mIndex].width; //display2_width;
	    param.alpha = 0x00; //Control by image alpha //0xff;
	    param.y_addr = mFbAddr[mIndex]; //sky_get_graphic_device_info.fb_base_addr + sky_get_graphic_device_info.y_offset;
	    param.u_addr = 0; //sky_get_graphic_device_info.fb_base_addr + sky_get_graphic_device_info.y_offset + 320	* 480;
	    param.v_addr = 0;
	    if (ioctl(android_native_window_t::fd, SKYFB_SET_DISPLAY_PARM, &param) == -1) {
		    LOGD("mapDisp2Params() -> SKYFB_SET_DISPLAY_PARM failed\n");
	    }
    }*/
}

void FramebufferNativeWindow::turnDisp2On(bool bTurnOn) {
    // Turn off display2 for now.
    /*if (android_native_window_t::fd >= 0) {
        struct skyfb_api_display_status d2status;
        d2status.display = SKYFB_DISP2;
        d2status.status = bTurnOn ? SKYFB_ON : SKYFB_OFF;
        if (ioctl(android_native_window_t::fd,SKYFB_SET_DISPLAY_STATUS, &d2status) == -1) {
	        LOGD("turnDisp2On() -> SKYFB_SET_DISPLAY_STATUS failed\n");
        }
    }
    */
}

void FramebufferNativeWindow::setDisp2Addr(void) {
    /*
    if (android_native_window_t::fd >= 0
        && mFbAddr[0] != 0 && mFbAddr[1] != 0) {
        struct skyfb_api_display_addr param;
	    param.display = SKYFB_DISP2; //display1 or display2
	    param.y_addr = mFbAddr[mIndex]; //YCC420, ARGB
	    param.u_addr = 0; //YCC420
	    param.v_addr = 0; //none use in ARGB and YCC420 mode
	    if (ioctl(android_native_window_t::fd, SKYFB_SET_DISPLAY_ADDR, &param) == -1) {
		    LOGD("setDisp2Addr() -> SKYFB_SET_DISPLAY_ADDR failed\n");
	    }
    }
    */
}

void FramebufferNativeWindow::preResetAlpha(void) {
/*
	if (android_native_window_t::fd >= 0) {
        struct skyfb_api_bitblt bb;
	    bb.src_addr = mFbAddr[mIndex]; // + (r.left + android_native_buffer_t::stride * r.top) * bpp;
	    bb.dst_addr = mFbAddr[1-mIndex]; // + (r.left + android_native_buffer_t::stride * r.top) * bpp;
	    bb.width = mFb[1-mIndex].width;
	    bb.height = mFb[1-mIndex].height;
	    bb.src_stride = android_native_buffer_t::stride;
	    bb.dst_stride = android_native_buffer_t::stride;
	    bb.direction = 0;//BB_DIR_START_UL;
	    bb.alpha_value_from = 0;//AV_FROM_REG;
	    bb.alpha_value = 0x00;
	    bb.alpha_blend_status = 1;//SKYFB_ON;
        bb.alpha_blend_from = 0;
        bb.alpha_blend_value = 0x00;
	    if (ioctl(android_native_window_t::fd, SKYFB_2D_BITBLT, &bb) == -1) {
		    LOGE("preResetAlpha() -> SKYFB_2D_BITBLT error!");
	    }
    }*/
}
#endif

// [BEGIN] set display mode
int32_t FramebufferNativeWindow::setDisplayMode(int mode) {
	
#ifdef SV886X
	int fd = android_native_window_t::fd;
	if (fd < 0) {
		return -errno;
	}

	if (ioctl(fd, SKYFB_SET_MODE_ONLY, &mode) != 0) {
		LOGE("Set Display Mode failed!");
		return -errno;
	}
        // TODO: must notice this issue!
	//regetSurfaceInfo();
#else
	// running in the emulator!
	LOGD("set mode in emulator is forbidden!");
#endif
	return NO_ERROR;
}

// [BEGIN] set display mode
uint32_t FramebufferNativeWindow::getDisplayMode() {
	
#ifdef SV886X
	int fd = android_native_window_t::fd;
	if (fd < 0) {
		return -errno;
	}
	
    uint32_t mode;
	if (ioctl(fd, SKYFB_GET_MODE, &mode) != 0) {
		LOGE("Get Display Mode failed!");
		return -errno;
	}
	// return uint32_t = (mode << 16) | format
	return mode;
#else
	// running in the emulator!
	LOGD("get mode in emulator is forbidden!");
#endif
	return NO_ERROR;
}

// [BEGIN] Support hardware cursor
int32_t FramebufferNativeWindow::setCursorBmp(uint8_t *bmpCursor, int size) {
	
#ifdef SV886X
	int fd = android_native_window_t::fd;
	if (fd < 0) {
		return -errno;
	}

	if (ioctl(fd, SKYFB_CURSOR_SET_BITMAP, bmpCursor) == -1) {
		LOGE("Set cursor bitmap failed!");
		return -errno;
	}
#endif
	return NO_ERROR;
}

int32_t FramebufferNativeWindow::setCursorPos(int enable, int x, int y, int alpha) {
	
#ifdef SV886X
	int fd = android_native_window_t::fd;
	if (fd < 0) {
		return -errno;
	}
	skyfb_api_cursor_parm params;
	params.status = enable;
	params.xpos = x;
	params.ypos = y;
	params.alpha = alpha;
	if (ioctl(fd, SKYFB_CURSOR_SET_PARM, &params) == -1) {
		LOGE("Not support set cursor parameter.");
		return -errno;
	}
#endif
	return NO_ERROR;
}

//add by ethan
int32_t FramebufferNativeWindow::getDeviceFD() {
	
#ifdef SV886X
	 char const * const device_template[] = {
            "/dev/graphics/fb%u",
            "/dev/fb%u",
            0 };
    int fd = -1;
    int i=0;
    char name[64];
    while ((fd==-1) && device_template[i]) {
        snprintf(name, 64, device_template[i], 0);
        fd = open(name, O_RDWR, 0);        
    }
    if (fd < 0)
        return -errno;
    else
	 return fd;
#endif
	return NO_ERROR;
}

// [END]

// ----------------------------------------------------------------------------
}; // namespace android
// ----------------------------------------------------------------------------

using namespace android;

EGLNativeWindowType android_createDisplaySurface(void)
{
    FramebufferNativeWindow* w;
    w = new FramebufferNativeWindow();
    if (w->getDevice() == NULL) {
        // get a ref so it can be destroyed when we exit this block
        sp<FramebufferNativeWindow> ref(w);
        return NULL;
    }
    return (EGLNativeWindowType)w;
}
