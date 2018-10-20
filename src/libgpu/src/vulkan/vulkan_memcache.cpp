#pragma optimize("", off)

#ifdef DECAF_VULKAN
#include "vulkan_driver.h"
#include "gpu_tiling.h"

namespace vulkan
{

MemCacheObject *
Driver::_allocMemCache(phys_addr address, uint32_t size, const MemCacheMutator& mutator)
{
   vk::BufferCreateInfo bufferDesc;
   bufferDesc.size = size;
   bufferDesc.usage =
      vk::BufferUsageFlagBits::eVertexBuffer |
      vk::BufferUsageFlagBits::eUniformBuffer |
      vk::BufferUsageFlagBits::eTransferDst |
      vk::BufferUsageFlagBits::eTransferSrc;
   bufferDesc.sharingMode = vk::SharingMode::eExclusive;
   bufferDesc.queueFamilyIndexCount = 0;
   bufferDesc.pQueueFamilyIndices = nullptr;

   VmaAllocationCreateInfo allocInfo = {};
   allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

   VkBuffer buffer;
   VmaAllocation allocation;
   vmaCreateBuffer(mAllocator,
                   &static_cast<VkBufferCreateInfo>(bufferDesc),
                   &allocInfo,
                   &buffer,
                   &allocation,
                   nullptr);

   auto cache = new MemCacheObject();
   cache->address = address;
   cache->size = size;
   cache->mutator = mutator;
   cache->allocation = allocation;
   cache->buffer = buffer;
   cache->dataHash = DataHash {};
   cache->lastUsageIndex = 0;
   cache->extnRefCount = 0;
   return cache;
}

void
Driver::_uploadMemCache(MemCacheObject *cache)
{
   void *data = phys_cast<void*>(cache->address).getRawPointer();

   if (cache->mutator.mode == MemCacheMutator::Mode::None) {
      // When there is no special mutator mode, we don't need to do anything
      // special and can just directly copy
   } else if (cache->mutator.mode == MemCacheMutator::Mode::Retile) {
      // Bring some stuff local so its easier to deal with
      auto& retile = cache->mutator.retile;
      auto dataBytesPtr = reinterpret_cast<uint8_t*>(data);

      // Set up our scratch buffer for untiling...
      auto& untiledImage = mScratchRetiling;
      untiledImage.resize(cache->size);

      // Perform the untiling
      gpu::convertFromTiled(
         untiledImage.data(),
         retile.pitch,
         dataBytesPtr,
         retile.tileMode,
         retile.swizzle,
         retile.pitch,
         retile.pitch,
         retile.height,
         retile.depth,
         retile.aa,
         retile.isDepth,
         retile.bpp);

      // Update the data pointer to be our untiled data instead
      data = reinterpret_cast<void*>(untiledImage.data());
   } else {
      decaf_abort("Unsupported memory cache mutator mode");
   }

   // Upload the data to the GPU through a staging buffer.  We usage a staging
   // buffer to enable all GPU-used resources to use GPU-side memory.
   auto stagingBuffer = getStagingBuffer(cache->size);
   void *mappedPtr = mapStagingBuffer(stagingBuffer, false);
   memcpy(mappedPtr, data, cache->size);
   unmapStagingBuffer(stagingBuffer, true);

   // Copy the data out of the staging buffer into the memory cache.
   vk::BufferCopy copyDesc;
   copyDesc.srcOffset = 0;
   copyDesc.dstOffset = 0;
   copyDesc.size = cache->size;
   mActiveCommandBuffer.copyBuffer(stagingBuffer->buffer, cache->buffer, { copyDesc });
}

void
Driver::_downloadMemCache(MemCacheObject *cache)
{
   // Create a staging buffer to use for the readback
   auto stagingBuffer = getStagingBuffer(cache->size);

   // Copy the data into our staging buffer from the cache object
   vk::BufferCopy copyDesc;
   copyDesc.srcOffset = 0;
   copyDesc.dstOffset = 0;
   copyDesc.size = cache->size;
   mActiveCommandBuffer.copyBuffer(cache->buffer, stagingBuffer->buffer, { copyDesc });

   // Record the pending invalidation of this buffer so that future
   // memory users that overlap this region know that they need to
   // fetch the data from here until the invalidation has completed.
   mPendingInvalidations.push_back(cache);

   // Add a task to copy the data back off the GPU...
   addRetireTask([=](){
      void *data = phys_cast<void*>(cache->address).getRawPointer();

      // Map our staging buffer so we can copy out of it.
      void *mappedPtr = mapStagingBuffer(stagingBuffer, false);

      if (cache->mutator.mode == MemCacheMutator::Mode::None) {
         // When no mutator is present, we can copy directly from the GPU back.
         memcpy(data, mappedPtr, cache->size);
      } else if (cache->mutator.mode == MemCacheMutator::Mode::Retile) {
         // Bring some stuff local so its easier to deal with
         auto& retile = cache->mutator.retile;
         auto dataBytesPtr = reinterpret_cast<uint8_t*>(data);

         // When retiling, it is benefitial for us to set up scratch space, do a DMA copy
         // from the shared memory to the host first, and then perform untiling into CPU mem.
         auto& untiledImage = mScratchRetiling;
         untiledImage.resize(cache->size);
         memcpy(untiledImage.data(), mappedPtr, cache->size);

         // Perform the tiling directly into the CPU memory.
         gpu::convertToTiled(
            dataBytesPtr,
            untiledImage.data(),
            retile.pitch,
            retile.tileMode,
            retile.swizzle,
            retile.pitch,
            retile.pitch,
            retile.height,
            retile.depth,
            retile.aa,
            retile.isDepth,
            retile.bpp);
      } else {
         decaf_abort("Unsupported memory cache mutator mode");
      }

      // Unmap the staging buffer.
      unmapStagingBuffer(stagingBuffer, true);

      // Remove the pending invalidation now that we've got it in CPU memory. Note
      // that we assume callbacks are invoked in the same order as the invalidations
      // and we only remove a single entry.
      auto foundElem = std::find(mPendingInvalidations.begin(), mPendingInvalidations.end(), cache);
      decaf_check(foundElem != mPendingInvalidations.end());
      mPendingInvalidations.erase(foundElem);
   });
}

MemCacheObject *
Driver::getMemCache(phys_addr address, uint32_t size, const MemCacheMutator& mutator)
{
   auto cacheKey = (static_cast<uint64_t>(address.getAddress()) << 32) | static_cast<uint64_t>(size);
   auto& cache = mMemCaches[cacheKey];
   if (!cache) {
      // If there is not yet a cache object, we need to create it.
      cache = _allocMemCache(address, size, mutator);
   } else {
      // Make sure mutator is correct.  In the future we should assume the
      // data is invalidated if the mutator is no longer correct.  We aren't
      // going to do this today though, as it potentially requires a CPU stall
      // in order to wait for GPU data to show up for retiling...
      cache->mutator == mutator;
   }

   refreshMemCache(cache);

   return cache;
}

void
Driver::refreshMemCache(MemCacheObject *cache)
{
   if (cache->lastUsageIndex >= mActivePm4BufferIndex) {
      // This cache object was already fetched at least once in this
      // Pm4Context, no need to do any additional work...
      return;
   }

   auto dataPtr = phys_cast<void*>(cache->address).getRawPointer();
   auto dataSize = cache->size;
   auto dataHash = DataHash {}.write(dataPtr, cache->size);
   if (cache->lastUsageIndex > 0 && cache->dataHash == dataHash) {
      // The buffer is still up to date, no need to upload
      cache->lastUsageIndex = mActivePm4BufferIndex;
      return;
   }

   _uploadMemCache(cache);
   cache->dataHash = dataHash;
   cache->lastUsageIndex = mActivePm4BufferIndex;
}

void
Driver::invalidateMemCache(MemCacheObject *cache)
{
   _downloadMemCache(cache);
}

DataBufferObject *
Driver::getDataMemCache(phys_addr baseAddress, uint32_t size, bool discardData)
{
   auto cache = getMemCache(baseAddress, size);
   return static_cast<DataBufferObject *>(cache);
}

} // namespace vulkan

#endif // ifdef DECAF_VULKAN