/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef skgpu_graphite_GlobalCache_DEFINED
#define skgpu_graphite_GlobalCache_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/private/SkSpinlock.h"
#include "src/core/SkLRUCache.h"
#include "src/gpu/ResourceKey.h"

class SkShaderCodeDictionary;

namespace skgpu::graphite {

class ComputePipeline;
class GraphicsPipeline;

/**
 * GlobalCache holds GPU resources that should be shared by every Recorder. The common requirement
 * of these resources are they are static/read-only, have long lifetimes, and are likely to be used
 * by multiple Recorders. The canonical example of this are pipelines.
 *
 * GlobalCache is thread safe, but intentionally splits queries and storing operations so that they
 * are not atomic. The pattern is to query for a resource, which has a high likelihood of a cache
 * hit. If it's not found, the Recorder creates the resource on its own, without locking the
 * GlobalCache. After the resource is created, it is added to the GlobalCache, atomically returning
 * the winning Resource in the event of a race between Recorders for the same UniqueKey.
 */
class GlobalCache : public SkRefCnt {
public:
    GlobalCache();
    ~GlobalCache() override;

    // Find a cached GraphicsPipeline that matches the associated key.
    sk_sp<GraphicsPipeline> findGraphicsPipeline(const UniqueKey&) SK_EXCLUDES(fSpinLock);
    // Associate the given pipeline with the key. If the key has already had a separate pipeline
    // associated with the key, that pipeline is returned and the passed-in pipeline is discarded.
    // Otherwise, the passed-in pipeline is held by the GlobalCache and also returned back.
    sk_sp<GraphicsPipeline> addGraphicsPipeline(const UniqueKey&,
                                                sk_sp<GraphicsPipeline>) SK_EXCLUDES(fSpinLock);

    // Find amd add operations for ComputePipelines, with the same pattern as GraphicsPipelines.
    sk_sp<ComputePipeline> findComputePipeline(const UniqueKey&) SK_EXCLUDES(fSpinLock);
    sk_sp<ComputePipeline> addComputePipeline(const UniqueKey&,
                                              sk_sp<ComputePipeline>) SK_EXCLUDES(fSpinLock);

    SkShaderCodeDictionary* shaderCodeDictionary() const { return fShaderCodeDictionary.get(); }

private:
    struct KeyHash {
        uint32_t operator()(const UniqueKey& key) const { return key.hash(); }
    };

    using GraphicsPipelineCache = SkLRUCache<UniqueKey, sk_sp<GraphicsPipeline>, KeyHash>;
    using ComputePipelineCache  = SkLRUCache<UniqueKey, sk_sp<ComputePipeline>,  KeyHash>;

    // TODO: Have this owned through Context separately from GlobalCache
    std::unique_ptr<SkShaderCodeDictionary> fShaderCodeDictionary;

    // TODO: can we do something better given this should have write-seldom/read-often behavior?
    mutable SkSpinlock fSpinLock;

    // GraphicsPipelines and ComputePipelines are expensive to create, likely to be used by multiple
    // Recorders, and are ideally pre-compiled on process startup so thread write-contention is
    // expected to be low. For these reasons we store pipelines globally instead of per-Recorder.
    GraphicsPipelineCache fGraphicsPipelineCache SK_GUARDED_BY(fSpinLock);
    ComputePipelineCache  fComputePipelineCache  SK_GUARDED_BY(fSpinLock);

    // TODO: Cache/own static and GPU-private buffers that RenderSteps create on initialization?
};

} // namespace skgpu::graphite

#endif // skgpu_graphite_GlobalCache_DEFINED
