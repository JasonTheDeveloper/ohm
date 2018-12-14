// Copyright (c) 2018
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#ifndef GPUKERNEL_H
#define GPUKERNEL_H

#include "gpuConfig.h"

#include <functional>

namespace gputil
{
  struct KernelDetail;
  class Buffer;
  class Device;
  class EventList;
  class Event;
  class Queue;

  struct gputilAPI Dim3
  {
    size_t x = 1, y = 1, z = 1;

    Dim3() {}
    Dim3(size_t x, size_t y = 1, size_t z = 1)
      : x(x)
      , y(y)
      , z(z)
    {}

    inline size_t volume() const { return x * y * z; }

    inline size_t operator[](int i) const
    {
      switch (i)
      {
      case 0:
        return x;
      case 1:
        return y;
      case 2:
        return z;
      }
      return 0;
    }

    inline size_t &operator[](int i)
    {
      switch (i)
      {
      case 0:
        return x;
      case 1:
        return y;
      case 2:
        return z;
      }
      static size_t invalid = 0u;
      return invalid;
    }

    inline size_t operator[](unsigned i) const { return operator[](int(i)); }
    inline size_t &operator[](unsigned i) { return operator[](int(i)); }

    inline size_t operator[](size_t i) const { return operator[](int(i)); }
    inline size_t &operator[](size_t i) { return operator[](int(i)); }
  };

  template <typename T>
  struct gputilAPI BufferArg
  {
    using ArgType = T;
    inline BufferArg(Buffer &buffer)
      : buffer(buffer)
    {}
    Buffer &buffer;
  };

  /// Local memory calculation function.
  /// @param work_group_size The work group total size.
  /// @return The number of bytes required for a group this size.
  using LocalMemFunc = std::function<size_t(size_t)>;

  /// Defines a callable kernel object.
  ///
  /// For OpenCL, this wraps the OpenCL kernel object and is initialised using <tt>gputil::openCLKernel()</tt>
  /// using a @c Program and the entry point name.
  ///
  /// For CUDA, this wraps a function pointer which calls the CUDA kernel and is created using
  /// <tt>gputil::cudaKernel()</tt>.
  ///
  /// There is no implementation indendent way of creating a @c Kernel.
  ///
  /// Invoking the kernel requires at least a global and local size (threads and blocks size). OpenCL global offset
  /// is not supported. A @c Queue pointer must be passed, though may be null, as it marks the beginning of device
  /// arguments. An @c Event object to track completion and an @p EventList to wait on before executing may also be
  /// optionally given any any combination. @c Buffer objects must be wrapped in a @c BufferArg in order to define
  /// (pointer) type on the device.
  ///
  /// A kernel invocation then takes this form:
  /// @code{.unparsed}
  ///   kernel(global_size, local_size[, wait_on_events][, completion_event], queue, ...args);
  /// @endcode
  ///
  /// Local memory is sized by using @c addLocal() which defines a functional object to define requires local memory
  /// size based on the total local memory size.
  class gputilAPI Kernel
  {
  public:
    Kernel();
    Kernel(Kernel &&other);

    ~Kernel();

    bool isValid() const;

    void release();

    /// Add local memory calculation.
    ///
    /// Local memory is calculated by invoking the given function, passing the single dimensional work group size
    /// in order to calculate the required work group local memory size in bytes. This function is invoked just
    /// prior to invoking the kernel and when calculating the optimal work group size.
    ///
    /// Under CUDA, local memory requirements are tallied and passed to the kernel function hook given the total local
    /// memory required.
    ///
    /// Under OpenCL, each @c addLocal() call adds a local memory argument to the end of the argument list.
    ///
    /// @param local_calc The functional object used to calculate local memory size requirements.
    void addLocal(const LocalMemFunc &local_calc);

    /// Calculate the optimal size (or volume) of a local work group. This attempts to gain maximum occupancy while
    /// considering the required local memory usage.
    /// @return The optimal work group size.
    size_t calculateOptimalWorkGroupSize();

    /// Fetch the previously calculated optimal work group size (see @c calculateOptimalWorkGroupSize()).
    /// @return The optimal work group size.
    size_t optimalWorkGroupSize() const;

    /// Calculate the appropriate global and work group sizes for executing this @c Kernel to process
    /// @p total_work_items items. The aim is to gain maximum local thread occupancy.
    ///
    /// The @p total_work_items defines a volume of items to process. The global size is set appropriately to cover
    /// the @p total_work_items with the @p local_size set to cover these in a grid pattern with consideration to the
    /// device capabilities and maximum occupancy. This includes maximum work group sizes and local memory constraints.
    ///
    /// @param[out] global_size Set to the grid global size. This may be larger than @p total_work_group_items to ensure
    ///   an exact multiple of the @p local_size.
    /// @param[out] local_size Set to the local work group size required to cover the @p global_size/@p total_work_items.
    /// @param total_work_items The total volume of items to process.
    void calculateGrid(gputil::Dim3 *global_size, gputil::Dim3 *local_size, const gputil::Dim3 &total_work_items);

    template <typename... ARGS>
    int operator()(const Dim3 &global_size, const Dim3 &local_size, Queue *queue, ARGS... args);

    template <typename... ARGS>
    int operator()(const Dim3 &global_size, const Dim3 &local_size, Event &completion_event, Queue *queue,
                   ARGS... args);

    template <typename... ARGS>
    int operator()(const Dim3 &global_size, const Dim3 &local_size, const EventList &event_list, Queue *queue,
                   ARGS... args);

    template <typename... ARGS>
    int operator()(const Dim3 &global_size, const Dim3 &local_size, const EventList &event_list,
                   Event &completion_event, Queue *queue, ARGS... args);

    KernelDetail *detail() const { return imp_; }

    Device device();

    Kernel &operator=(Kernel &&other);

  private:
    KernelDetail *imp_;
  };
}  // namespace gputil

#if GPUTIL_TYPE == GPUTIL_OPENCL
#include "cl/gpuKernel2.h"
#elif GPUTIL_TYPE == GPUTIL_CUDA
#include "cuda/gpuKernel2.h"
#else  // GPUTIL_TYPE == ???
#error Unknown GPU base API
#endif  // GPUTIL_TYPE

#endif  // GPUKERNEL_H
