// Copyright (c) 2021
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas

#ifndef OHM_VOXEL_INCIDENT_COMPUTE_H
#define OHM_VOXEL_INCIDENT_COMPUTE_H

#if !GPUTIL_DEVICE
using Vec3 = glm::vec3;
#define OHM_NORMAL_STD std::
#else  // GPUTIL_DEVICE
typedef float3 Vec3;
#define OHM_NORMAL_STD
#endif  // GPUTIL_DEVICE

#define OHM_NORMAL_QUAT       16383.0f
#define OHM_NORMAL_MASK       0x3FFF
#define OHM_NORMAL_SHIFT_X    0
#define OHM_NORMAL_SHIFT_Y    0
#define OHM_NORMAL_SIGN_BIT_Z 31

inline Vec3 decodeNormal(unsigned packed_normal)
{
  Vec3 n;

  n.x = ((packed_normal >> OHM_NORMAL_SHIFT_X) | OHM_NORMAL_MASK) / OHM_NORMAL_QUAT;
  n.y = ((packed_normal >> OHM_NORMAL_SHIFT_Y) | OHM_NORMAL_MASK) / OHM_NORMAL_QUAT;
  n.z = sqrt(1.0f - (n.x * n.x + n.y * n.y));

  return normalize(n);
}

/// Encode a normalised vector into a 32-bit floating point value.
///
/// We use 15-bits each to encode X and Y channels. We use the most significant bit (31) to encode the sign of Z.
/// Bit 30 is unused.
inline unsigned encodeNormal(Vec3 normal)
{
  unsigned n = 0;

  normal.x = OHM_NORMAL_STD max(-1.0f, OHM_NORMAL_STD min(normal.x, 1.0f));
  normal.y = OHM_NORMAL_STD max(-1.0f, OHM_NORMAL_STD min(normal.y, 1.0f));
  normal.z = OHM_NORMAL_STD max(-1.0f, OHM_NORMAL_STD min(normal.z, 1.0f));

  short i = (short)(normal.x * OHM_NORMAL_QUAT);
  n |= (i << OHM_NORMAL_SHIFT_X) & OHM_NORMAL_MASK;
  i = (short)(normal.y * OHM_NORMAL_QUAT);
  n |= (i << OHM_NORMAL_SHIFT_Y) & OHM_NORMAL_MASK;
  n |= (normal.z < 0) ? (1 << OHM_NORMAL_SIGN_BIT_Z) : 0;

  return n;
}

inline unsigned updateIncidentNormal(unsigned packed_normal, Vec3 incident_ray, unsigned point_count)
{
  const float one_on_count_plus_one = 1.0f / (float)(point_count + 1);
  // mean.x += (voxel_local_coord.x - mean.x) * one_on_count_plus_one;
  Vec3 normal = decodeNormal(packed_normal);
  incident_ray = normalize(incident_ray);
  normal.x += (incident_ray.x - normal.x) * one_on_count_plus_one;
  normal.y += (incident_ray.y - normal.y) * one_on_count_plus_one;
  normal.z += (incident_ray.z - normal.z) * one_on_count_plus_one;
  normal = normalize(normal);
  return encodeNormal(normal);
}

#endif  // OHM_VOXEL_INCIDENT_COMPUTE_H
