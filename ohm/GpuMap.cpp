// Copyright (c) 2017
// Commonwealth Scientific and Industrial Research
// Organisation (CSIRO) ABN 41 687 119 230
//
// Author: Kazys Stepanas
#include "GpuMap.h"

#include "DefaultLayers.h"
#include "GpuCache.h"
#include "GpuLayerCache.h"
#include "GpuTransformSamples.h"
#include "MapChunk.h"
#include "MapRegion.h"
#include "OccupancyMap.h"
#include "OccupancyUtil.h"

#include "private/GpuMapDetail.h"
#include "private/OccupancyMapDetail.h"

#include <gputil/gpuBuffer.h>
#include <gputil/gpuEvent.h>
#include <gputil/gpuPinnedBuffer.h>
#include <gputil/gpuPlatform.h>

#include <glm/ext.hpp>

#include <cassert>
#include <functional>
#include <initializer_list>
#include <iostream>


#define DEBUG_RAY 0

#if DEBUG_RAY
#pragma optimize("", off)
#endif  // DEBUG_RAY

using namespace ohm;

namespace
{
  typedef std::function<void(const glm::i16vec3 &, const glm::dvec3 &, const glm::dvec3 &)> RegionWalkFunction;

  void walkRegions(const OccupancyMap &map, const glm::dvec3 &start_point, const glm::dvec3 &end_point,
                   const RegionWalkFunction &func)
  {
    // see "A Faster Voxel Traversal Algorithm for Ray
    // Tracing" by Amanatides & Woo
    const glm::i16vec3 start_point_key = map.regionKey(start_point);
    const glm::i16vec3 end_point_key = map.regionKey(end_point);
    const glm::dvec3 start_point_local = glm::vec3(start_point - map.origin());
    const glm::dvec3 end_point_local = glm::vec3(end_point - map.origin());

    glm::dvec3 direction = glm::vec3(end_point - start_point);
    double length = glm::dot(direction, direction);
    length = (length >= 1e-6) ? std::sqrt(length) : 0;
    direction *= 1.0 / length;

    if (start_point_key == end_point_key)
    {
      func(start_point_key, start_point, end_point);
      return;
    }

    int step[3] = { 0 };
    glm::dvec3 region;
    double time_max[3];
    double time_delta[3];
    double time_limit[3];
    double next_region_border;
    double direction_axis_inv;
    const glm::dvec3 region_resolution = map.regionSpatialResolution();
    glm::i16vec3 current_key = start_point_key;

    region = map.regionCentreLocal(current_key);

    // Compute step direction, increments and maximums along
    // each axis.
    for (unsigned i = 0; i < 3; ++i)
    {
      if (direction[i] != 0)
      {
        direction_axis_inv = 1.0 / direction[i];
        step[i] = (direction[i] > 0) ? 1 : -1;
        // Time delta is the ray time between voxel
        // boundaries calculated for each axis.
        time_delta[i] = region_resolution[i] * std::abs(direction_axis_inv);
        // Calculate the distance from the origin to the
        // nearest voxel edge for this axis.
        next_region_border = region[i] + step[i] * 0.5f * region_resolution[i];
        time_max[i] = (next_region_border - start_point_local[i]) * direction_axis_inv;
        time_limit[i] =
          std::abs((end_point_local[i] - start_point_local[i]) * direction_axis_inv);  // +0.5f *
                                                                                       // regionResolution[i];
      }
      else
      {
        time_max[i] = time_delta[i] = std::numeric_limits<double>::max();
        time_limit[i] = 0;
      }
    }

    bool limit_reached = false;
    int axis;
    while (!limit_reached && current_key != end_point_key)
    {
      func(current_key, start_point, end_point);

      if (time_max[0] < time_max[2])
      {
        axis = (time_max[0] < time_max[1]) ? 0 : 1;
      }
      else
      {
        axis = (time_max[1] < time_max[2]) ? 1 : 2;
      }

      limit_reached = std::abs(time_max[axis]) > time_limit[axis];
      current_key[axis] += step[axis];
      time_max[axis] += time_delta[axis];
    }

    // Touch the last region.
    func(current_key, start_point, end_point);
  }

  inline bool goodRay(const glm::dvec3 &start, const glm::dvec3 &end, double max_range = 500.0)
  {
    bool is_good = true;
    if (glm::any(glm::isnan(start)))
    {
      // std::cerr << "NAN start point" << std::endl;
      is_good = false;
    }
    if (glm::any(glm::isnan(end)))
    {
      // std::cerr << "NAN end point" << std::endl;
      is_good = false;
    }

    const glm::dvec3 ray = end - start;
    if (max_range && glm::dot(ray, ray) > max_range * max_range)
    {
      // std::cerr << "Ray too long: (" <<
      // glm::distance(start, end) << "): " << start << " ->
      // " << end << std::endl;
      is_good = false;
    }

    return is_good;
  }
}  // namespace

// GPU related functions.
namespace ohm
{
  int initialiseRegionUpdateGpu(gputil::Device &gpu);
  void releaseRegionUpdateGpu();

  int updateRegion(gputil::Queue &queue, gputil::Buffer &chunk_mem, gputil::Buffer &region_key_buffer,
                   gputil::Buffer &region_offset_buffer, unsigned region_count, gputil::Buffer &ray_mem,
                   unsigned ray_count, const glm::ivec3 &region_voxel_dimensions, double voxel_resolution,
                   float adjust_miss, float adjust_hit, float min_voxel_value, float max_voxel_value,
                   std::initializer_list<gputil::Event> events, gputil::Event *completion_event);
}  // namespace ohm


const double GpuMap::kDefaultMaxRange = 500.0;


GpuCache *ohm::gpumap::enableGpu(OccupancyMap &map)
{
  return enableGpu(map, GpuCache::kDefaultLayerMemSize, true);
}


GpuCache *ohm::gpumap::enableGpu(OccupancyMap &map, size_t layer_gpu_mem_size, bool mappable_buffers)
{
  OccupancyMapDetail &map_imp = *map.detail();
  if (map_imp.gpu_cache)
  {
    return map_imp.gpu_cache;
  }

  if (layer_gpu_mem_size == 0)
  {
    layer_gpu_mem_size = GpuCache::kDefaultLayerMemSize;
  }

  initialiseGpuCache(map, layer_gpu_mem_size, mappable_buffers);
  return map_imp.gpu_cache;
}


void ohm::gpumap::sync(OccupancyMap &map)
{
  if (GpuCache *cache = gpuCache(map))
  {
    for (unsigned i = 0; i < cache->layerCount(); ++i)
    {
      if (GpuLayerCache *layer = cache->layerCache(i))
      {
        layer->syncToMainMemory();
      }
    }
  }
}


void ohm::gpumap::sync(OccupancyMap &map, unsigned layer_index)
{
  if (GpuCache *cache = gpuCache(map))
  {
    if (GpuLayerCache *layer = cache->layerCache(layer_index))
    {
      layer->syncToMainMemory();
    }
  }
}


GpuCache *ohm::gpumap::gpuCache(OccupancyMap &map)
{
  return map.detail()->gpu_cache;
}


GpuMap::GpuMap(OccupancyMap *map, bool borrowed_map, unsigned expected_point_count, size_t gpu_mem_size)
  : imp_(new GpuMapDetail(map, borrowed_map))
{
  gpumap::enableGpu(*map, gpu_mem_size, true);
  GpuCache &gpu_cache = *map->detail()->gpu_cache;
  imp_->gpu_ok = initialiseRegionUpdateGpu(gpu_cache.gpu()) == 0;
  const unsigned prealloc_region_count = 1024u;
  for (unsigned i = 0; i < GpuMapDetail::kBuffersCount; ++i)
  {
    imp_->ray_buffers[i] =
      gputil::Buffer(gpu_cache.gpu(), sizeof(gputil::float3) * expected_point_count, gputil::kBfReadHost);
    imp_->region_key_buffers[i] =
      gputil::Buffer(gpu_cache.gpu(), sizeof(gputil::int3) * prealloc_region_count, gputil::kBfReadHost);
    imp_->region_offset_buffers[i] =
      gputil::Buffer(gpu_cache.gpu(), sizeof(gputil::ulong1) * prealloc_region_count, gputil::kBfReadHost);
  }
  imp_->max_range_filter = kDefaultMaxRange;
  imp_->transform_samples = new GpuTransformSamples(gpu_cache.gpu());
}


GpuMap::~GpuMap()
{
  releaseRegionUpdateGpu();
  delete imp_;
}


bool GpuMap::gpuOk() const
{
  return imp_->gpu_ok;
}


OccupancyMap &GpuMap::map()
{
  return *imp_->map;
}


const OccupancyMap &GpuMap::map() const
{
  return *imp_->map;
}


bool GpuMap::borrowedMap() const
{
  return imp_->borrowed_map;
}


double GpuMap::maxRangeFilter() const
{
  return imp_->max_range_filter;
}


void GpuMap::setMaxRangeFilter(double range)
{
  imp_->max_range_filter = range;
}


void GpuMap::syncOccupancy()
{
  if (imp_->map)
  {
    gpumap::sync(*imp_->map, kGcIdOccupancy);
  }
}


unsigned GpuMap::integrateRays(const glm::dvec3 *rays, unsigned point_count, bool end_points_as_occupied)
{
  // Wait for previous ray operations to complete.
  const int buf_idx = imp_->next_buffers_index;
  waitOnPreviousOperation(buf_idx);
  return integrateRaysT<glm::dvec3>(imp_->ray_buffers[imp_->next_buffers_index],
                                    imp_->ray_upload_events[imp_->next_buffers_index], rays, point_count, false,
                                    end_points_as_occupied);
}


unsigned GpuMap::integrateRays(gputil::Buffer &buffer, unsigned point_count, bool end_points_as_occupied)
{
  // Copy results into transformed_rays.
  gputil::PinnedBuffer ray_buffer(buffer, gputil::kPinRead);
  imp_->transformed_rays.resize(point_count);
  ray_buffer.readElements<gputil::float3>(imp_->transformed_rays.data(), point_count);
  ray_buffer.unpin();

  // Preloaded buffer. Not waiting on any event.
  gputil::Event dummy_event;
  return integrateRaysT(buffer, dummy_event, imp_->transformed_rays.data(), point_count, true, end_points_as_occupied);
}


unsigned GpuMap::integrateRays(gputil::Buffer &buffer, const glm::vec3 *rays, unsigned point_count,
                               bool end_points_as_occupied)
{
  // Preloaded buffer. Not waiting on any event.
  gputil::Event dummy_event;
  return integrateRaysT(buffer, dummy_event, rays, point_count, true, end_points_as_occupied);
}


unsigned GpuMap::integrateRays(gputil::Buffer &buffer, const glm::vec4 *rays, unsigned point_count,
                               bool end_points_as_occupied)
{
  // Preloaded buffer. Not waiting on any event.
  gputil::Event dummy_event;
  return integrateRaysT(buffer, dummy_event, rays, point_count, true, end_points_as_occupied);
}


unsigned GpuMap::integrateRays(gputil::Buffer &buffer, const glm::dvec3 *rays, unsigned point_count,
                               bool end_points_as_occupied)
{
  // Preloaded buffer. Not waiting on any event.
  gputil::Event dummy_event;
  return integrateRaysT(buffer, dummy_event, rays, point_count, true, end_points_as_occupied);
}


unsigned GpuMap::integrateLocalRays(const double *transform_times, const glm::dvec3 *transform_translations,
                                    const glm::dquat *transform_rotations, unsigned transform_count,
                                    const double *sample_times, const glm::dvec3 *local_samples, unsigned point_count,
                                    bool end_points_as_occupied)
{
  if (!imp_->map)
  {
    return 0u;
  }

  if (!imp_->gpu_ok)
  {
    return 0u;
  }

  OccupancyMap &map = *imp_->map;
  GpuCache *gpu_cache = gpumap::enableGpu(map);

  if (!gpu_cache)
  {
    return 0u;
  }

  if (point_count == 0 || transform_count == 0)
  {
    return 0u;
  }

  // Wait for previous ray operations to complete.
  const int buf_idx = imp_->next_buffers_index;
  waitOnPreviousOperation(buf_idx);

  unsigned upload_count = imp_->transform_samples->transform(
    transform_times, transform_translations, transform_rotations, transform_count, sample_times, local_samples,
    point_count, gpu_cache->gpuQueue(), imp_->ray_buffers[buf_idx], imp_->ray_upload_events[buf_idx],
    imp_->max_range_filter);

  if (upload_count == 0)
  {
    return 0u;
  }

  // Integrate rays from pre-existing buffer.
  return integrateRays(imp_->ray_buffers[buf_idx], upload_count * 2, end_points_as_occupied);
}


GpuCache *GpuMap::gpuCache() const
{
  return imp_->map->detail()->gpu_cache;
}


template <typename VEC_TYPE>
unsigned GpuMap::integrateRaysT(gputil::Buffer &buffer, gputil::Event &buffer_event, const VEC_TYPE *rays,
                                unsigned point_count, bool preloaded_buffer, bool end_points_as_occupied)
{
  if (!imp_->map)
  {
    return 0u;
  }

  if (!imp_->gpu_ok)
  {
    return 0u;
  }

  OccupancyMap &map = *imp_->map;
  GpuCache *gpu_cache = gpumap::enableGpu(map);

  if (!gpu_cache)
  {
    return 0u;
  }

  if (point_count == 0)
  {
    return 0u;
  }

  // Resolve the buffer index to use. We need to support cases where buffer is already one fo the imp_->ray_buffers.
  // Check this first.
  // We still need a buffer index for event tracking.
  int buf_idx = -1;
  if (&buffer == &imp_->ray_buffers[0])
  {
    buf_idx = 0;
  }
  else if (&buffer == &imp_->ray_buffers[1])
  {
    buf_idx = 1;
  }
  else
  {
    // External buffer provided. Wait for previous operation to complete.
    // Wait for previous ray operations to complete.
    buf_idx = imp_->next_buffers_index;
    waitOnPreviousOperation(buf_idx);
  }

  // Touch the map.
  map.touch();

  // Get the GPU cache.
  GpuLayerCache &layer_cache = *gpu_cache->layerCache(kGcIdOccupancy);
  imp_->batch_marker = layer_cache.beginBatch();

  // Region walking function tracking which regions are
  // affected by a ray.
  const auto region_func = [this](const glm::i16vec3 &region_key, const glm::dvec3 & /*origin*/,
                                  const glm::dvec3 & /*sample*/) {
    const unsigned region_hash = MapRegion::Hash::calculate(region_key);
    if (imp_->findRegion(region_hash, region_key) == imp_->regions.end())
    {
      imp_->regions.insert(std::make_pair(region_hash, region_key));
    }
  };

  // Reserve GPU memory for the rays.
  if (!preloaded_buffer)
  {
    buffer.resize(sizeof(gputil::float3) * point_count);
  }

  gputil::PinnedBuffer ray_buffer;
  glm::vec4 ray_start, ray_end;

  if (!preloaded_buffer)
  {
    ray_buffer = gputil::PinnedBuffer(buffer, gputil::kPinWrite);
  }


  // TODO: break up long lines. Requires the kernel knows which are real end points and which aren't.
  // Build region set and upload rays.
  imp_->regions.clear();
  unsigned upload_count = 0u;
  for (unsigned i = 0; i < point_count; i += 2)
  {
    ray_start = glm::vec4(glm::vec3(rays[i + 0]), 0);
    ray_end = glm::vec4(glm::vec3(rays[i + 1]), 0);
    if (!goodRay(ray_start, ray_end, imp_->max_range_filter))
    {
      continue;
    }

    // Upload if not preloaded.
    if (!preloaded_buffer)
    {
      ray_buffer.write(glm::value_ptr(ray_start), sizeof(glm::vec3), (upload_count + 0) * sizeof(gputil::float3));
      ray_buffer.write(glm::value_ptr(ray_end), sizeof(glm::vec3), (upload_count + 1) * sizeof(gputil::float3));
      upload_count += 2;
    }

    // std::cout << i / 2 << ' ' <<
    // imp_->map->voxelKey(rays[i + 0]) << " -> " <<
    // imp_->map->voxelKey(rays[i + 1])
    //          << "  <=>  " << rays[i + 0] << " -> " <<
    //          rays[i + 1] << std::endl;
    walkRegions(*imp_->map, rays[i + 0], rays[i + 1], region_func);
  }

  if (preloaded_buffer)
  {
    upload_count = point_count;
    // Asynchronous unpin. Kernels will wait on the associated event.
    ray_buffer.unpin(&layer_cache.gpuQueue(), nullptr, &buffer_event);
    imp_->ray_upload_events[buf_idx] = buffer_event;
  }

  imp_->ray_counts[buf_idx] = unsigned(upload_count / 2);

  if (upload_count == 0)
  {
    return 0u;
  }

  // Size the region buffers.
  imp_->region_key_buffers[buf_idx].elementsResize<gputil::int3>(imp_->regions.size());
  imp_->region_offset_buffers[buf_idx].elementsResize<gputil::ulong1>(imp_->regions.size());

  // Execute on each region.
  gputil::PinnedBuffer regions_buffer(imp_->region_key_buffers[buf_idx], gputil::kPinWrite);
  gputil::PinnedBuffer offsets_buffer(imp_->region_offset_buffers[buf_idx], gputil::kPinWrite);
  // Note: bufIdx may have change when calling
  // enqueueRegion. Do not use after this point.
  for (auto &&region_iter : imp_->regions)
  {
    enqueueRegion(region_iter.first, region_iter.second, regions_buffer, offsets_buffer, end_points_as_occupied, true);
  }

  finaliseBatch(regions_buffer, offsets_buffer, end_points_as_occupied);

  return upload_count;
}


void GpuMap::waitOnPreviousOperation(int buffer_index)
{
  // Wait first on the event known to complete last.
  imp_->region_update_events[buffer_index].wait();
  imp_->region_update_events[buffer_index].release();

  imp_->ray_upload_events[buffer_index].wait();
  imp_->ray_upload_events[buffer_index].release();

  imp_->region_key_upload_events[buffer_index].wait();
  imp_->region_key_upload_events[buffer_index].release();

  imp_->region_offset_upload_events[buffer_index].wait();
  imp_->region_offset_upload_events[buffer_index].release();
}


void GpuMap::enqueueRegion(unsigned region_hash, const glm::i16vec3 &region_key, gputil::PinnedBuffer &regions_buffer,
                           gputil::PinnedBuffer &offsets_buffer, bool end_points_as_occupied, bool allow_retry)
{
  // Upload chunk to GPU.
  MapChunk *chunk = nullptr;
  gputil::Event upload_event;
  gputil::ulong1 mem_offset;
  GpuLayerCache::CacheStatus status;

  int buf_idx = imp_->next_buffers_index;
  GpuCache &gpu_cache = *this->gpuCache();
  GpuLayerCache &layer_cache = *gpu_cache.layerCache(kGcIdOccupancy);
  mem_offset = gputil::ulong1(layer_cache.upload(*imp_->map, region_key, chunk, &upload_event, &status,
                                                 imp_->batch_marker, GpuLayerCache::kAllowRegionCreate));

  if (status != GpuLayerCache::kCacheFull)
  {
    // std::cout << "region: [" << regionKey.x << ' ' <<
    // regionKey.y << ' ' << regionKey.z << ']' <<
    // std::endl;
    gputil::int3 gpu_region_key = { region_key.x, region_key.y, region_key.z };
    regions_buffer.write(&gpu_region_key, sizeof(gpu_region_key),
                         imp_->region_counts[buf_idx] * sizeof(gpu_region_key));
    offsets_buffer.write(&mem_offset, sizeof(mem_offset), imp_->region_counts[buf_idx] * sizeof(mem_offset));
    ++imp_->region_counts[buf_idx];
  }
  else if (allow_retry)
  {
    const int previous_buf_idx = buf_idx;
    finaliseBatch(regions_buffer, offsets_buffer, end_points_as_occupied);

    // Repin these buffers, but the index has changed.
    const unsigned regions_processed = imp_->region_counts[buf_idx];
    buf_idx = imp_->next_buffers_index;
    waitOnPreviousOperation(buf_idx);

    // Copy the rays buffer from the batch we just
    // finalised.
    gputil::copyBuffer(imp_->ray_buffers[buf_idx], imp_->ray_buffers[previous_buf_idx], &gpu_cache.gpuQueue(), nullptr,
                       &imp_->ray_upload_events[previous_buf_idx]);
    imp_->ray_counts[buf_idx] = imp_->ray_counts[previous_buf_idx];

    // This statement should always be true, but it would be
    // bad to underflow.
    if (regions_processed < imp_->regions.size())
    {
      // Size the region buffers.
      imp_->region_key_buffers[buf_idx].gputil::Buffer::elementsResize<gputil::int3>(imp_->regions.size() -
                                                                                     regions_processed);
      imp_->region_offset_buffers[buf_idx].gputil::Buffer::elementsResize<gputil::ulong1>(imp_->regions.size() -
                                                                                          regions_processed);

      regions_buffer = gputil::PinnedBuffer(imp_->region_key_buffers[buf_idx], gputil::kPinRead);
      offsets_buffer = gputil::PinnedBuffer(imp_->region_offset_buffers[buf_idx], gputil::kPinRead);

      // Try again, but don't allow retry.
      enqueueRegion(region_hash, region_key, regions_buffer, offsets_buffer, end_points_as_occupied, false);
    }
  }

  // Mark the region as dirty.
  chunk->dirty_stamp = chunk->touched_stamps[kDlOccupancy] = imp_->map->stamp();
}


void GpuMap::finaliseBatch(gputil::PinnedBuffer &regions_buffer, gputil::PinnedBuffer &offsets_buffer,
                           bool end_points_as_occupied)
{
  const int buf_idx = imp_->next_buffers_index;
  const OccupancyMapDetail *map = imp_->map->detail();

  // Complete region data upload.
  GpuCache &gpu_cache = *this->gpuCache();
  GpuLayerCache &layer_cache = *gpu_cache.layerCache(kGcIdOccupancy);
  regions_buffer.unpin(&layer_cache.gpuQueue(), nullptr, &imp_->region_key_upload_events[buf_idx]);
  offsets_buffer.unpin(&layer_cache.gpuQueue(), nullptr, &imp_->region_offset_upload_events[buf_idx]);

  // Enqueue update kernel.
  updateRegion(layer_cache.gpuQueue(), *layer_cache.buffer(), imp_->region_key_buffers[buf_idx],
               imp_->region_offset_buffers[buf_idx], imp_->region_counts[buf_idx], imp_->ray_buffers[buf_idx],
               imp_->ray_counts[buf_idx], map->region_voxel_dimensions, map->resolution, map->miss_value,
               (end_points_as_occupied) ? map->hit_value : map->miss_value, map->min_node_value, map->max_node_value,
               { imp_->ray_upload_events[buf_idx], imp_->region_key_upload_events[buf_idx],
                 imp_->region_offset_upload_events[buf_idx] },
               &imp_->region_update_events[buf_idx]);
  // layerCache.gpuQueue().flush();

  // Update most recent chunk GPU event.
  layer_cache.updateEvents(imp_->batch_marker, imp_->region_update_events[buf_idx]);
  // layerCache.updateEvent(*chunk, updateEvent);

  // std::cout << imp_->region_counts[bufIdx] << "
  // regions\n" << std::flush;

  imp_->region_counts[buf_idx] = 0;
  // Cycle odd numbers to avoid zero which is a special case
  // value.
  imp_->batch_marker = layer_cache.beginBatch();
  imp_->next_buffers_index = 1 - imp_->next_buffers_index;
}
