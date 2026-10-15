#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::ColorTarget;
using AgcDriver::Graphics::ColorTileMode;
using AgcDriver::Graphics::ClearMultisampleTargets;
using AgcDriver::Graphics::MultisampleTargetView;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::RequireNoMultisampleTarget;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 8;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t HalfBlockBytes = 65536;
constexpr std::size_t BlockBytes = HalfBlockBytes * 2;
constexpr std::uint32_t Untouched = 0xdeadbeefu;
constexpr std::uint32_t StoredValue = 0x12345678u;

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e040300, 0x7e060280, 0x7e0202ff, StoredValue, 0xf0200108, 0x00020102, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels, std::uint32_t maxMip) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        maxMip << 4u,
        0u,
        0u,
    };
}

std::uint64_t GuestBytes(const std::uint32_t* texels, std::uint32_t maxMip) {
    const auto words = TextureDescriptor(texels, maxMip);
    return AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(words)).guestBytes;
}

void Store(AgcDriver::VulkanDevice& device, const std::uint32_t* texels, std::uint32_t maxMip) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels, maxMip);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    const std::span<const std::uint32_t> code(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::vector<std::uint32_t> storage((BlockBytes + 0x10000u) / 4u);
        auto* texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + 0xffffu) & ~std::uintptr_t{0xffffu});
        std::fill(texels, texels + BlockBytes / 4u, Untouched);
        auto* chunk1 = reinterpret_cast<std::uint8_t*>(texels);
        auto* chunk2 = chunk1 + HalfBlockBytes;
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(chunk1, HalfBlockBytes, true, true, true);
            mutation.Add(chunk2, HalfBlockBytes, true, true, true);
        }
        Require(AgcDriver::Graphics::RegisteredReadableCovers(AddressOf(chunk1), BlockBytes), "contiguous guest allocation ranges must be covered");
        const auto surface = AddressOf(texels);
        const auto guestBytes = GuestBytes(texels, 0u);
        Require(guestBytes <= BlockBytes, "the texture must fit within the registered memory");

        Store(*device, texels, 0u);
        const auto pending = StorageTexture::FindPending(surface, guestBytes);
        Require(pending != nullptr, "the store dispatch must leave a pending storage image");

        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        ColorTarget target{};
        target.address = surface;
        target.surfaceAddress = surface;
        target.extent = {Side, Side};
        target.surfaceExtent = target.extent;
        target.format = VK_FORMAT_R8G8B8A8_UNORM;
        target.bytes = Side * Side * 4u;
        target.tileMode = ColorTileMode::RenderTarget;
        target.samples = 4;
        target.elementBytes = 4;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Require(MultisampleTargetView(context, target) != VK_NULL_HANDLE, "multisampled target view creation failed");
        }

        std::array<std::byte, 64> readback{};
        AgcDriver::GuestMemory::Read(surface, readback);
        RequireNoMultisampleTarget(surface, 64u, "a CPU read");
        RequireNoMultisampleTarget(surface, 64u, "a DMA copy");

        device->WaitIdle();
        ClearMultisampleTargets(device->Device());
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            AgcDriver::Graphics::FlushCachedTextures(device->Device());
        }
        device->WaitIdle();
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(chunk1);
            mutation.Remove(chunk2);
        }
        std::puts("multisample pending access regression tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
