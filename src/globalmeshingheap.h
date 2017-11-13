// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#ifndef MESH__GLOBALMESHINGHEAP_H
#define MESH__GLOBALMESHINGHEAP_H

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "sanitizer/sanitizer_stoptheworld.h"

#include "heaplayers.h"

#include "binnedtracker.h"
#include "internal.h"
#include "meshable-arena.h"
#include "meshing.h"
#include "miniheap.h"

using namespace HL;

namespace mesh {

template <int NumBins>
class GlobalHeapStats {
public:
  atomic_size_t meshCount;
  atomic_size_t mhFreeCount;
  atomic_size_t mhAllocCount;
  atomic_size_t mhHighWaterMark;
};

template <typename BigHeap,                      // for large allocations
          int NumBins,                           // number of size classes
          int (*getSizeClass)(const size_t),     // same as for local
          size_t (*getClassMaxSize)(const int),  // same as for local
          int DefaultMeshPeriod,                 // perform meshing on average once every MeshPeriod frees
          size_t MinStringLen = 8UL>
class GlobalMeshingHeap : public MeshableArena {
private:
  DISALLOW_COPY_AND_ASSIGN(GlobalMeshingHeap);
  typedef MeshableArena Super;

  static_assert(getClassMaxSize(NumBins - 1) == 16384, "expected 16k max object size");
  static_assert(HL::gcd<BigHeap::Alignment, Alignment>::value == Alignment,
                "expected BigHeap to have 16-byte alignment");

  struct MeshArguments {
    GlobalMeshingHeap *instance;
    internal::vector<std::pair<MiniHeap *, MiniHeap *>> mergeSets;
  };

public:
  enum { Alignment = 16 };

  GlobalMeshingHeap()
      : _maxObjectSize(getClassMaxSize(NumBins - 1)),
        _prng(internal::seed()),
        _fastPrng(internal::seed(), internal::seed()) {
    for (size_t i = 0; i < NumBins; i++) {
      _littleheaps[i].init(this);
    }

    resetNextMeshCheck();
  }

  inline void dumpStrings() const {
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    for (size_t i = 0; i < NumBins; i++) {
      _littleheaps[i].printOccupancy();
    }
  }

  inline void dumpStats(int level, bool beDetailed) const {
    if (level < 1)
      return;

    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    debug("MESH COUNT:         %zu\n", (size_t)_stats.meshCount);
    debug("MH Alloc Count:     %zu\n", (size_t)_stats.mhAllocCount);
    debug("MH Free  Count:     %zu\n", (size_t)_stats.mhFreeCount);
    debug("MH High Water Mark: %zu\n", (size_t)_stats.mhHighWaterMark);
    for (size_t i = 0; i < NumBins; i++)
      _littleheaps[i].dumpStats(beDetailed);
  }

  inline MiniHeap *allocMiniheap(size_t objectSize) {
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    d_assert(objectSize <= _maxObjectSize);

    const int sizeClass = getSizeClass(objectSize);
    const size_t sizeMax = getClassMaxSize(sizeClass);

    d_assert_msg(objectSize == sizeMax, "sz(%zu) shouldn't be greater than %zu (class %d)", objectSize, sizeMax,
                 sizeClass);
    d_assert(sizeClass >= 0);
    d_assert(sizeClass < NumBins);

    // check our bins for a miniheap to reuse
    MiniHeap *existing = _littleheaps[sizeClass].selectForReuse();
    if (existing != nullptr) {
      existing->reattach(_prng, _fastPrng);  // populate freelist, set attached bit
      d_assert(existing->isAttached());
      // TODO: check that metadata is right?
      return existing;
    }

    // if we have objects bigger than the size of a page, allocate
    // multiple pages to amortize the cost of creating a
    // miniheap/globally locking the heap.
    size_t nObjects = max(HL::CPUInfo::PageSize / sizeMax, MinStringLen);

    const size_t nPages = PageCount(sizeMax * nObjects);
    const size_t spanSize = HL::CPUInfo::PageSize * nPages;
    d_assert(0 < spanSize);

    void *span = Super::malloc(spanSize);
    if (unlikely(span == nullptr))
      abort();

    void *buf = internal::Heap().malloc(sizeof(MiniHeap));
    if (unlikely(buf == nullptr))
      abort();

    // if (spanSize > 4096)
    //   mesh::debug("spana %p(%zu) %p (%zu)", span, spanSize, buf, objectSize);
    MiniHeap *mh = new (buf) MiniHeap(span, nObjects, sizeMax, _prng, _fastPrng, spanSize);
    Super::assoc(span, mh, nPages);

    trackMiniheapLocked(sizeClass, mh);

    _stats.mhAllocCount++;
    //_stats.mhHighWaterMark = max(_miniheaps.size(), _stats.mhHighWaterMark.load());
    //_stats.mhClassHWM[sizeClass] = max(_littleheapCounts[sizeClass], _stats.mhClassHWM[sizeClass].load());

    return mh;
  }

  void *malloc(size_t sz) {
    const int sizeClass = getSizeClass(sz);
    const size_t sizeMax = getClassMaxSize(sizeClass);

    if (unlikely(sizeMax <= _maxObjectSize))
      abort();

    std::lock_guard<std::mutex> lock(_bigMutex);
    return _bigheap.malloc(sz);
  }

  // if the MiniHeap is non-null, its reference count is increased by one
  inline MiniHeap *miniheapFor(const void *ptr) const {
    std::shared_lock<std::shared_timed_mutex> sharedLock(_mhRWLock);

    auto mh = reinterpret_cast<MiniHeap *>(Super::lookup(ptr));
    if (mh != nullptr)
      mh->ref();

    return mh;
  }

  void trackMiniheapLocked(size_t sizeClass, MiniHeap *mh) {
    _littleheaps[sizeClass].add(mh);
  }

  void untrackMiniheapLocked(size_t sizeClass, MiniHeap *mh) {
    _stats.mhAllocCount -= 1;
    _littleheaps[sizeClass].remove(mh);
  }

  // called with lock held
  void freeMiniheapAfterMeshLocked(MiniHeap *mh, bool untrack = true) {
    const auto sizeClass = getSizeClass(mh->objectSize());
    if (untrack)
      untrackMiniheapLocked(sizeClass, mh);

    mh->MiniHeapBase::~MiniHeapBase();
    char *mhp = reinterpret_cast<char *>(mh);
    memset(mhp, 66, sizeof(MiniHeap));
    // internal::Heap().free(mh);
  }

  void freeMiniheap(MiniHeap *&mh, bool untrack = true) {
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    const auto spans = mh->spans();
    const auto spanSize = mh->spanSize();

    const auto meshCount = mh->meshCount();
    for (size_t i = 0; i < meshCount; i++) {
      Super::free(reinterpret_cast<void *>(spans[i]), spanSize);
    }

    _stats.mhFreeCount++;

    freeMiniheapAfterMeshLocked(mh, untrack);
    mh = nullptr;
  }

  void free(void *ptr) {
    // if (unlikely(internal::isMeshMarker(ptr))) {
    //   dumpStats(2, false);
    //   for (size_t i = 0; i < 128; i++)
    //     meshAllSizeClasses();
    //   dumpStats(2, false);
    //   return;
    // }

    // two possibilities: most likely the ptr is small (and therefor
    // owned by a miniheap), or is a large allocation

    auto mh = miniheapFor(ptr);
    if (unlikely(!mh)) {
      std::lock_guard<std::mutex> lock(_bigMutex);
      _bigheap.free(ptr);
      return;
    }

    mh->free(ptr);
    bool shouldConsiderMesh = !mh->isEmpty();
    // unreffed by the bin tracker
    // mh->unref();

    const auto szClass = getSizeClass(mh->objectSize());

    bool shouldFlush = false;
    {
      // FIXME: inefficient
      std::shared_lock<std::shared_timed_mutex> sharedLock(_mhRWLock);
      // this may free the miniheap -- we can't safely access it after
      // this point.
      shouldFlush = _littleheaps[szClass].postFree(mh);
      mh = nullptr;
    }

    if (shouldFlush)
      _littleheaps[szClass].flushFreeMiniheaps();

    if (!shouldConsiderMesh) {
      return;
    }

    if (unlikely(shouldMesh())) {
      // meshAllSizeClasses();
      // meshSizeClass(getSizeClass(mh->objectSize()));
    }
  }

  inline size_t getSize(void *ptr) const {
    if (unlikely(ptr == nullptr))  // || internal::isMeshMarker(ptr))
      return 0;

    auto mh = miniheapFor(ptr);
    if (likely(mh)) {
      auto size = mh->getSize(ptr);
      mh->unref();
      return size;
    } else {
      std::lock_guard<std::mutex> lock(_bigMutex);
      return _bigheap.getSize(ptr);
    }
  }

  int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    std::shared_lock<std::shared_timed_mutex> sharedLock(_mhRWLock);

    if (!oldp || !oldlenp || *oldlenp < sizeof(size_t))
      return -1;

    auto statp = reinterpret_cast<size_t *>(oldp);

    if (strcmp(name, "mesh.check_period") == 0) {
      *statp = _meshPeriod;
      if (!newp || newlen < sizeof(size_t))
        return -1;
      auto newVal = reinterpret_cast<size_t *>(newp);
      _meshPeriod = *newVal;
      resetNextMeshCheck();
    } else if (strcmp(name, "mesh.compact") == 0) {
      sharedLock.unlock();
      meshAllSizeClasses();
      sharedLock.lock();
    } else if (strcmp(name, "arena") == 0) {
      // not sure what this should do
    } else if (strcmp(name, "stats.resident") == 0) {
      auto pss = internal::measurePssKiB();
      // mesh::debug("measurePssKiB: %zu KiB", pss);

      *statp = pss * 1024;  // originally in KB
    } else if (strcmp(name, "stats.active") == 0) {
      // all miniheaps at least partially full
      size_t sz = _bigheap.arenaSize();
      for (size_t i = 0; i < NumBins; i++) {
        const auto count = _littleheaps[i].nonEmptyCount();
        if (count == 0)
          continue;
        sz += count * _littleheaps[i].objectSize() * _littleheaps[i].objectCount();
      }
      *statp = sz;
    } else if (strcmp(name, "stats.allocated") == 0) {
      // same as active for us, for now -- memory not returned to the OS
      size_t sz = _bigheap.arenaSize();
      for (size_t i = 0; i < NumBins; i++) {
        const auto &bin = _littleheaps[i];
        const auto count = bin.nonEmptyCount();
        if (count == 0)
          continue;
        sz += bin.objectSize() * bin.allocatedObjectCount();
      }
      *statp = sz;
    }
    return 0;
  }

  size_t getAllocatedMiniheapCount() const {
    return Super::bitmap().inUseCount();
  }

  void lock() {
    _mhRWLock.lock();
    _bigMutex.lock();
  }

  void unlock() {
    _bigMutex.unlock();
    _mhRWLock.unlock();
  }

  // PUBLIC ONLY FOR TESTING
  // must be called with the world stopped, after call to mesh()
  // completes src is a nullptr
  void meshLocked(MiniHeap *dst, MiniHeap *&src) {
    if (dst->meshCount() + src->meshCount() > internal::MaxMeshes)
      return;

    dst->consume(src);

    const size_t dstSpanSize = dst->spanSize();
    const auto dstSpanStart = reinterpret_cast<void *>(dst->getSpanStart());

    const auto srcSpans = src->spans();
    const auto srcMeshCount = src->meshCount();

    for (size_t i = 0; i < srcMeshCount; i++) {
      Super::mesh(dstSpanStart, srcSpans[i], dstSpanSize);
    }

    // make sure we adjust what bin the destination is in -- it might
    // now be full and not a candidate for meshing
    _littleheaps[getSizeClass(dst->objectSize())].postFree(dst);
    freeMiniheapAfterMeshLocked(src);
    src = nullptr;
  }

protected:
  inline void resetNextMeshCheck() {
    // a period of 0 means do not mesh.
    if (_meshPeriod == 0)
      return;

    uniform_int_distribution<size_t> distribution(1, _meshPeriod);
    _nextMeshCheck = distribution(_prng);
  }

  inline bool shouldMesh() {
    _nextMeshCheck--;
    bool shouldMesh = _meshPeriod > 0 && _nextMeshCheck == 0;
    if (unlikely(shouldMesh))
      resetNextMeshCheck();

    return shouldMesh;
  }

  static void performMeshing(const __sanitizer::SuspendedThreadsList &suspendedThreads, void *argument) {
    MeshArguments *args = (MeshArguments *)argument;

    for (auto &mergeSet : args->mergeSets) {
      // merge into the one with a larger mesh count
      if (std::get<0>(mergeSet)->meshCount() < std::get<1>(mergeSet)->meshCount())
        mergeSet = std::pair<MiniHeap *, MiniHeap *>(std::get<1>(mergeSet), std::get<0>(mergeSet));

      args->instance->meshLocked(std::get<0>(mergeSet), std::get<1>(mergeSet));
    }
  }

  // check for meshes in all size classes -- must be called unlocked
  void meshAllSizeClasses() {
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    MeshArguments args;
    args.instance = this;

    // first, clear out any free memory we might have
    for (size_t i = 0; i < NumBins; i++) {
      _littleheaps[i].flushFreeMiniheaps();
    }

    // FIXME: is it safe to have this function not use internal::allocator?
    auto meshFound = function<void(std::pair<MiniHeap *, MiniHeap *> &&)>(
        // std::allocator_arg, internal::allocator,
        [&](std::pair<MiniHeap *, MiniHeap *> &&miniheaps) {
          if (std::get<0>(miniheaps)->isMeshingCandidate() && std::get<0>(miniheaps)->isMeshingCandidate())
            args.mergeSets.push_back(std::move(miniheaps));
        });

    for (size_t i = 0; i < NumBins; i++) {
      // method::randomSort(_prng, _littleheapCounts[i], _littleheaps[i], meshFound);
      // method::greedySplitting(_prng, _littleheaps[i], meshFound);
      method::simpleGreedySplitting(_prng, _littleheaps[i], meshFound);
    }

    if (args.mergeSets.size() == 0) {
      // debug("nothing to mesh.");
      return;
    }

    _stats.meshCount += args.mergeSets.size();

    // run the actual meshing with the world stopped
    __sanitizer::StopTheWorld(performMeshing, &args);
  }

  void meshSizeClass(size_t sizeClass) {
    MeshArguments args;
    args.instance = this;

    // XXX: can we get away with using shared at first, then upgrading to exclusive?
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    // FIXME: is it safe to have this function not use internal::allocator?
    auto meshFound = function<void(internal::vector<MiniHeap *> &&)>(
        // std::allocator_arg, internal::allocator,
        [&](internal::vector<MiniHeap *> &&miniheaps) {
          if (miniheaps[0]->isMeshingCandidate() && miniheaps[1]->isMeshingCandidate())
            args.mergeSets.push_back(std::move(miniheaps));
        });

    // method::greedySplitting(_prng, _littleheaps[sizeClass], meshFound);
    // method::simpleGreedySplitting(_prng, _littleheaps[sizeClass], meshFound);

    if (args.mergeSets.size() == 0)
      return;

    _stats.meshCount += args.mergeSets.size();

    // run the actual meshing with the world stopped
    __sanitizer::StopTheWorld(performMeshing, &args);

    // internal::StopTheWorld();
    // performMeshing(&args);
    // internal::StartTheWorld();
  }

  const size_t _maxObjectSize;
  size_t _nextMeshCheck{0};
  atomic_size_t _meshPeriod{DefaultMeshPeriod};

  // The big heap is handles malloc requests for large objects.  We
  // define a separate class to handle these to segregate bookkeeping
  // for large malloc requests from the ones used to back spans (which
  // are allocated from the arena)
  BigHeap _bigheap{};

  mt19937_64 _prng;
  MWC _fastPrng;

  BinnedTracker<MiniHeap, GlobalMeshingHeap> _littleheaps[NumBins];

  mutable std::mutex _bigMutex{};
  mutable std::shared_timed_mutex _mhRWLock{};

  GlobalHeapStats<NumBins> _stats{};
};
}  // namespace mesh

#endif  // MESH__GLOBALMESHINGHEAP_H
