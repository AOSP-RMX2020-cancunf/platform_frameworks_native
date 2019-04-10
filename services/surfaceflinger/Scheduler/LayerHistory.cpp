/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "LayerHistory.h"

#include <cinttypes>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>

#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include "SchedulerUtils.h"

namespace android {
namespace scheduler {

std::atomic<int64_t> LayerHistory::sNextId = 0;

LayerHistory::LayerHistory() {
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sf.layer_history_trace", value, "0");
    mTraceEnabled = bool(atoi(value));
}

LayerHistory::~LayerHistory() = default;

std::unique_ptr<LayerHistory::LayerHandle> LayerHistory::createLayer(const std::string name,
                                                                     float maxRefreshRate) {
    const int64_t id = sNextId++;

    std::lock_guard lock(mLock);
    mInactiveLayerInfos.emplace(id, std::make_shared<LayerInfo>(name, maxRefreshRate));
    return std::make_unique<LayerHistory::LayerHandle>(*this, id);
}

void LayerHistory::destroyLayer(const int64_t id) {
    std::lock_guard lock(mLock);
    auto it = mActiveLayerInfos.find(id);
    if (it != mActiveLayerInfos.end()) {
        mActiveLayerInfos.erase(it);
    }

    it = mInactiveLayerInfos.find(id);
    if (it != mInactiveLayerInfos.end()) {
        mInactiveLayerInfos.erase(it);
    }
}

void LayerHistory::insert(const std::unique_ptr<LayerHandle>& layerHandle, nsecs_t presentTime) {
    std::shared_ptr<LayerInfo> layerInfo;
    {
        std::lock_guard lock(mLock);
        auto layerInfoIterator = mInactiveLayerInfos.find(layerHandle->mId);
        if (layerInfoIterator != mInactiveLayerInfos.end()) {
            layerInfo = layerInfoIterator->second;
            mInactiveLayerInfos.erase(layerInfoIterator);
            mActiveLayerInfos.insert({layerHandle->mId, layerInfo});
        } else {
            layerInfoIterator = mActiveLayerInfos.find(layerHandle->mId);
            if (layerInfoIterator != mActiveLayerInfos.end()) {
                layerInfo = layerInfoIterator->second;
            } else {
                ALOGW("Inserting information about layer that is not registered: %" PRId64,
                      layerHandle->mId);
                return;
            }
        }
    }
    layerInfo->setLastPresentTime(presentTime);
}

float LayerHistory::getDesiredRefreshRate() {
    float newRefreshRate = 0.f;
    std::lock_guard lock(mLock);

    removeIrrelevantLayers();

    // Iterate through all layers that have been recently updated, and find the max refresh rate.
    for (const auto& [layerId, layerInfo] : mActiveLayerInfos) {
        const float layerRefreshRate = layerInfo->getDesiredRefreshRate();
        if (mTraceEnabled) {
            // Store the refresh rate in traces for easy debugging.
            std::string layerName = "LFPS " + layerInfo->getName();
            ATRACE_INT(layerName.c_str(), std::round(layerRefreshRate));
            ALOGD("%s: %f", layerName.c_str(), std::round(layerRefreshRate));
        }
        if (layerInfo->isRecentlyActive() && layerRefreshRate > newRefreshRate) {
            newRefreshRate = layerRefreshRate;
        }
    }
    if (mTraceEnabled) {
        ALOGD("LayerHistory DesiredRefreshRate: %.2f", newRefreshRate);
    }

    return newRefreshRate;
}

void LayerHistory::removeIrrelevantLayers() {
    const int64_t obsoleteEpsilon = systemTime() - scheduler::OBSOLETE_TIME_EPSILON_NS.count();
    // Iterator pointing to first element in map
    auto it = mActiveLayerInfos.begin();
    while (it != mActiveLayerInfos.end()) {
        // If last updated was before the obsolete time, remove it.
        if (it->second->getLastUpdatedTime() < obsoleteEpsilon) {
            // erase() function returns the iterator of the next
            // to last deleted element.
            if (mTraceEnabled) {
                ALOGD("Layer %s obsolete", it->second->getName().c_str());
                // Make sure to update systrace to indicate that the layer was erased.
                std::string layerName = "LFPS " + it->second->getName();
                ATRACE_INT(layerName.c_str(), 0);
            }
            auto id = it->first;
            auto layerInfo = it->second;
            layerInfo->clearHistory();
            mInactiveLayerInfos.insert({id, layerInfo});
            it = mActiveLayerInfos.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace scheduler
} // namespace android