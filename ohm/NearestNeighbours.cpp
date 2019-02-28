// Copyright (c) 2017
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#include "NearestNeighbours.h"

#include "private/GpuProgramRef.h"
#include "private/MapLayoutDetail.h"
#include "private/NearestNeighboursDetail.h"
#include "private/OccupancyMapDetail.h"
#include "private/OccupancyQueryAlg.h"

#include "DefaultLayer.h"
#include "Key.h"
#include "MapChunk.h"
#include "OccupancyMap.h"
#include "OhmGpu.h"
#include "QueryFlag.h"

#include <gputil/gpuPinnedBuffer.h>

#include <3esservermacros.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <functional>
#include <iostream>

#if defined(OHM_EMBED_GPU_CODE) && GPUTIL_TYPE == GPUTIL_OPENCL
#include "NearestNeighboursQueryResource.h"
#endif  // defined(OHM_EMBED_GPU_CODE) && GPUTIL_TYPE == GPUTIL_OPENCL

// See nearestneighbours.cl define of the same name.
// #define CACHE_LOCAL_RESULTS

using namespace ohm;

namespace
{
  // TODO: base this value on GPU capabilities.
  const size_t kGpuBatchSize = 32 * 32 * 32;

#ifdef OHM_EMBED_GPU_CODE
  GpuProgramRef program_ref("NearestNeighboursQuery", GpuProgramRef::kSourceString, NearestNeighboursQueryCode,
                            NearestNeighboursQueryCode_length);
#else   // OHM_EMBED_GPU_CODE
  GpuProgramRef program_ref("NearestNeighboursQuery", GpuProgramRef::kSourceFile, "NearestNeighboursQuery.cl");
#endif  // OHM_EMBED_GPU_CODE

  int initialiseGpu(const OccupancyMapDetail & /*map*/, NearestNeighboursDetail &query)
  {
    if (query.gpu_ok)
    {
      return 0;
    }

    NearestNeighboursDetail::GpuData &gpu_data = query.gpu_data;
    query.gpu = gpuDevice();

    if (!program_ref.addReference(query.gpu))
    {
      return -1;
    }

    gpu_data.queue = query.gpu.defaultQueue();
    gpu_data.local_ranges.resize(kGpuBatchSize);
    gpu_data.region_keys.resize(kGpuBatchSize);
    gpu_data.local_keys.resize(kGpuBatchSize);

#if OHM_GPU == OHM_GPU_OPENCL
    gpu_data.nn_kernel = gputil::openCLKernel(program_ref.program(), "nearestNeighbours");
#endif  // OHM_GPU == OHM_GPU_OPENCL

    if (!gpu_data.nn_kernel.isValid())
    {
      program_ref.releaseReference();
      return -1;
    }

#ifdef CACHE_LOCAL_RESULTS
    // Add local ranges argument.
    gpu_data.nn_kernel.addLocal([](size_t workgroupSize) { return sizeof(float) * workgroupSize; });
    // Add region keys argument.s
    gpu_data.nn_kernel.addLocal([](size_t workgroupSize) { return sizeof(gputil::short3) * workgroupSize; });
    // Add local voxel keys argument.
    gpu_data.nn_kernel.addLocal([](size_t workgroupSize) { return sizeof(gputil::uchar3) * workgroupSize; });
#endif  // CACHE_LOCAL_RESULTS

    gpu_data.nn_kernel.calculateOptimalWorkGroupSize();

#if OHM_GPU == OHM_GPU_OPENCL
    gpu_data.info_kernel = gputil::openCLKernel(program_ref.program(), "showNNInfo");
#endif  // OHM_GPU == OHM_GPU_OPENCL
    if (!gpu_data.info_kernel.isValid())
    {
      program_ref.releaseReference();
      return -1;
    }

    gpu_data.info_kernel.calculateOptimalWorkGroupSize();

    // Initialise buffers.
    gpu_data.gpu_voxels = gputil::Buffer(query.gpu, kGpuBatchSize * sizeof(float), gputil::kBfReadHost);
    gpu_data.gpu_voxel_region_keys =
      gputil::Buffer(query.gpu, kGpuBatchSize * sizeof(gputil::short3), gputil::kBfReadHost);
    gpu_data.gpu_voxel_voxel_keys =
      gputil::Buffer(query.gpu, kGpuBatchSize * sizeof(gputil::uchar3), gputil::kBfReadHost);
    gpu_data.gpu_ranges = gputil::Buffer(query.gpu, kGpuBatchSize * sizeof(float), gputil::kBfWriteHost);
    gpu_data.gpu_result_region_keys =
      gputil::Buffer(query.gpu, kGpuBatchSize * sizeof(gputil::short3), gputil::kBfWriteHost);
    gpu_data.gpu_result_voxel_keys =
      gputil::Buffer(query.gpu, kGpuBatchSize * sizeof(gputil::uchar3), gputil::kBfWriteHost);
    gpu_data.gpu_result_count = gputil::Buffer(query.gpu, sizeof(unsigned), gputil::kBfReadWriteHost);

    query.gpu_ok = true;

    return 0;
  }

  void releaseGpu(NearestNeighboursDetail &query)
  {
    if (query.gpu_ok)
    {
      NearestNeighboursDetail::GpuData &gpu_data = query.gpu_data;
      query.gpu_ok = false;
      gpu_data.nn_kernel.release();
      gpu_data.info_kernel.release();
      program_ref.releaseReference();
    }
  }

  unsigned regionNearestNeighboursCpu(OccupancyMap &map, NearestNeighboursDetail &query, const glm::i16vec3 &region_key,
                                      ClosestResult &closest)
  {
    const OccupancyMapDetail &map_data = *map.detail();
    const auto chunk_search = map_data.findRegion(region_key);
    glm::vec3 query_origin, voxel_vector;
    Key voxel_key(nullptr);
    const MapChunk *chunk = nullptr;
    const float *occupancy = nullptr;
    float range_squared = 0;
    unsigned added = 0;

    std::function<bool(float, const OccupancyMapDetail &)> voxel_occupied_func;

    query_origin = glm::vec3(query.near_point - map.origin());

    if (chunk_search == map_data.chunks.end())
    {
      // The entire region is unknown space...
      if ((query.query_flags & ohm::kQfUnknownAsOccupied) == 0)
      {
        // ... and unknown space is considered free. No results to add.
        return 0;
      }

      // ... and we have to treat unknown space as occupied.
      chunk = nullptr;
      occupancy = nullptr;
      // Setup voxel occupancy test function to pass all voxels in this region.
      voxel_occupied_func = [](const float, const OccupancyMapDetail &) -> bool { return true; };
    }
    else
    {
      chunk = chunk_search->second;
      occupancy = chunk->layout->layer(chunk->layout->occupancyLayer()).voxelsAs<float>(*chunk);
      // Setup the voxel test function to check the occupancy threshold and behaviour flags.
      voxel_occupied_func = [&query](const float voxel, const OccupancyMapDetail &map_data) -> bool {
        if (voxel == voxel::invalidMarkerValue())
        {
          if (query.query_flags & ohm::kQfUnknownAsOccupied)
          {
            return true;
          }
          return false;
        }
        if (voxel >= map_data.occupancy_threshold_value ||
            (query.query_flags & ohm::kQfUnknownAsOccupied) && voxel == voxel::invalidMarkerValue())
        {
          return true;
        }
        return false;
      };
    }

    TES_STMT(std::vector<tes::Vector3f> includedOccupied);
    TES_STMT(std::vector<tes::Vector3f> excludedOccupied);
    TES_STMT(std::vector<tes::Vector3f> includedUncertain);
    TES_STMT(std::vector<tes::Vector3f> excludedUncertain);
    // TES_BOX_W(g_3es, TES_COLOUR(LightSeaGreen), 0u,
    //           glm::value_ptr(region_centre), glm::value_ptr(map.regionSpatialResolution()));

    for (int z = 0; z < map_data.region_voxel_dimensions.z; ++z)
    {
      for (int y = 0; y < map_data.region_voxel_dimensions.y; ++y)
      {
        for (int x = 0; x < map_data.region_voxel_dimensions.x; ++x)
        {
          if (voxel_occupied_func(*occupancy, map_data))
          {
            // Occupied voxel, or invalid voxel to be treated as occupied.
            // Calculate range to centre.
            voxel_key = Key(region_key, x, y, z);
            voxel_vector = map.voxelCentreLocal(voxel_key);
            voxel_vector -= query_origin;
            range_squared = glm::dot(voxel_vector, voxel_vector);
            if (range_squared <= query.search_radius * query.search_radius)
            {
              query.intersected_voxels.push_back(voxel_key);
              query.ranges.push_back(std::sqrt(range_squared));

              if (range_squared < closest.range)
              {
                closest.index = query.intersected_voxels.size() - 1;
                closest.range = range_squared;
              }

              ++added;
#ifdef TES_ENABLE
              if (occupancy && *occupancy != voxel::invalidMarkerValue())
              {
                includedOccupied.push_back(tes::V3Arg(glm::value_ptr(map.voxelCentreGlobal(voxel_key))));
              }
              else
              {
                includedUncertain.push_back(tes::V3Arg(glm::value_ptr(map.voxelCentreGlobal(voxel_key))));
              }
#endif  // TES_ENABLE
            }
#ifdef TES_ENABLE
            else
            {
              if (occupancy && *occupancy != voxel::invalidMarkerValue())
              {
                excludedOccupied.push_back(tes::V3Arg(glm::value_ptr(map.voxelCentreGlobal(voxel_key))));
              }
              else
              {
                excludedUncertain.push_back(tes::V3Arg(glm::value_ptr(map.voxelCentreGlobal(voxel_key))));
              }
            }
#endif  // TES_ENABLE
          }

          // Next voxel. Will be null for uncertain regions.
          occupancy = (occupancy) ? occupancy + 1 : occupancy;
        }
      }
    }

#ifdef TES_ENABLE
    // Visualise points.
    if (!excludedUncertain.empty())
    {
      TES_POINTS(g_3es, TES_COLOUR(Salmon), &excludedUncertain.data()->x, (unsigned)excludedUncertain.size(),
                 sizeof(tes::Vector3f));
    }

    if (!includedUncertain.empty())
    {
      TES_POINTS(g_3es, TES_COLOUR(PowderBlue), &includedUncertain.data()->x, (unsigned)includedUncertain.size(),
                 sizeof(tes::Vector3f));
    }

    if (!excludedOccupied.empty())
    {
      TES_POINTS(g_3es, TES_COLOUR(Orange), &excludedOccupied.data()->x, (unsigned)excludedOccupied.size(),
                 sizeof(tes::Vector3f));
    }

    if (!includedOccupied.empty())
    {
      TES_POINTS(g_3es, TES_COLOUR(LightSkyBlue), &includedOccupied.data()->x, (unsigned)includedOccupied.size(),
                 sizeof(tes::Vector3f));
    }
#endif  //  TES_ENABLE

    TES_SERVER_UPDATE(g_3es, 0.0f);

    return added;
  }

  // Wait for the given GPU queue to finish and migrate the data into query.
  unsigned finishGpuOperation(NearestNeighboursDetail &query, NearestNeighboursDetail::GpuData &gpu_data,
                              ClosestResult &closest)
  {
    unsigned added = 0;

    //---------------------------------
    const unsigned zero = 0u;
    gpu_data.gpu_result_count.fill(&zero, sizeof(zero));

    const OccupancyMapDetail &map = *query.map->detail();
    const glm::dvec3 near_point_local = query.near_point - map.origin;
    const gputil::float3 near_point_gpu = { float(near_point_local.x), float(near_point_local.y),
                                            float(near_point_local.z), 0 };
    const gputil::uchar3 voxel_dim_gpu = { map.region_voxel_dimensions.x, map.region_voxel_dimensions.y,
                                           map.region_voxel_dimensions.z };
    const gputil::float3 region_spatial_dim_gpu = { cl_float(map.region_spatial_dimensions.x),
                                                    cl_float(map.region_spatial_dimensions.y),
                                                    cl_float(map.region_spatial_dimensions.z) };

    gputil::Dim3 local_size(
      std::min<size_t>(gpu_data.nn_kernel.calculateOptimalWorkGroupSize(), gpu_data.queued_voxels));
    gputil::Dim3 global_size(gpu_data.queued_voxels);

    gputil::Event nn_kernel_event;

    int err;
    err = gpu_data.nn_kernel(global_size, local_size, nn_kernel_event, &gpu_data.queue,
                             // Kernel args.
                             voxel_dim_gpu, region_spatial_dim_gpu, gputil::BufferArg<float>(gpu_data.gpu_voxels),
                             gputil::BufferArg<gputil::short3>(gpu_data.gpu_voxel_region_keys),
                             gputil::BufferArg<gputil::uchar3>(gpu_data.gpu_voxel_voxel_keys),
                             gputil::BufferArg<float>(gpu_data.gpu_ranges),
                             gputil::BufferArg<gputil::short3>(gpu_data.gpu_result_region_keys),
                             gputil::BufferArg<gputil::uchar3>(gpu_data.gpu_result_voxel_keys),
                             gputil::BufferArg<gputil::uint1>(gpu_data.gpu_result_count), near_point_gpu,
                             float(query.search_radius), float(map.occupancy_threshold_value), float(map.resolution),
                             cl_int((query.query_flags & kQfUnknownAsOccupied) ? 1 : 0), gpu_data.queued_voxels
                             // , __local float *localRanges
                             // , __local short3 *localVoxelKeys
                             // , __local int3 *localRegionKeys
    );

    if (err)
    {
      return 0;
    }

#if SHOW_INFO
    global_size = local_size = gputil::Dim3(1);
    err = gpu_data.info_kernel(global_size, local_size, gputil::EventList({ nn_kernel_event }), nn_kernel_event,
                               &gpu_data.queue,
                               // Kernel args
                               gputil::BufferArg<gputil::uint>(gpu_data.gpu_result_count));

    if (err)
    {
      return 0;
    }
#endif  // SHOW_INFO

    // Block until the currently queued operations complete.
    nn_kernel_event.wait();

    gpu_data.gpu_result_count.read(&gpu_data.result_count, sizeof(gpu_data.result_count));
    if (gpu_data.result_count)
    {
      gpu_data.gpu_ranges.readElements(gpu_data.local_ranges.data(), gpu_data.result_count);
      gpu_data.gpu_result_region_keys.readElements(gpu_data.region_keys.data(), sizeof(*gpu_data.region_keys.data()),
                                                   gpu_data.result_count, 0, sizeof(gputil::short3));
      gpu_data.gpu_result_voxel_keys.readElements(gpu_data.local_keys.data(), sizeof(*gpu_data.local_keys.data()),
                                                  gpu_data.result_count, 0, sizeof(gputil::uchar3));

#ifndef VALIDATE_KEYS
      Key key;
      // Collate results.
      for (unsigned i = 0; i < gpu_data.result_count; ++i)
      {
        key.setRegionKey(glm::i16vec3(gpu_data.region_keys[i].x, gpu_data.region_keys[i].y, gpu_data.region_keys[i].z));
        key.setLocalAxis(0, gpu_data.local_keys[i].x);
        key.setLocalAxis(1, gpu_data.local_keys[i].y);
        key.setLocalAxis(2, gpu_data.local_keys[i].z);
        query.intersected_voxels.push_back(key);
        query.ranges.push_back(gpu_data.local_ranges[i]);

        if (gpu_data.local_ranges[i] * gpu_data.local_ranges[i] < closest.range)
        {
          closest.index = query.intersected_voxels.size();
          closest.range = gpu_data.local_ranges[i] * gpu_data.local_ranges[i];
        }
        ++added;
      }
#else   // VALIDATE_KEYS
      bool keyOk = true;
      unsigned nindex = 0;
      Key key;
      double range, expectedRange;
      // Collate results.
      for (unsigned i = 0; i < gpuData.resultCount; ++i)
      {
        // Validate keys are as expected when writing all keys out.
        ++added;
        key.setRegionKey(glm::i16vec3(gpuData.regionKeys[i].x, gpuData.regionKeys[i].y, gpuData.regionKeys[i].z));
        key.setLocalAxis(0, gpuData.localKeys[i].x);
        key.setLocalAxis(1, gpuData.localKeys[i].y);
        key.setLocalAxis(2, gpuData.localKeys[i].z);
        nindex = voxelIndex(key, query.map->regionVoxelDimensions());
        query.intersectedVoxels.push_back(key);
        keyOk = nindex == i;
        if (!keyOk)
        {
          std::cout << "Key failure [" << i << "]: R(" << int(key.regionKey().x) << ' ' << int(key.regionKey().y) << ' '
                    << int(key.regionKey().z) << ") V(" << int(key.localKey().x) << ' ' << int(key.localKey().y) << ' '
                    << int(key.localKey().z) << ") : " << gpuData.localRanges[i] << std::endl;
          nindex = voxelIndex(key, query.map->regionVoxelDimensions());
        }
        // Validate the range.
        range = gpuData.localRanges[i];
        expectedRange = glm::distance(query.near_point, query.map->voxelCentreGlobal(key));
        if (std::abs(range - expectedRange) > 1e-5)
        {
          std::cout << "  Range deviation. [" << i << "]: R(" << int(key.regionKey().x) << ' ' << int(key.regionKey().y)
                    << ' ' << int(key.regionKey().z) << ") V(" << int(key.localKey().x) << ' ' << int(key.localKey().y)
                    << ' ' << int(key.localKey().z) << ") : "
                    << "Expected/actual: " << expectedRange << ' ' << range << std::endl;
        }

        query.ranges.push_back(gpuData.localRanges[i]);

        if (gpuData.localRanges[i] * gpuData.localRanges[i] < closest.range)
        {
          closest.index = query.intersectedVoxels.size();
          closest.range = gpuData.localRanges[i] * gpuData.localRanges[i];
        }
      }
#endif  // VALIDATE_KEYS
    }

    gpu_data.clear();

    return added;
  }


  // Can handle splitting up regions which are larger than GpuBatchSize, but only when the
  // gpuData.queuedNodes size is zero on calling.
  bool regionNearestNeighboursGpu(const OccupancyMap &map, NearestNeighboursDetail &query,
                                  const glm::i16vec3 &region_key, ClosestResult &closest,
                                  NearestNeighboursDetail::GpuData &gpu_data)
  {
    const OccupancyMapDetail &data = *map.detail();
    const auto chunk_search = data.findRegion(region_key);
    const size_t voxels_volume =
      data.region_voxel_dimensions.x * data.region_voxel_dimensions.y * data.region_voxel_dimensions.z;

    gputil::short3 gpu_region_key = { region_key.x, region_key.y, region_key.z };

    if (chunk_search == data.chunks.end())
    {
      // The entire region is unknown space...
      if ((query.query_flags & ohm::kQfUnknownAsOccupied) == 0)
      {
        // ... and unknown space is considered free. No results to add.
        return false;
      }
    }

    unsigned pushed = 0;
    int kx, ky, kz;
    bool need_new_batch = false;
    // Used to push voxel keys.
    kx = ky = kz = 0;

    // Handle having voxel regions exceeding the allowed GPU buffer sizes.
    while (pushed < map.regionVoxelVolume())
    {
      const unsigned push_size = unsigned(std::min(voxels_volume - pushed, kGpuBatchSize));
      need_new_batch = false;
      if (chunk_search == data.chunks.end())
      {
        // The entire region is unknown space...
        // ... and we have to treat unknown space as occupied.
        // Fill the voxel buffer with zero to mark unknown space.
        const float occupied_voxel = data.occupancy_threshold_value;
        gpu_data.gpu_voxels.fillPartial(&occupied_voxel, sizeof(occupied_voxel), push_size * sizeof(occupied_voxel),
                                        gpu_data.queued_voxels * sizeof(occupied_voxel));
        gpu_data.gpu_voxel_region_keys.fillPartial(&gpu_region_key, sizeof(gpu_region_key),
                                                   push_size * sizeof(gpu_region_key),
                                                   gpu_data.queued_voxels * sizeof(gpu_region_key));
        map.regionCentreGlobal(region_key);
      }
      else
      {
        const MapChunk *chunk = chunk_search->second;
        const float *voxels = chunk->layout->layer(chunk->layout->occupancyLayer()).voxelsAs<float>(*chunk);

        gpu_data.gpu_voxels.write(voxels + pushed, push_size * sizeof(*voxels),
                                  gpu_data.queued_voxels * sizeof(*voxels));
        gpu_data.gpu_voxel_region_keys.fillPartial(&gpu_region_key, sizeof(gpu_region_key),
                                                   push_size * sizeof(gpu_region_key),
                                                   gpu_data.queued_voxels * sizeof(gpu_region_key));
      }

      gputil::uchar3 voxel_key;
      gputil::PinnedBuffer pinned(gpu_data.gpu_voxel_voxel_keys, gputil::kPinWrite);

      kx = pushed % map.regionVoxelDimensions().x;
      ky = (pushed / map.regionVoxelDimensions().x) % map.regionVoxelDimensions().y;
      kz = (pushed / (map.regionVoxelDimensions().x * map.regionVoxelDimensions().x)) % map.regionVoxelDimensions().z;
      for (int z = kz; z < map.regionVoxelDimensions().z; ++z)
      {
        kz = 0;
        voxel_key.z = z;
        for (int y = ky; y < map.regionVoxelDimensions().y; ++y)
        {
          ky = 0;
          voxel_key.y = y;
          for (int x = kx; x < map.regionVoxelDimensions().x; ++x)
          {
            kx = 0;
            voxel_key.x = x;
            pinned.write(&voxel_key, sizeof(voxel_key), gpu_data.queued_voxels * sizeof(voxel_key));
            ++gpu_data.queued_voxels;
            ++pushed;
            if (gpu_data.queued_voxels == kGpuBatchSize)
            {
              // Pushed enough to process.
              finishGpuOperation(query, gpu_data, closest);
              need_new_batch = true;
              break;
            }
          }

          if (need_new_batch)
          {
            // Pushed enough.
            break;
          }
        }
        if (need_new_batch)
        {
          // Pushed enough.
          break;
        }
      }
    }

    return true;
  }


  unsigned pushNodes(const OccupancyMap &map, NearestNeighboursDetail &query, const glm::i16vec3 &region_key,
                     ClosestResult &closest)
  {
    unsigned added = 0;
    NearestNeighboursDetail::GpuData &gpu_data = query.gpu_data;
    const size_t voxels_volume = map.regionVoxelVolume();

#ifdef VALIDATE_KEYS
    // Always complete previous queued operation when validating keys. This is to allow
    // us to infer each voxel index by its position in the results array.
    if (gpuData.queuedNodes)
#else   // VALIDATE_KEYS
    if (gpu_data.queued_voxels && gpu_data.queued_voxels + voxels_volume > kGpuBatchSize)
#endif  // VALIDATE_KEYS
    {
      // Complete existing queue.
      added = finishGpuOperation(query, gpu_data, closest);
    }

    regionNearestNeighboursGpu(map, query, region_key, closest, gpu_data);
    return added;
  }


  unsigned occupancyNearestNeighboursGpu(OccupancyMap &map, NearestNeighboursDetail &query, ClosestResult &closest,
                                         const glm::dvec3 &min_extents, const glm::dvec3 &max_extents)
  {
    unsigned added_neighbours = 0;
    const std::function<unsigned(OccupancyMap &, NearestNeighboursDetail &, const glm::i16vec3 &, ClosestResult &)>
      rnn_gpu_func = pushNodes;

    added_neighbours += ohm::occupancyQueryRegions(map, query, closest, min_extents, max_extents, rnn_gpu_func);

    // Finalise queues.
    NearestNeighboursDetail::GpuData &gpu_data = query.gpu_data;
    if (gpu_data.queued_voxels)
    {
      added_neighbours += finishGpuOperation(query, gpu_data, closest);
    }

    return added_neighbours;
  }
}  // namespace


NearestNeighbours::NearestNeighbours(NearestNeighboursDetail *detail)
  : Query(detail)
{
  // Can't initalise GPU until onSetMap() (need a valid map).
}


NearestNeighbours::NearestNeighbours(OccupancyMap &map, const glm::dvec3 &near_point, float search_radius,
                                     unsigned query_flags)
  : Query(new NearestNeighboursDetail)
  , query_flags_(query_flags)
{
  setMap(&map);
  setNearPoint(near_point);
  setSearchRadius(search_radius);
  setQueryFlags(query_flags);

  initialiseGpu(*map.detail(), *imp());
}


NearestNeighbours::~NearestNeighbours()
{
  NearestNeighboursDetail *d = imp();
  if (d)
  {
    releaseGpu(*d);
  }
  delete d;
  imp_ = nullptr;
}


glm::dvec3 NearestNeighbours::nearPoint() const
{
  const NearestNeighboursDetail *d = imp();
  return d->near_point;
}


void NearestNeighbours::setNearPoint(const glm::dvec3 &point)
{
  NearestNeighboursDetail *d = imp();
  d->near_point = point;
}


float NearestNeighbours::searchRadius() const
{
  const NearestNeighboursDetail *d = imp();
  return d->search_radius;
}


void NearestNeighbours::setSearchRadius(float range)
{
  NearestNeighboursDetail *d = imp();
  d->search_radius = range;
}


void NearestNeighbours::onSetMap()
{
  NearestNeighboursDetail *d = imp();
  if (d->map)
  {
    initialiseGpu(*d->map->detail(), *d);
  }
}


bool NearestNeighbours::onExecute()
{
  NearestNeighboursDetail *d = imp();

  if (!d->map)
  {
    return false;
  }

  ClosestResult closest;

  const glm::dvec3 min_extents = d->near_point - glm::dvec3(d->search_radius, d->search_radius, d->search_radius);
  const glm::dvec3 max_extents = d->near_point + glm::dvec3(d->search_radius, d->search_radius, d->search_radius);

  // Create debug visualisation objects. We use the map address to persist objects.
  // Search sphere.
  TES_SPHERE_W(g_3es, TES_COLOUR_A(GreenYellow, 128), uint32_t((size_t)d->map), glm::value_ptr(d->near_point),
               d->search_radius);
  // Search bounds.
  TES_BOX_W(g_3es, TES_COLOUR(FireBrick), uint32_t((size_t)d->map), glm::value_ptr(0.5 * min_extents + max_extents),
            glm::value_ptr(max_extents - min_extents));

  if (d->gpu_ok && (d->query_flags & kQfGpuEvaluate))
  {
    occupancyNearestNeighboursGpu(*d->map, *d, closest, min_extents, max_extents);
  }
  else
  {
    ohm::occupancyQueryRegions(*d->map, *d, closest, min_extents, max_extents, &regionNearestNeighboursCpu);
  }

  TES_SERVER_UPDATE(g_3es, 0.0f);
  // Delete debug objects.
  TES_SPHERE_END(g_3es, uint32_t((size_t)d->map));
  TES_BOX_END(g_3es, uint32_t((size_t)d->map));

  if ((d->query_flags & kQfNearestResult) && !d->intersected_voxels.empty())
  {
    d->intersected_voxels[0] = d->intersected_voxels[closest.index];
    d->intersected_voxels.resize(1);
    d->ranges[0] = d->ranges[closest.index];
    d->number_of_results = 1u;
    d->ranges.resize(1);
  }
  else
  {
    d->number_of_results = d->intersected_voxels.size();
  }

  return true;
}


bool NearestNeighbours::onExecuteAsync()
{
  return false;
}


void NearestNeighbours::onReset(bool /*hard_reset*/)
{
  // NearestNeighboursDetail *d = imp();
}


NearestNeighboursDetail *NearestNeighbours::imp()
{
  return static_cast<NearestNeighboursDetail *>(imp_);
}


const NearestNeighboursDetail *NearestNeighbours::imp() const
{
  return static_cast<const NearestNeighboursDetail *>(imp_);
}
