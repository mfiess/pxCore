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

// rtScript.cpp

#include "rtScript.h"

#include "rtScriptHeaders.h"

#include "rtPathUtils.h"

#include "assert.h"
#include "rtThreadUtils.h"
#include "pxTimer.h"

#if defined RTSCRIPT_SUPPORT_NODE || defined RTSCRIPT_SUPPORT_V8
#include "rtScriptV8/rtScriptV8Node.h"
#endif

#ifdef RTSCRIPT_SUPPORT_DUKTAPE
#include "rtScriptDuk/rtScriptDuk.h"
#endif


#ifdef __APPLE__
#if defined(USE_STD_THREADS)
#include <thread>
#include <mutex>
static std::mutex sSceneLock;
static std::thread::id sCurrentSceneThread;
#else
static pthread_mutex_t sSceneLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER; //PTHREAD_MUTEX_INITIALIZER;
static pthread_t sCurrentSceneThread;
#ifndef RUNINMAIN
static pthread_mutex_t sObjectMapMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER; //PTHREAD_MUTEX_INITIALIZER;
#endif //!RUNINMAIN
#endif //__APPLE__
#elif defined(USE_STD_THREADS)
#include <thread>
#include <mutex>
static std::mutex sSceneLock;
static std::thread::id sCurrentSceneThread;
#else
static pthread_mutex_t sSceneLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_t sCurrentSceneThread;
#ifndef RUNINMAIN
static pthread_mutex_t sObjectMapMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#endif //!RUNINMAIN
#endif

#ifndef ENABLE_DEBUG_MODE
args_t *s_gArgs;
#endif

static int sLockCount;

bool rtWrapperSceneUpdateHasLock()
{
#ifdef USE_STD_THREADS
  return std::this_thread::get_id() == sCurrentSceneThread;
#else
  return pthread_equal(pthread_self(),sCurrentSceneThread);
#endif
}

std::recursive_mutex gSceneMutex;

void rtWrapperSceneUpdateEnter()
{
  gSceneMutex.lock();
}
void rtWrapperSceneUpdateExit()
{
  gSceneMutex.unlock();
}

/*void rtWrapperSceneUpdateEnter()
{
#ifndef RUNINMAIN
  //printf("rtWrapperSceneUpdateEnter() pthread_self= %x\n",pthread_self());
#ifdef USE_STD_THREADS
  std::unique_lock<std::mutex> lock(sSceneLock);
  sCurrentSceneThread = std::this_thread::get_id();
  sLockCount++;
#else
  //assert(pthread_mutex_lock(&sSceneLock) == 0);
  //sCurrentSceneThread = pthread_self();
  // Main thread is now NOT the node thread
  if(rtIsMainThread())
  {
    // Check if this thread already has a lock 
    if(rtWrapperSceneUpdateHasLock()) 
    {
      //printf("increment lock count only\n");
      sLockCount++;
    } 
    else 
    {
      //printf("rtWrapperSceneUpdateEnter locking\n");
      uv_mutex_lock(&threadMutex);
      //printf("rtWrapperSceneUpdateEnter GOT LOCK!!!\n");
      sCurrentSceneThread = pthread_self();
      sLockCount++;
    }
  }
#endif //USE_STD_THREADS

#else // RUNINMAIN
#ifdef USE_STD_THREADS
  std::unique_lock<std::mutex> lock(sSceneLock);
  sCurrentSceneThread = std::this_thread::get_id();
#else
  assert(pthread_mutex_lock(&sSceneLock) == 0);
  sCurrentSceneThread = pthread_self();
#endif
  sLockCount++;
#endif // RUNINMAIN
  
}

void rtWrapperSceneUpdateExit()
{ 
#ifndef RUNINMAIN
  //printf("rtWrapperSceneUpdateExit() pthread_self= %x\n",pthread_self());
  if(rtIsMainThread()) {
 
#ifndef RT_USE_SINGLE_RENDER_THREAD
  assert(rtWrapperSceneUpdateHasLock());
#endif //RT_USE_SINGLE_RENDER_THREAD

  sLockCount--;

#ifdef USE_STD_THREADS
  if (sLockCount == 0)
    sCurrentSceneThread = std::thread::id();
#else
  if (sLockCount == 0)
    sCurrentSceneThread = 0;
#endif

#ifdef USE_STD_THREADS
  std::unique_lock<std::mutex> lock(sSceneLock);
#else
  //assert(pthread_mutex_unlock(&sSceneLock) == 0);
  // Main thread is now NOT the node thread
  if (sLockCount == 0) {
    //printf("rtWrapperSceneUpdateExit unlocking\n");
    uv_mutex_unlock(&threadMutex);
  }

#endif
  }

#else //RUNINMAIN
#ifndef RT_USE_SINGLE_RENDER_THREAD
  assert(rtWrapperSceneUpdateHasLock());
#endif //RT_USE_SINGLE_RENDER_THREAD

  sLockCount--;
#ifdef USE_STD_THREADS
  if (sLockCount == 0)
    sCurrentSceneThread = std::thread::id();
#else
  if (sLockCount == 0)
    sCurrentSceneThread = 0;
#endif

#ifdef USE_STD_THREADS
  std::unique_lock<std::mutex> lock(sSceneLock);
#else
  assert(pthread_mutex_unlock(&sSceneLock) == 0);
#endif
#endif // RUNINMAIN
}*/

#ifdef RUNINMAIN
#define ENTERSCENELOCK()
#define EXITSCENELOCK()
#else
#define ENTERSCENELOCK() rtWrapperSceneUpdateEnter();
#define EXITSCENELOCK() rtWrapperSceneUpdateExit();
#endif

std::thread* gScriptThread = NULL;
std::recursive_mutex gScriptMutex;
std::vector<rtScriptTaskRef> gScriptTasks;
bool gScriptThreadRunning = false;

void scriptThreadProcessing(rtScript* script)
{
  if (script == NULL){
    return;
  }
  bool running = true;
  std::vector<rtScriptTaskRef> tempScripts;
  while (running)
  {
    {
      std::unique_lock<std::recursive_mutex>(gScriptMutex);
      for (std::vector<rtScriptTaskRef>::const_iterator it = gScriptTasks.begin(); it != gScriptTasks.end();it++)
      {
        if ((*it).getPtr() != NULL)
        {
          tempScripts.push_back(*it);
        }
      }
      gScriptTasks.clear();
      running = gScriptThreadRunning;
    }
    ENTERSCENELOCK();
    if (tempScripts.size() > 0)
    {
      for (std::vector<rtScriptTaskRef>::const_iterator it = tempScripts.begin(); it != tempScripts.end(); it++)
      {
        if ((*it).getPtr() != NULL)
        {
          (*it)->executeScript();
        }
      }
      tempScripts.clear();
    }
    script->pump();
    EXITSCENELOCK();

    pxSleepMS(16);
  }
}

rtScript::rtScript(bool threaded):mInitialized(false), mScript(),
                                  mScriptTasks(), mScriptMutex(), mThreaded(threaded)
{
}
rtScript::~rtScript() {}

rtError rtScript::init()
{
  if (false == mInitialized)
  {
    #if defined(RTSCRIPT_SUPPORT_NODE) && defined(RTSCRIPT_SUPPORT_DUKTAPE) 
      static int useDuktape = -1;
    
      if (useDuktape < 0)
      {
        useDuktape = 0;
        rtString f;
        if (rtGetHomeDirectory(f) == RT_OK)
        {
          f.append(".sparkUseDuktape");
          useDuktape = rtFileExists(f)?1:0;
        }
      }
      if (useDuktape != 0)
        createScriptDuk(mScript);
      else
        createScriptNode(mScript);
    #elif defined(RTSCRIPT_SUPPORT_V8)
        createScriptV8(mScript);
    #elif defined(RTSCRIPT_SUPPORT_DUKTAPE)
        createScriptDuk(mScript);
    #elif defined(RTSCRIPT_SUPPORT_NODE)
        createScriptNode(mScript);
    #else
    #error "No Script Engine Supported"
    #endif
    
    mScript->init();
    mInitialized = true;
  }
  if (mThreaded)
  {
    startBackgroundProcessing();
  }
  return RT_OK;
}

rtError rtScript::term()
{
  if (mThreaded)
  {
    stopBackgroundProcessing();
  }
  return RT_OK;
}

rtString rtScript::engine()
{
  return mScript->engine();
}

rtError rtScript::pump() 
{
  mScript->pump();
  return RT_OK;
}

rtError rtScript::collectGarbage() 
{
  mScript->collectGarbage();
  return RT_OK;
}

rtError rtScript::createContext(const char *lang, rtScriptContextRef& ctx)
{
  return mScript->createContext(lang, ctx);
  //return RT_FAIL;
}

void* rtScript::getParameter(rtString param) 
{
  return mScript->getParameter(param);
}

void rtScript::startBackgroundProcessing()
{
  if (gScriptThread == NULL)
  {
    gScriptThreadRunning = true;
    gScriptThread = new std::thread{scriptThreadProcessing, this};
  }
}

void rtScript::stopBackgroundProcessing()
{
  std::unique_lock<std::recursive_mutex>(gScriptMutex);
  gScriptThreadRunning = false;
}

void rtScript::executeTask(rtScriptTaskRef task)
{
  std::unique_lock<std::recursive_mutex>(gScriptMutex);
  gScriptTasks.push_back(task);
}
