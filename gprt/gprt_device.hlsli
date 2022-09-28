// MIT License

// Copyright (c) 2022 Nathan V. Morrical

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

namespace gprt {
  inline uint32_t make_8bit(const float f)
  {
    return min(255,max(0,int(f*256.f)));
  }

  inline uint32_t make_rgba(const float3 color)
  {
    return
      (make_8bit(color.x) << 0) +
      (make_8bit(color.y) << 8) +
      (make_8bit(color.z) << 16) +
      (0xffU << 24);
  }

  [[vk::ext_instruction(4447)]]
  RaytracingAccelerationStructure getAccelHandle(uint64_t ptr);
};

struct PushConstants {
  uint64_t r[16];
};
[[vk::push_constant]] PushConstants pc;

/*
  The DXC compiler unfortunately doesn't handle multiple mixed entry points 
  of different stage kinds in the same compilation step. So we instead 
  use these macros to selectively filter all but a particular shader type, 
  and compile repeatedly for all shader kinds (compute, raygen, anyhit, etc)

  Down the road, this could also give us an opportunity to shim in an Embree
  backend.
*/
#ifndef GPRT_COMPUTE_PROGRAM
#ifdef COMPUTE
#define GPRT_COMPUTE_PROGRAM(progName)                                  \
  /* fwd decl for the kernel func to call */                            \
  void progName(uint3 tid);                                             \
  [shader("compute")]                                                   \
  [numthreads(1,1,1)]                                                   \
  void __compute__##progName( uint3 tid : SV_DispatchThreadID)          \
  {                                                                     \
    progName(tid);                                                      \
  }                                                                     \
  /* now the actual device code that the user is writing: */            \
  void progName                                                         \
/* program args and body supplied by user ... */                      
#else
#define GPRT_COMPUTE_PROGRAM(progName)                                  \
/* Dont add entry point decorators, instead treat as just a function. */\
void progName                                                           \
/* program args and body supplied by user ... */   
#endif
#endif



#ifndef GPRT_RAYGEN_PROGRAM
#ifdef RAYGEN
#define GPRT_RAYGEN_PROGRAM(progName, RecordType)                       \
  /* fwd decl for the kernel func to call */                            \
  void progName(in RecordType recordData);                              \
  [[vk::shader_record_ext]]                                             \
  ConstantBuffer<RecordType> progName##RecordData;                      \
  [shader("raygeneration")]                                             \
  void __raygen__##progName()                                           \
  {                                                                     \
    progName(progName##RecordData);                                     \
  }                                                                     \
  /* now the actual device code that the user is writing: */            \
  void progName                                                         \
/* program args and body supplied by user ... */                      
#else
#define GPRT_RAYGEN_PROGRAM(progName, RecordType)                       \
/* Dont add entry point decorators, instead treat as just a function. */\
void progName                                                           \
/* program args and body supplied by user ... */   
#endif
#endif


#ifndef GPRT_CLOSEST_HIT_PROGRAM
#ifdef CLOSESTHIT
#define GPRT_CLOSEST_HIT_PROGRAM(progName, RecordType, PayloadType, AttributeType)     \
  /* fwd decl for the kernel func to call */                            \
  void progName(in RecordType record, inout PayloadType payload, in AttributeType attributes);       \
  [[vk::shader_record_ext]]                                             \
  ConstantBuffer<RecordType> progName##RecordData;                      \
  [shader("closesthit")]                                                \
  void __closesthit__##progName(inout PayloadType payload, in AttributeType attributes)              \
  {                                                                     \
    progName(progName##RecordData, payload, attributes);                            \
  }                                                                     \
  /* now the actual device code that the user is writing: */            \
  void progName                                                         \
/* program args and body supplied by user ... */                      
#else
#define GPRT_CLOSEST_HIT_PROGRAM(progName, RecordType, PayloadType, AttributeType)     \
/* Dont add entry point decorators, instead treat as just a function. */\
void progName                                                           \
/* program args and body supplied by user ... */   
#endif
#endif



#ifndef GPRT_INTERSECTION_PROGRAM
#ifdef INTERSECTION
#define GPRT_INTERSECTION_PROGRAM(progName, RecordType)     \
  /* fwd decl for the kernel func to call */                            \
  void progName(in RecordType record);       \
  [[vk::shader_record_ext]]                                             \
  ConstantBuffer<RecordType> progName##RecordData;                      \
  [shader("intersection")]                                                \
  void __intersection__##progName()              \
  {                                                                     \
    progName(progName##RecordData);                            \
  }                                                                     \
  /* now the actual device code that the user is writing: */            \
  void progName                                                         \
/* program args and body supplied by user ... */                      
#else
#define GPRT_INTERSECTION_PROGRAM(progName, RecordType)     \
/* Dont add entry point decorators, instead treat as just a function. */\
void progName                                                           \
/* program args and body supplied by user ... */   
#endif
#endif


#ifndef GPRT_MISS_PROGRAM
#ifdef MISS
#define GPRT_MISS_PROGRAM(progName, RecordType, PayloadType)     \
  /* fwd decl for the kernel func to call */                            \
  void progName(in RecordType record, inout PayloadType payload);   \
  [[vk::shader_record_ext]]                                             \
  ConstantBuffer<RecordType> progName##RecordData;                      \
  [shader("miss")]                                                \
  void __miss__##progName(inout PayloadType payload)                  \
  {                                                                     \
    progName(progName##RecordData, payload);                            \
  }                                                                     \
  /* now the actual device code that the user is writing: */            \
  void progName                                                         \
/* program args and body supplied by user ... */                      
#else
#define GPRT_MISS_PROGRAM(progName, RecordType, PayloadType)                       \
/* Dont add entry point decorators, instead treat as just a function. */\
void progName                                                           \
/* program args and body supplied by user ... */   
#endif
#endif

// note, for bounds programs, I'd like to pass the SBT records in to enable
// reading vertices and computing actual AABBs...
// this is an issue with OWL, where you have to instead store a duplicate of everything
// in a global launch parameters structure.

// One tricky thing is that there might be multiple ray types, and with each ray type
// comes a unique SBT record. But an AABB bounds can only have one AABB for all ray types.
// so I'm thinking we call some wrapper function multiple times, one per SBT record type, or something like that...
// and then we'd compute the union of all the generated AABBs for all records...