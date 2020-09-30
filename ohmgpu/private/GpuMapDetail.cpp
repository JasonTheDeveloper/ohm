// Copyright (c) 2018
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#include "GpuMapDetail.h"

#include "GpuCache.h"
#include "GpuCacheParams.h"
#include "GpuMap.h"
#include "GpuTransformSamples.h"

#include <ohm/DefaultLayer.h>
#include <ohm/MapLayer.h>
#include <ohm/OccupancyMap.h>
#include <ohm/private/OccupancyMapDetail.h>

#include <gputil/gpuDevice.h>

#include <map>

using namespace ohm;

namespace
{
  void onOccupancyLayerChunkSync(MapChunk *chunk, const glm::u8vec3 &region_dimensions)
  {
    chunk->searchAndUpdateFirstValid(region_dimensions);
  }
}  // namespace

GpuMapDetail::~GpuMapDetail()
{
  if (!borrowed_map)
  {
    delete map;
  }
}

GpuCache *ohm::initialiseGpuCache(OccupancyMap &map, size_t target_gpu_mem_size, unsigned flags)
{
  OccupancyMapDetail *detail = map.detail();
  GpuCache *gpu_cache = static_cast<GpuCache *>(detail->gpu_cache);
  if (!gpu_cache)
  {
    target_gpu_mem_size = (target_gpu_mem_size) ? target_gpu_mem_size : GpuCache::kDefaultTargetMemSize;
    gpu_cache = new GpuCache(map, target_gpu_mem_size, flags);
    detail->gpu_cache = gpu_cache;

    reinitialiseGpuCache(gpu_cache, map, flags);
  }

  return gpu_cache;
}


void ohm::reinitialiseGpuCache(GpuCache *gpu_cache, OccupancyMap &map, unsigned flags)
{
  if (gpu_cache)
  {
    gpu_cache->clear();
    gpu_cache->removeLayers();

    // Create default layers.
    unsigned mappable_flag = 0;
    if (flags & gpumap::kGpuForceMappedBuffers)
    {
      mappable_flag |= kGcfMappable;
    }
    else if (flags & gpumap::kGpuAllowMappedBuffers)
    {
      // Use mapped buffers if device has unified host memory.
      if (gpu_cache->gpu().unifiedMemory())
      {
        mappable_flag |= kGcfMappable;
      }
    }

    // Setup known layers.
    const int occupancy_layer = map.layout().occupancyLayer();
    const int mean_layer = map.layout().meanLayer();
    const int covariance_layer = map.layout().covarianceLayer();
    const int clearance_layer = map.layout().clearanceLayer();
    std::vector<int> known_layers = { occupancy_layer, mean_layer, covariance_layer, clearance_layer };

    // Calculate the relative layer memory sizes.
    std::map<int, size_t> layer_mem_weight;
    size_t total_mem_weight = 0;

    for (int layer_index : known_layers)
    {
      if (layer_index >= 0)
      {
        const size_t layer_size = map.layout().layer(layer_index).layerByteSize(map.regionVoxelDimensions());
        layer_mem_weight[layer_index] = layer_size;
        total_mem_weight += layer_size;
      }
    }

    // Distribute the target member size.
    for (auto &layer_weight : layer_mem_weight)
    {
      // Logic is: layer_mem = target mem * (layer_weight / total_weight)
      layer_weight.second = layer_weight.second * gpu_cache->targetGpuLayerSize() / total_mem_weight;
    }

    if (occupancy_layer >= 0)
    {
      gpu_cache->createCache(kGcIdOccupancy,
                             // On sync, ensure the first valid voxel is updated.
                             GpuLayerCacheParams{ layer_mem_weight[occupancy_layer], occupancy_layer,
                                                  kGcfRead | kGcfWrite | mappable_flag, &onOccupancyLayerChunkSync });
    }

    // Initialise the voxel mean layer.
    if (mean_layer >= 0)
    {
      gpu_cache->createCache(kGcIdVoxelMean, GpuLayerCacheParams{ layer_mem_weight[mean_layer], mean_layer,
                                                                  kGcfRead | kGcfWrite | mappable_flag });
    }

    if (covariance_layer >= 0)
    {
      // TODO: (KS) add the write flag if we move to being able to process the samples on GPU too.
      gpu_cache->createCache(kGcIdCovariance, GpuLayerCacheParams{ layer_mem_weight[covariance_layer], covariance_layer,
                                                                   kGcfRead | kGcfWrite | mappable_flag });
    }

    // Note: we create the clearance gpu cache if we have a clearance layer, but it caches the occupancy_layer as that
    // is the information it reads.
    if (clearance_layer >= 0)
    {
      // Use of occupancy_layer below is correct. See comment above.
      gpu_cache->createCache(kGcIdClearance, GpuLayerCacheParams{ layer_mem_weight[clearance_layer], occupancy_layer,
                                                                  kGcfRead | mappable_flag });
    }
  }
}
