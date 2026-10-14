#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Words = 32;
constexpr std::uint32_t Size = 16;
constexpr std::uint32_t Format16UNorm = 7;
constexpr std::uint32_t Type1D = 8;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t ResultOffset = 8;
alignas(256) std::array<std::uint32_t, Threads * Words> Buffer{};
alignas(256) std::array<std::uint8_t, 16384> Line{};
alignas(256) std::array<std::uint8_t, 16384> Plane{};

alignas(256) constexpr std::array<std::uint32_t, 17> Code{
    0x34020087, 0xe0301000, 0x80000201, 0xe0301004, 0x80000301, 0xe0301008, 0x80000401,
    0xbf8c3f70, 0xf0bc0108, 0x00610802, 0xbf8c3f70, 0xe0701020, 0x80000801, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 17> ExplicitLodCode{
    0x34020087, 0xe0301000, 0x80000201, 0xe0301004, 0x80000301, 0xe0301008, 0x80000401,
    0xbf8c3f70, 0xf0b00108, 0x00610802, 0xbf8c3f70, 0xe0701020, 0x80000801, 0xbf810000,
};

constexpr std::array<float, 8> LineV{0.0f, 0.5f, 1.0f, 1.5f, -0.25f, 3.0f, 0.25f, 0.75f};

std::uint32_t TexelOf(std::uint32_t tid) {
    return tid % Size;
}

std::uint32_t Bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

void FillInput() {
    Buffer.fill(0xdeadbeefu);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto x = TexelOf(tid);
        const auto ref = Bits(0.5f);
        const auto u = Bits((static_cast<float>(x) + 0.5f) / static_cast<float>(Size));
        const auto v = Bits(LineV[tid % LineV.size()]);
        const std::array<std::uint32_t, 3> words{ref, u, v};
        std::copy(words.begin(), words.end(), &Buffer[tid * Words]);
    }
}

std::array<std::uint32_t, 8> LineDescriptor() {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Line.data()));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format16UNorm << 20u) | (((Size - 1u) & 3u) << 30u),
        (Size - 1u) >> 2u,
        0x0924u | (Type1D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 8> PlaneDescriptor() {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Plane.data()));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format16UNorm << 20u) | (((Size - 1u) & 3u) << 30u),
        ((Size - 1u) >> 2u) | ((Size - 1u) << 14u),
        0x0924u | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void FillDepthTexture(std::span<std::uint8_t> storage, const std::array<std::uint32_t, 8>& descriptor, std::uint32_t rows) {
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(descriptor));
    Require(geometry.guestBytes <= storage.size(), "image plane address on a line depth compare: the texture does not fit the storage");
    const auto& mip = geometry.mips.at(0);
    std::fill(storage.begin(), storage.end(), 0u);
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < Size; ++x) {
            auto* texel = reinterpret_cast<std::uint16_t*>(&storage[geometry.GuestLayerOffset(0) + mip.tiledOffset + static_cast<std::uint64_t>(y) * mip.pitchBytes + x * 2u]);
            const float depth = (static_cast<float>(x) + 0.5f) / static_cast<float>(Size);
            *texel = static_cast<std::uint16_t>(std::lround(depth * 65535.0f));
        }
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 4> SamplerDescriptor() {
    return {3u << 12u, 0xfffu << 12u, 1u << 26u, 0u};
}

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 8>& texture) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto buffer = BufferDescriptor(Buffer.data(), static_cast<std::uint32_t>(Buffer.size() * 4u));
    const auto sampler = SamplerDescriptor();
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 12);
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

void Check(const std::string& name) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto x = TexelOf(tid);
        const float texelDepth = (static_cast<float>(x) + 0.5f) / static_cast<float>(Size);
        const float expected = (0.5f <= texelDepth) ? 1.0f : 0.0f;
        const float actual = std::bit_cast<float>(Buffer[tid * Words + ResultOffset]);
        Require(std::fabs(actual - expected) < 0.01f, name + ": thread " + std::to_string(tid) + " expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

void RequireRefused(AgcDriver::VulkanDevice& device) {
    std::string refusal;
    try {
        Run(device, ExplicitLodCode, LineDescriptor());
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    Require(refusal.find("incompatible with the static runtime image interface") != std::string::npos, "image_sample_c_l 2d on a 1D texture was not refused: " + refusal);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        FillDepthTexture(Line, LineDescriptor(), 1u);
        FillInput();
        Run(*device, Code, LineDescriptor());
        Check("1D depth texture via 2D image_sample_c_lz");
        FillDepthTexture(Plane, PlaneDescriptor(), Size);
        FillInput();
        Run(*device, Code, PlaneDescriptor());
        Check("2D depth texture via 2D image_sample_c_lz");
        RequireRefused(*device);
        std::puts("image plane address on line depth compare tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
