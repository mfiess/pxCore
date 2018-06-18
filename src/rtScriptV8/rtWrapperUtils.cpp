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

#include "rtWrapperUtils.h"
#include "rtObjectWrapper.h"
#include "rtFunctionWrapper.h"
#include <rtMutex.h>

#if defined(USE_STD_THREADS)
#include <thread>
#include <mutex>
#endif

using namespace std;

//-----------------------------------

namespace rtScriptV8Utils
{

const int HandleMap::kContextIdIndex = 2;

struct ObjectReference
{
  rtObjectRef        RTObject;
  Persistent<Object> PersistentObject;
  uint32_t           CreationContextId;
};

typedef std::map< rtIObject*, ObjectReference*  > ObjectReferenceMap;
static ObjectReferenceMap objectMap;

uint32_t
GetContextId(Local<Context>& ctx)
{
  if (ctx.IsEmpty()) return 0;

  HandleScope handleScope(ctx->GetIsolate());
  Local<Value> val = ctx->GetEmbedderData(HandleMap::kContextIdIndex);
  assert(!val.IsEmpty());
  return val->Uint32Value();
}

static void WeakCallback(const WeakCallbackInfo<rtIObject>& data) {
   Locker locker(data.GetIsolate());
   Isolate::Scope isolateScope(data.GetIsolate());
   HandleScope handleScope(data.GetIsolate());
   rtObjectRef temp;
   ObjectReferenceMap::iterator j = objectMap.find(data.GetParameter());
   if (j != objectMap.end())
   {
      // TODO: Removing this temporarily until we understand how this callback works. I
      // would have assumed that this is a weak persistent since we called SetWeak() on it
      // before inserting it into the objectMap_rt2v8 map.
      // assert(p->IsWeak());
      //
      j->second->PersistentObject.ClearWeak();
      j->second->PersistentObject.Reset();
      temp = j->second->RTObject;
      delete j->second;
      objectMap.erase(j);
   }
   else
   {
      rtLogWarn("failed to find:%p in map", data.GetParameter());
   }
   if (NULL != temp.getPtr())
   {
      rtObjectRef parentRef;
      rtError err = temp.get<rtObjectRef>("parent",parentRef);
      if (err == RT_OK)
      {
         if (NULL == parentRef)
         {
            temp.send("dispose");
         }
      }
   }
}

void
HandleMap::clearAllForContext(uint32_t contextId)
{
  typedef ObjectReferenceMap::iterator iterator;

  int n = 0;
  rtLogInfo("clearing all persistent handles for: %u size:%u", contextId,
    static_cast<unsigned>(objectMap.size()));
  vector<iterator> refs;
  for (iterator begin = objectMap.begin(), end = objectMap.end(); begin != end;)
  {
      ObjectReference* ref = begin->second;
      //rtLogInfo("looking at:%d == %d", ref->CreationContextId, contextId);

      if (ref->CreationContextId == contextId)
      {
        refs.push_back(begin);
        ref->PersistentObject.ClearWeak();
        ref->PersistentObject.Reset();
        delete ref;
        ref = NULL;
        begin++;
        n++;
      }
      else
      {
        //rtLogInfo("looping :%d == %d", ref->CreationContextId, contextId);
        ++begin;
      }
  }

  for(unsigned int i=0; i<refs.size();i++)
  {
    objectMap.erase(refs[i]);
  }
  refs.clear();
  //rtLogInfo("clear complete for id[%d] . removed:%d size:%u", contextId, n,
      //static_cast<unsigned>(objectMap.size()));
}

void HandleMap::addWeakReference(v8::Isolate* isolate, const rtObjectRef& from, Local<Object>& to)
{
  HandleScope handleScope(isolate);
  Local<Context> creationContext = to->CreationContext();

  uint32_t const contextIdCreation = GetContextId(creationContext);
  assert(contextIdCreation != 0);
  ObjectReferenceMap::iterator i = objectMap.find(from.getPtr());
  if (i != objectMap.end())
  {
    if (!(i->second->PersistentObject.IsNearDeath()))
    {
      rtLogError("About to add weak reference which is already present");
    }
  }
  //assert(i == objectMap.end());

  if (i == objectMap.end())
  {
    // rtLogInfo("add id:%u addr:%p", contextIdCreation, from.getPtr());
    ObjectReference* entry(new ObjectReference());
    entry->PersistentObject.Reset(isolate, to);
    entry->PersistentObject.SetWeak(from.getPtr(), WeakCallback, v8::WeakCallbackType::kParameter);
    entry->RTObject = from;
    entry->CreationContextId = contextIdCreation;
    objectMap.insert(std::make_pair(from.getPtr(), entry));
  }
rtWrapperSceneUpdateExit();

  #if 0
  static FILE* f = NULL;
  if (!f)
    f = fopen("/tmp/handles.txt", "w+");
  if (f)
  {
    rtString desc;
    const_cast<rtObjectRef &>(from).sendReturns<rtString>("description", desc);
    fprintf(f, "%p (%s) => %p\n", from.getPtr(), desc.cString(), h);
  }
  #endif

}

Local<Object> HandleMap::lookupSurrogate(v8::Local<v8::Context>& ctx, const rtObjectRef& from)
{
  Isolate* isolate = ctx->GetIsolate();
  EscapableHandleScope scope(isolate);
  Local<Object> obj;
  ObjectReferenceMap::iterator i = objectMap.find(from.getPtr());
  if (i == objectMap.end())
  {
    return scope.Escape(obj);
  }
  obj = PersistentToLocal(isolate, i->second->PersistentObject);

  if (!obj.IsEmpty())
  {
    // JR sanity check
    if ((from.getPtr() != NULL) && (from.get<rtFunctionRef>("animateTo") != NULL) &&
        (!obj->Has(v8::String::NewFromUtf8(isolate,"animateTo"))))
    {
      // TODO change description to a property
      rtString desc;
      const_cast<rtObjectRef &>(from).sendReturns<rtString>("description", desc);
      rtLogError("type mismatch in handle map %p (%s)", from.getPtr(), desc.cString());
      assert(false);
    }
  }

  return scope.Escape(obj);
}

bool rtIsPromise(const rtValue& v)
{
  if (v.getType() != RT_rtObjectRefType)
    return false;

  rtObjectRef ref = v.toObject();
  if (!ref)
    return false;

  rtString desc;
  rtError err = ref.sendReturns<rtString>("description", desc);
  if (err != RT_OK)
    return false;

  return strcmp(desc.cString(), "rtPromise") == 0;
}



using namespace v8;

Handle<Value> rt2js(Local<Context>& ctx, const rtValue& v)
{
  Context::Scope contextScope(ctx);

  Isolate* isolate = ctx->GetIsolate();

  switch (v.getType())
  {
    case RT_int32_tType:
      {
        int32_t i = v.toInt32();
        return Integer::New(isolate, i);
      }
      break;
    case RT_uint32_tType:
      {
        uint32_t u = v.toUInt32();
        return Integer::NewFromUnsigned(isolate, u);
      }
      break;
    case RT_int64_tType:
      {
        double d = v.toDouble();
        return Number::New(isolate, d);
      }
      break;
    case RT_floatType:
      {
        float f = v.toFloat();
        return Number::New(isolate, f);
      }
      break;
    case RT_doubleType:
      {
        double d = v.toDouble();
        return Number::New(isolate, d);
      }
      break;
    case RT_uint64_tType:
      {
        double d = v.toDouble();
        return Number::New(isolate, d);
      }
      break;
    case RT_functionType:
      {
        rtFunctionRef func = v.toFunction();
        if (!func)
          return v8::Null(isolate);
        return rtFunctionWrapper::createFromFunctionReference(ctx, isolate, func);
      }
      break;
    case RT_rtObjectRefType:
      {
        rtObjectRef obj = v.toObject();
        if (!obj)
          return v8::Null(isolate);

        return jsObjectWrapper::isJavaScriptObjectWrapper(obj)
          ? static_cast<jsObjectWrapper *>(obj.getPtr())->getWrappedObject()
          : rtObjectWrapper::createFromObjectReference(ctx, obj);
      }
      break;
    case RT_boolType:
      return Boolean::New(isolate, v.toBool());
      break;
    case RT_rtStringType:
      {
        rtString s = v.toString();
        return String::NewFromUtf8(isolate, s.cString());
      }
      break;
    case RT_voidPtrType:
      rtLogWarn("attempt to convert from void* to JS object");
      return Handle<Value>(); // TODO
      break;
    case 0: // This is really a value rtValue() will set mType to zero
      return Handle<Value>();
      break;
    default:
      rtLogFatal("unsupported rtValue [(char value(%c) int value(%d)] to javascript conversion",
          v.getType(), v.getType());
      break;
  }

  return Undefined(isolate);
}

rtValue js2rt(v8::Local<v8::Context>& ctx, const Handle<Value>& val, rtWrapperError* )
{
  v8::Isolate* isolate = ctx->GetIsolate();

  if (val->IsUndefined()) { return rtValue((void *)0); }
  if (val->IsNull())      { return rtValue((char *)0); }
  if (val->IsString())    { return toString(val); }
  if (val->IsFunction())  { return rtValue(rtFunctionRef(new jsFunctionWrapper(ctx, val))); }
  if (val->IsArray() || val->IsObject())
  {
    // This is mostly a heuristic. We should probably set a second internal
    // field and use a uuid as a magic number to ensure this is one of our
    // wrapped objects.
    // It's very possible that someone is trying to use another wrapped object
    // from some other native addon. Maybe that would actuall work and fail
    // at a lower level?
    Local<Object> obj = val->ToObject();
    if (obj->InternalFieldCount() > 0)
    {
      return rtObjectWrapper::unwrapObject(obj);
    }
    else
    {
      // this is a regular JS object. i.e. one that does not wrap an rtObject.
      // in this case, we'll provide the necessary adapter.
      return rtValue(new jsObjectWrapper(isolate, obj->ToObject(), val->IsArray()));
    }
  }

  if (val->IsBoolean())   { return rtValue(val->BooleanValue()); }
  if (val->IsNumber())    { return rtValue(val->NumberValue()); }
  if (val->IsInt32())     { return rtValue(val->Int32Value()); }
  if (val->IsUint32())    { return rtValue(val->Uint32Value()); }

  rtLogFatal("unsupported javascript -> rtValue type conversion");
  return rtValue(0);
}

} // namespace
