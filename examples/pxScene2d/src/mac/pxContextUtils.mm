#include "pxContextUtils.h"

#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#import <Cocoa/Cocoa.h>
#include <map>
#include <thread>
#include <mutex>

#include "rtThreadUtils.h"

struct contextData
{
    NSOpenGLContext *glContext = nil;
    NSOpenGLPixelFormat *pixelFormat = nil;

    bool isCurrent = false;
};

extern NSOpenGLContext *openGLContext;

NSOpenGLPixelFormatAttribute attribs[] =
    {
        /*NSOpenGLPFADoubleBuffer,*/
        NSOpenGLPFAAllowOfflineRenderers, // lets OpenGL know this context is offline renderer aware
        NSOpenGLPFAMultisample, 1,
        NSOpenGLPFASampleBuffers, 1,
        NSOpenGLPFASamples, 4,
        NSOpenGLPFAColorSize, 32,
        NSOpenGLPFAOpenGLProfile,NSOpenGLProfileVersionLegacy/*, NSOpenGLProfileVersion3_2Core*/, // Core Profile is the future
        0
    };

NSOpenGLPixelFormat *emptypixelFormat = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attribs] retain];
NSOpenGLContext *emptyGlContext = [[NSOpenGLContext alloc] initWithFormat:emptypixelFormat shareContext:nil];

std::recursive_mutex contextsMutex;

std::map<rtThreadId, contextData> backgroundContexts;


pxError createGLContext()
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



    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];
    data.pixelFormat   = [pf retain];
    data.glContext = [[NSOpenGLContext alloc] initWithFormat:data.pixelFormat shareContext:openGLContext];

    {
        std::unique_lock<std::recursive_mutex> {contextsMutex};
        backgroundContexts[currentThreadId] = data;
    }

    return PX_OK;
}

pxError makeInternalGLContextCurrent(bool current)
{
    rtThreadId currentThreadId = rtThreadGetCurrentId();
    contextData data;
    if (current)
    {
        bool contextExists = false;
        {
            std::unique_lock<std::recursive_mutex> {contextsMutex};
            if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
            {
                contextExists = true;
                data = backgroundContexts[currentThreadId];
            }
        }
        if (!contextExists)
        {
            createGLContext();
            {
                std::unique_lock<std::recursive_mutex> {contextsMutex};
                if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
                {
                    data = backgroundContexts[currentThreadId];
                }
            }
            [data.glContext makeCurrentContext];
            
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        else
        {
            [data.glContext makeCurrentContext];
        }
        data.isCurrent = true;
        {
            std::unique_lock<std::recursive_mutex> {contextsMutex};
            backgroundContexts[currentThreadId] = data;
        }
    }
    else
    {
        [openGLContext makeCurrentContext];
        std::unique_lock<std::recursive_mutex> {contextsMutex};
        if ( backgroundContexts.find(currentThreadId) != backgroundContexts.end())
        {
            data = backgroundContexts[currentThreadId];
            data.isCurrent = false;
            backgroundContexts[currentThreadId] = data;
        }
    }
    return PX_OK;
}

pxError requestContextOwnership()
{
  if (!rtIsMainThread())
  {
    makeInternalGLContextCurrent(true);
  }
  //[openGLContext makeCurrentContext];
  return PX_OK;
}

pxError releaseContextOwnership()
{
  if (!rtIsMainThread())
  {
    makeInternalGLContextCurrent(false);
  }
  return PX_OK;
}
