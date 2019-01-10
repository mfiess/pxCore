/*

pxCore Copyright 2005-2018 John Robinson

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include "pxContextUtils.h"
#include "rtLog.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <map>
#include "rtThreadUtils.h"
#include <thread>
#include <mutex>

struct contextData
{
  EGLDisplay eglDisplay = 0;
  EGLSurface eglSurface = 0;
  EGLContext eglContext = 0;

  EGLDisplay prevEglDisplay = 0;
  EGLSurface prevEglDrawSurface = 0;
  EGLSurface prevEglReadSurface = 0;
  EGLContext prevEglContext = 0;
  
  bool isCurrent = false;
};

EGLContext defaultEglContext = 0;
EGLDisplay defaultEglDisplay = 0;
EGLSurface defaultEglDrawSurface = 0;
EGLSurface defaultEglReadSurface = 0;

std::recursive_mutex contextsMutex;

std::map<rtThreadId, contextData> backgroundContexts;

int pxCreateEglContext()
{
  rtThreadId currentThreadId = rtThreadGetCurrentId();
  {
    std::unique_lock<std::recursive_mutex> {contextsMutex};
    if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
    {
      return PX_FAIL;
    }
  }
  contextData data;
  rtLogInfo("creating new context\n");
  EGLDisplay egl_display      = 0;
  EGLSurface egl_surface      = 0;
  EGLContext egl_context      = 0;
  EGLConfig *egl_config;
  EGLint     major_version;
  EGLint     minor_version;
  int        config_select    = 0;
  int        configs;

  egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_display == EGL_NO_DISPLAY)
  {
    rtLogError("eglGetDisplay() failed, did you register any exclusive displays\n");
    return PX_FAIL;
  }

  if (!eglInitialize(egl_display, &major_version, &minor_version))
  {
     rtLogError("eglInitialize() failed\n");
     return PX_FAIL;
  }

  if (!eglGetConfigs(egl_display, NULL, 0, &configs))
  {
     rtLogError("eglGetConfigs() failed\n");
     return PX_FAIL;
  }

  egl_config = (EGLConfig *)alloca(configs * sizeof(EGLConfig));

  {
    const int   NUM_ATTRIBS = 21;
    EGLint      *attr = (EGLint *)malloc(NUM_ATTRIBS * sizeof(EGLint));
    int         i = 0;

    attr[i++] = EGL_RED_SIZE;        attr[i++] = 8;
    attr[i++] = EGL_GREEN_SIZE;      attr[i++] = 8;
    attr[i++] = EGL_BLUE_SIZE;       attr[i++] = 8;
    attr[i++] = EGL_ALPHA_SIZE;      attr[i++] = 8;
    attr[i++] = EGL_DEPTH_SIZE;      attr[i++] = 24;
    attr[i++] = EGL_STENCIL_SIZE;    attr[i++] = 0;
    attr[i++] = EGL_SURFACE_TYPE;    attr[i++] = EGL_PBUFFER_BIT;
    attr[i++] = EGL_RENDERABLE_TYPE; attr[i++] = EGL_OPENGL_ES2_BIT;

    attr[i++] = EGL_NONE;

    if (!eglChooseConfig(egl_display, attr, egl_config, configs, &configs) || (configs == 0))
    {
      rtLogError("eglChooseConfig() failed");
      return PX_FAIL;
    }

    free(attr);
  }

  for (config_select = 0; config_select < configs; config_select++)
  {
    EGLint red_size, green_size, blue_size, alpha_size, depth_size;

    eglGetConfigAttrib(egl_display, egl_config[config_select], EGL_RED_SIZE,   &red_size);
    eglGetConfigAttrib(egl_display, egl_config[config_select], EGL_GREEN_SIZE, &green_size);
    eglGetConfigAttrib(egl_display, egl_config[config_select], EGL_BLUE_SIZE,  &blue_size);
    eglGetConfigAttrib(egl_display, egl_config[config_select], EGL_ALPHA_SIZE, &alpha_size);
    eglGetConfigAttrib(egl_display, egl_config[config_select], EGL_DEPTH_SIZE, &depth_size);

    if ((red_size == 8) && (green_size == 8) && (blue_size == 8) && (alpha_size == 8))
    {
      rtLogInfo("Selected config: R=%d G=%d B=%d A=%d Depth=%d\n", red_size, green_size, blue_size, alpha_size, depth_size);
      break;
    }
  }

  if (config_select == configs)
  {
    rtLogError("No suitable configs found\n");
    return PX_FAIL;
  }

  EGLint attribList[] =
  {
    EGL_WIDTH, 1280,
    EGL_HEIGHT, 720,
    EGL_LARGEST_PBUFFER, EGL_TRUE,
    EGL_NONE
  };
  egl_surface = eglCreatePbufferSurface(egl_display, egl_config[config_select], attribList);
  if (egl_surface == EGL_NO_SURFACE)
  {
    eglGetError(); /* Clear error */
    egl_surface = eglCreateWindowSurface(egl_display, egl_config[config_select], (EGLNativeWindowType)NULL, NULL);
  }

  if (egl_surface == EGL_NO_SURFACE)
  {
    rtLogError("eglCreateWindowSurface() failed\n");
    return PX_FAIL;
  }

  {
    EGLint     ctx_attrib_list[3] =
    {
      EGL_CONTEXT_CLIENT_VERSION, 2, /* For ES2 */
      EGL_NONE
    };

    egl_context = eglCreateContext(egl_display, egl_config[config_select], defaultEglContext /*EGL_NO_CONTEXT*/, ctx_attrib_list);
    if (egl_context == EGL_NO_CONTEXT)
    {
      rtLogError("eglCreateContext() failed");
      return PX_FAIL;
    }
  }

  data.eglDisplay = egl_display;
  data.eglSurface = egl_surface;
  data.eglContext = egl_context;
  rtLogInfo("display: %p surface: %p context: %p created\n", data.eglDisplay, data.eglSurface, data.eglContext);
  {
    std::unique_lock<std::recursive_mutex> {contextsMutex};
    backgroundContexts[currentThreadId] = data;
  }

  return PX_OK;
}
  
void pxMakeEglCurrent(contextData data)
{
  if (!data.isCurrent)
  {
    data.prevEglDisplay = eglGetCurrentDisplay();
    data.prevEglDrawSurface = eglGetCurrentSurface(EGL_DRAW);
    data.prevEglReadSurface = eglGetCurrentSurface(EGL_READ);
    data.prevEglContext = eglGetCurrentContext();
    bool success = eglMakeCurrent(data.eglDisplay, data.eglSurface, data.eglSurface, data.eglContext);
    if (!success)
    {
      int eglError = eglGetError();
      rtLogWarn("make current error: %d\n", eglError);
      return;
    }

    data.isCurrent = true;
    {
      std::unique_lock<std::recursive_mutex> {contextsMutex};
      backgroundContexts[rtThreadGetCurrentId()] = data;
    }
  }
}

void pxDoneEglCurrent()
{
  contextData data;
  {
    std::unique_lock<std::recursive_mutex> {contextsMutex};
    data = backgroundContexts[rtThreadGetCurrentId()];
  }
  if (data.isCurrent)
  {
    eglMakeCurrent(data.prevEglDisplay, data.prevEglDrawSurface, data.prevEglReadSurface, data.prevEglContext);
    data.isCurrent = false;
    {
      std::unique_lock<std::recursive_mutex> {contextsMutex};
      backgroundContexts[rtThreadGetCurrentId()] = data;
    }
  }
}

void pxDeleteEglContext()
{
  pxDoneEglCurrent();
  rtThreadId currentThreadId = rtThreadGetCurrentId();
  contextData data;
  {
    std::unique_lock<std::recursive_mutex> {contextsMutex};
    if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
    {
      return;
    }
    else
    {
      data = backgroundContexts[currentThreadId];
      backgroundContexts.erase(currentThreadId);
    }
  }
  rtLogInfo("deleting pxscene context\n");
  eglDestroySurface(data.eglDisplay, data.eglSurface);
  eglDestroyContext(data.eglDisplay, data.eglContext);
  data.eglDisplay = 0;
  data.eglSurface = 0;
  data.eglContext = 0;
  data.prevEglDisplay = 0;
  data.prevEglDrawSurface = 0;
  data.prevEglReadSurface = 0;
  data.prevEglContext = 0;
}

pxError makeInternalGLContextCurrent(bool current)
{
  if (current)
  {
    rtThreadId currentThreadId = rtThreadGetCurrentId();
    bool contextExists = false;
    contextData data;
    {
      std::unique_lock<std::recursive_mutex> {contextsMutex};
      if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
      {
        contextExists = true;
        data = backgroundContexts[rtThreadGetCurrentId()];
      }
    }
    if (!contextExists)
    {
      pxCreateEglContext();
      {
        std::unique_lock<std::recursive_mutex> {contextsMutex};
        if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
        {
          data = backgroundContexts[rtThreadGetCurrentId()];
        }
      }
      pxMakeEglCurrent(data);

      glEnable(GL_BLEND);
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    else
    {
      pxMakeEglCurrent(data);
    }
  }
  else
  {
    pxDoneEglCurrent();
  }
  return PX_OK;
}

pxError requestContextOwnership()
{
  bool success = false;
  {
    std::unique_lock<std::recursive_mutex> {contextsMutex};
    success = eglMakeCurrent(defaultEglDisplay, defaultEglDrawSurface, defaultEglReadSurface, defaultEglContext);
  }
  if (!success)
  {
    int eglError = eglGetError();
    rtLogWarn("request context ownership failed: %d\n", eglError);
    return PX_FAIL;
  }
  return PX_OK;
}

pxError releaseContextOwnership()
{
  bool success = false;
  {
    std::unique_lock<std::recursive_mutex> {contextsMutex};
    success = eglMakeCurrent(defaultEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (!success)
  {
    int eglError = eglGetError();
    rtLogWarn("release context ownership failed: %d\n", eglError);
    return PX_FAIL;
  }
  return PX_OK;
}
