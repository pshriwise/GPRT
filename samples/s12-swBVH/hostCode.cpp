// MIT License

// Copyright (c) 2022 Nathan V. Morrical

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// This program sets up a single geometric object, a mesh for a cube, and
// its acceleration structure, then ray traces it.

// public GPRT API
#include <gprt.h>

#include <iomanip>

// our shared data structures between host and device
#include "sharedCode.h"
#include "lbvh.h"

// for generating meshes
#include <generator.hpp>
using namespace generator;

#define LOG(message)                                                                                                   \
  std::cout << GPRT_TERMINAL_BLUE;                                                                                     \
  std::cout << "#gprt.sample(main): " << message << std::endl;                                                         \
  std::cout << GPRT_TERMINAL_DEFAULT;
#define LOG_OK(message)                                                                                                \
  std::cout << GPRT_TERMINAL_LIGHT_BLUE;                                                                               \
  std::cout << "#gprt.sample(main): " << message << std::endl;                                                         \
  std::cout << GPRT_TERMINAL_DEFAULT;

extern GPRTProgram s12_deviceCode;
extern GPRTProgram lbvhDeviceCode;

// A class we'll use to quickly generate meshes and bottom level trees
template <typename T> struct Mesh {
  std::vector<float3> vertices;
  std::vector<uint3> indices;
  GPRTBufferOf<float3> vertexBuffer;
  GPRTBufferOf<uint3> indexBuffer;
  Mesh(){};
  Mesh(GPRTContext context, T generator) {
    // Use the generator to generate vertices and indices
    auto vertGenerator = generator.vertices();
    auto triGenerator = generator.triangles();
    while (!vertGenerator.done()) {
      auto vertex = vertGenerator.generate();
      auto position = vertex.position;
      vertices.push_back(float3(position[0], position[1], position[2]));
      vertGenerator.next();
    }
    while (!triGenerator.done()) {
      Triangle triangle = triGenerator.generate();
      auto vertices = triangle.vertices;
      indices.push_back(uint3(vertices[0], vertices[1], vertices[2]));
      triGenerator.next();
    }

    // Upload those to the device
    vertexBuffer = gprtDeviceBufferCreate<float3>(context, vertices.size(), vertices.data());
    indexBuffer = gprtDeviceBufferCreate<uint3>(context, indices.size(), indices.data());
  };

  void cleanup() {
    gprtBufferDestroy(vertexBuffer);
    gprtBufferDestroy(indexBuffer);
  };
};

// initial image resolution
const int2 fbSize = {1920, 1080};

// final image output
const char *outFileName = "s12-swBVH.png";

// Initial camera parameters
float3 lookFrom = {1.7f, 2.4f, -2.8f};
float3 lookAt = {0.0f, 0.5f, 0.0f};
float3 lookUp = {0.f, -1.f, 0.f};
float cosFovy = 0.66f;

#include <iostream>
int
main(int ac, char **av) {
  // In this example, we'll use compute shaders to build a sofware-traversable 
  // acceleration structure in parallel on the GPU. We'll use this tree for custom
  // tree traversal, namely a closest point on triangle query to compute a 
  // signed distance field.
  LOG("gprt example '" << av[0] << "' starting up");

  // create a context on the first device:
  gprtRequestWindow(fbSize.x, fbSize.y, "S04 Compute AABB");
  GPRTContext context = gprtContextCreate(nullptr, 1);
  GPRTModule module = gprtModuleCreate(context, s12_deviceCode);
  GPRTModule lbvhModule = gprtModuleCreate(context, lbvhDeviceCode);

  // ##################################################################
  // set up all the GPU kernels we want to run
  // ##################################################################

  // -------------------------------------------------------
  // set up LBVH programs for a triangle-based SW tree. We
  // will use this SW tree for closest-point-on-triangle queries
  // -------------------------------------------------------

  GPRTComputeOf<LBVHData> computeBounds = gprtComputeCreate<LBVHData>(context, lbvhModule, "ComputeTriangleBounds");
  GPRTComputeOf<LBVHData> computeCodes = gprtComputeCreate<LBVHData>(context, lbvhModule, "ComputeTriangleMortonCodes");
  GPRTComputeOf<LBVHData> makeNodes = gprtComputeCreate<LBVHData>(context, lbvhModule, "MakeNodes");
  GPRTComputeOf<LBVHData> splitNodes = gprtComputeCreate<LBVHData>(context, lbvhModule, "SplitNodes");
  GPRTComputeOf<LBVHData> buildHierarchy = gprtComputeCreate<LBVHData>(context, lbvhModule, "BuildTriangleHierarchy");

  // Triangle mesh we'll build the SW BVH over
  Mesh<TeapotMesh> mesh(context, TeapotMesh{{1}});

  LBVHData lbvhParams = {};
  lbvhParams.numPrims = mesh.indices.size();
  lbvhParams.numInner = lbvhParams.numPrims - 1;
  lbvhParams.numNodes = 2 * lbvhParams.numPrims - 1;
  
  // Input to LBVH construction
  lbvhParams.triangles = gprtBufferGetHandle(mesh.indexBuffer);
  lbvhParams.positions = gprtBufferGetHandle(mesh.vertexBuffer);

  // Output / intermediate buffers
  GPRTBufferOf<uint8_t> scratch = gprtDeviceBufferCreate<uint8_t>(context);
  GPRTBufferOf<uint32_t> mortonCodes = gprtDeviceBufferCreate<uint32_t>(context, lbvhParams.numPrims);
  GPRTBufferOf<uint32_t> ids = gprtDeviceBufferCreate<uint32_t>(context, lbvhParams.numPrims);
  GPRTBufferOf<int4> nodes = gprtDeviceBufferCreate<int4>(context, lbvhParams.numNodes);
  GPRTBufferOf<float3> aabbs = gprtDeviceBufferCreate<float3>(context, 2 * lbvhParams.numNodes);
  lbvhParams.mortonCodes = gprtBufferGetHandle(mortonCodes);
  lbvhParams.ids = gprtBufferGetHandle(ids);
  lbvhParams.nodes = gprtBufferGetHandle(nodes);
  lbvhParams.aabbs = gprtBufferGetHandle(aabbs);

  // initialize root AABB
  gprtBufferMap(aabbs);
  float3* aabbPtr = gprtBufferGetPointer(aabbs);
  aabbPtr[0].x = aabbPtr[0].y = aabbPtr[0].z = 1e20f;
  aabbPtr[1].x = aabbPtr[1].y = aabbPtr[1].z = -1e20f;
  gprtBufferUnmap(aabbs);

  gprtComputeSetParameters(computeBounds,  &lbvhParams);
  gprtComputeSetParameters(computeCodes,   &lbvhParams);
  gprtComputeSetParameters(makeNodes,      &lbvhParams);
  gprtComputeSetParameters(splitNodes,     &lbvhParams);
  gprtComputeSetParameters(buildHierarchy, &lbvhParams);

  gprtBuildShaderBindingTable(context, GPRT_SBT_COMPUTE);

  gprtComputeLaunch1D(context, computeBounds, lbvhParams.numPrims);
  gprtComputeLaunch1D(context, computeCodes, lbvhParams.numPrims);
  gprtBufferSortPayload(context, mortonCodes, ids, scratch);
  gprtComputeLaunch1D(context, makeNodes, lbvhParams.numNodes);
  gprtComputeLaunch1D(context, splitNodes, lbvhParams.numInner);
  gprtComputeLaunch1D(context, buildHierarchy, lbvhParams.numPrims);

  {
    gprtBufferMap(nodes);
    gprtBufferMap(aabbs);
    int4 *nodePtr = gprtBufferGetPointer(nodes);
    float3 *aabbPtr = gprtBufferGetPointer(aabbs);
    for (uint32_t i = 0; i < lbvhParams.numNodes; ++i) {
      std::cout<< std::setw(4) << nodePtr[i].x << " " << std::setw(4) << nodePtr[i].y << " " << std::setw(4) << nodePtr[i].z << " " << std::setw(4) << nodePtr[i].w << " ";
      std::cout<<"\taabb (" << aabbPtr[i*2+0].x << " " << aabbPtr[i*2+0].y << " " << aabbPtr[i*2+0].z << ")";
      std::cout<<", (" << aabbPtr[i*2+1].x << " " << aabbPtr[i*2+1].y << " " << aabbPtr[i*2+1].z << ")"<< std::endl;
    }
    gprtBufferUnmap(nodes);
    gprtBufferUnmap(aabbs);
  }

  // -------------------------------------------------------
  // set up ray gen program
  // -------------------------------------------------------
  GPRTRayGenOf<RayGenData> rayGen = gprtRayGenCreate<RayGenData>(context, module, "simpleRayGen");

  // ##################################################################
  // set the parameters for the rest of our kernels
  // ##################################################################

  // Setup pixel frame buffer
  GPRTBuffer frameBuffer = gprtDeviceBufferCreate(context, sizeof(uint32_t), fbSize.x * fbSize.y);

  // Raygen program frame buffer
  RayGenData *rayGenData = gprtRayGenGetParameters(rayGen);
  rayGenData->frameBuffer = gprtBufferGetHandle(frameBuffer);

  gprtBuildShaderBindingTable(context, GPRT_SBT_ALL);

  // ##################################################################
  // now that everything is ready: launch it ....
  // ##################################################################

  LOG("launching ...");

  bool firstFrame = true;
  double xpos = 0.f, ypos = 0.f;
  double lastxpos, lastypos;
  int iFrame = 0;
  do {
    float speed = .001f;
    lastxpos = xpos;
    lastypos = ypos;
    gprtGetCursorPos(context, &xpos, &ypos);
    if (firstFrame) {
      lastxpos = xpos;
      lastypos = ypos;
    }
    int state = gprtGetMouseButton(context, GPRT_MOUSE_BUTTON_LEFT);

    // If we click the mouse, we should rotate the camera
    // Here, we implement some simple camera controls
    if (state == GPRT_PRESS || firstFrame) {
      firstFrame = false;
      float4 position = {lookFrom.x, lookFrom.y, lookFrom.z, 1.f};
      float4 pivot = {lookAt.x, lookAt.y, lookAt.z, 1.0};
#ifndef M_PI
#define M_PI 3.1415926f
#endif

      // step 1 : Calculate the amount of rotation given the mouse movement.
      float deltaAngleX = (2 * M_PI / fbSize.x);
      float deltaAngleY = (M_PI / fbSize.y);
      float xAngle = (lastxpos - xpos) * deltaAngleX;
      float yAngle = (lastypos - ypos) * deltaAngleY;

      // step 2: Rotate the camera around the pivot point on the first axis.
      float4x4 rotationMatrixX = rotation_matrix(rotation_quat(lookUp, xAngle));
      position = (mul(rotationMatrixX, (position - pivot))) + pivot;

      // step 3: Rotate the camera around the pivot point on the second axis.
      float3 lookRight = cross(lookUp, normalize(pivot - position).xyz());
      float4x4 rotationMatrixY = rotation_matrix(rotation_quat(lookRight, yAngle));
      lookFrom = ((mul(rotationMatrixY, (position - pivot))) + pivot).xyz();

      // ----------- compute variable values  ------------------
      float3 camera_pos = lookFrom;
      float3 camera_d00 = normalize(lookAt - lookFrom);
      float aspect = float(fbSize.x) / float(fbSize.y);
      float3 camera_ddu = cosFovy * aspect * normalize(cross(camera_d00, lookUp));
      float3 camera_ddv = cosFovy * normalize(cross(camera_ddu, camera_d00));
      camera_d00 -= 0.5f * camera_ddu;
      camera_d00 -= 0.5f * camera_ddv;

      // ----------- set variables  ----------------------------
      RayGenData *raygenData = gprtRayGenGetParameters(rayGen);
      raygenData->camera.pos = camera_pos;
      raygenData->camera.dir_00 = camera_d00;
      raygenData->camera.dir_du = camera_ddu;
      raygenData->camera.dir_dv = camera_ddv;

    }
    
    rayGenData->iTime = gprtGetTime(context);
    rayGenData->iFrame = iFrame;

    // Use this to upload all set parameters to our ray tracing device
    gprtBuildShaderBindingTable(context, GPRT_SBT_RAYGEN);

    // Calls the GPU raygen kernel function
    gprtRayGenLaunch2D(context, rayGen, fbSize.x, fbSize.y);

    // If a window exists, presents the framebuffer here to that window
    gprtBufferPresent(context, frameBuffer);

    iFrame++;
  }
  // returns true if "X" pressed or if in "headless" mode
  while (!gprtWindowShouldClose(context));

  // Save final frame to an image
  LOG("done with launch, writing frame buffer to " << outFileName);
  gprtBufferSaveImage(frameBuffer, fbSize.x, fbSize.y, outFileName);
  LOG_OK("written rendered frame buffer to file " << outFileName);

  // ##################################################################
  // and finally, clean up
  // ##################################################################

  LOG("cleaning up ...");

  gprtBufferDestroy(frameBuffer);
  gprtRayGenDestroy(rayGen);

  mesh.cleanup();
  gprtComputeDestroy(computeBounds);
  gprtComputeDestroy(computeCodes);
  gprtComputeDestroy(makeNodes);
  gprtComputeDestroy(splitNodes);
  gprtComputeDestroy(buildHierarchy);
  gprtModuleDestroy(module);
  gprtContextDestroy(context);

  LOG_OK("seems all went OK; app is done, this should be the last output ...");
}
