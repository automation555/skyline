// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <kernel/memory.h>
#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include "buffer.h"

namespace skyline::gpu {
    vk::DeviceSize GuestBuffer::BufferSize() const {
        vk::DeviceSize size{};
        for (const auto &buffer : mappings)
            size += buffer.size_bytes();
        return size;
    }

    void Buffer::SetupGuestMappings() {
        auto &mappings{guest.mappings};
        if (mappings.size() == 1) {
            auto mapping{mappings.front()};
            u8 *alignedData{util::AlignDown(mapping.data(), PAGE_SIZE)};
            size_t alignedSize{static_cast<size_t>(util::AlignUp(mapping.data() + mapping.size(), PAGE_SIZE) - alignedData)};

            alignedMirror = gpu.state.process->memory.CreateMirror(alignedData, alignedSize);
            mirror = alignedMirror.subspan(static_cast<size_t>(mapping.data() - alignedData), mapping.size());
        } else {
            std::vector<span<u8>> alignedMappings;

            const auto &frontMapping{mappings.front()};
            u8 *alignedData{util::AlignDown(frontMapping.data(), PAGE_SIZE)};
            alignedMappings.emplace_back(alignedData, (frontMapping.data() + frontMapping.size()) - alignedData);

            size_t totalSize{frontMapping.size()};
            for (auto it{std::next(mappings.begin())}; it != std::prev(mappings.end()); ++it) {
                auto mappingSize{it->size()};
                alignedMappings.emplace_back(it->data(), mappingSize);
                totalSize += mappingSize;
            }

            const auto &backMapping{mappings.back()};
            totalSize += backMapping.size();
            alignedMappings.emplace_back(backMapping.data(), util::AlignUp(backMapping.size(), PAGE_SIZE));

            alignedMirror = gpu.state.process->memory.CreateMirrors(alignedMappings);
            mirror = alignedMirror.subspan(static_cast<size_t>(frontMapping.data() - alignedData), totalSize);
        }
    }

    Buffer::Buffer(GPU &gpu, GuestBuffer guest) : gpu(gpu), size(guest.BufferSize()), backing(gpu.memory.AllocateBuffer(size)), guest(std::move(guest)) {
        SetupGuestMappings();
        SynchronizeHost();
    }

    Buffer::~Buffer() {
        std::lock_guard lock(*this);
        SynchronizeGuest(true);
        if (alignedMirror.valid())
            munmap(alignedMirror.data(), alignedMirror.size());
    }

    void Buffer::WaitOnFence() {
        TRACE_EVENT("gpu", "Buffer::WaitOnFence");

        auto lCycle{cycle.lock()};
        if (lCycle) {
            lCycle->Wait();
            cycle.reset();
        }
    }

    void Buffer::SynchronizeHost() {
        WaitOnFence();

        TRACE_EVENT("gpu", "Buffer::SynchronizeHost");

        auto host{backing.data()};
        for (auto &mapping : guest.mappings) {
            auto mappingSize{mapping.size_bytes()};
            std::memcpy(host, mapping.data(), mappingSize);
            host += mappingSize;
        }
    }

    void Buffer::SynchronizeHostWithCycle(const std::shared_ptr<FenceCycle> &pCycle) {
        if (pCycle != cycle.lock())
            WaitOnFence();

        TRACE_EVENT("gpu", "Buffer::SynchronizeHostWithCycle");

        auto host{backing.data()};
        for (auto &mapping : guest.mappings) {
            auto mappingSize{mapping.size_bytes()};
            std::memcpy(host, mapping.data(), mappingSize);
            host += mappingSize;
        }
    }

    void Buffer::SynchronizeGuest() {
        WaitOnFence();

        TRACE_EVENT("gpu", "Buffer::SynchronizeGuest");

        auto host{backing.data()};
        for (auto &mapping : guest.mappings) {
            auto mappingSize{mapping.size_bytes()};
            std::memcpy(mapping.data(), host, mappingSize);
            host += mappingSize;
        }
    }

    /**
     * @brief A FenceCycleDependency that synchronizes the contents of a host buffer with the guest buffer
     */
    struct BufferGuestSync : public FenceCycleDependency {
        std::shared_ptr<Buffer> buffer;

        explicit BufferGuestSync(std::shared_ptr<Buffer> buffer) : buffer(std::move(buffer)) {}

        ~BufferGuestSync() {
            TRACE_EVENT("gpu", "Buffer::BufferGuestSync");
            buffer->SynchronizeGuest();
        }
    };

    void Buffer::SynchronizeGuestWithCycle(const std::shared_ptr<FenceCycle> &pCycle) {
        if (pCycle != cycle.lock())
            WaitOnFence();

        pCycle->AttachObject(std::make_shared<BufferGuestSync>(shared_from_this()));
        cycle = pCycle;
    }

    void Buffer::Write(span<u8> data, vk::DeviceSize offset) {
        std::memcpy(mirror.data() + offset, data.data(), data.size());
    }

    std::shared_ptr<BufferView> Buffer::GetView(vk::DeviceSize offset, vk::DeviceSize range, vk::Format format) {
        for (const auto &viewWeak : views) {
            auto view{viewWeak.lock()};
            if (view && view->offset == offset && view->range == range && view->format == format)
                return view;
        }

        auto view{std::make_shared<BufferView>(shared_from_this(), offset, range, format)};
        views.push_back(view);
        return view;
    }

    BufferView::BufferView(std::shared_ptr<Buffer> backing, vk::DeviceSize offset, vk::DeviceSize range, vk::Format format) : buffer(std::move(backing)), offset(offset), range(range), format(format) {}

    void BufferView::lock() {
        auto backing{std::atomic_load(&buffer)};
        while (true) {
            backing->lock();

            auto latestBacking{std::atomic_load(&buffer)};
            if (backing == latestBacking)
                return;

            backing->unlock();
            backing = latestBacking;
        }
    }

    void BufferView::unlock() {
        buffer->unlock();
    }

    bool BufferView::try_lock() {
        auto backing{std::atomic_load(&buffer)};
        while (true) {
            bool success{backing->try_lock()};

            auto latestBacking{std::atomic_load(&buffer)};
            if (backing == latestBacking)
                // We want to ensure that the try_lock() was on the latest backing and not on an outdated one
                return success;

            if (success)
                // We only unlock() if the try_lock() was successful and we acquired the mutex
                backing->unlock();
            backing = latestBacking;
        }
    }
}