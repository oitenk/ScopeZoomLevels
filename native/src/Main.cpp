// SZL Outline Fix - test plugin for Scope Zoom Levels: makes ping outlines line up with the
// magnified picture in a zoomed scope lens.
//
// Why: the lens shader enlarges a copy of the scene, but the game draws the ping/scan outlines
// later, on a layer of their own (a full-screen RGBA8 image, black except the outlines), which
// it mixes onto the screen together with the HUD. So inside the lens the outlines stay at their
// unzoomed size and place. Found with the SZL Inspector plugin (see ../inspector).
//
// What: right after the compute shader that finishes that layer (recognised by its shader
// hash), the lens area of the layer is copied to a buffer, enlarged by the lens magnification
// around the lens centre by a small compute shader of ours, and copied back. The game then
// mixes it as usual. Our work uses only root constants and root buffer views (no descriptor
// heaps), and the game's compute state is put back exactly as it was afterwards.
//
// The lens area: every scope glass draw (glass_scope.mt, recognised by its pixel shader) is drawn
// a second time into an 8-bit mask of ours, with the game's own vertex shader, so the remap knows
// exactly where the lens is on screen, for any scope, field of view or aim animation.
//
// Control (for now from the CET console):
//   Game.SZL_OutlineLens(true, 3.0)          on, with the lens zoom value; false to switch off
//   Game.SZL_OutlineTune(0.45, 0.45, 1.0)    outer limit x / y of the lens search (share of screen height), and a
//                                            factor: magnification = zoom value * factor
//   Game.SZL_OutlineCenter(0.5, 0.5)         lens centre (share of screen width / height)
//   Game.SZL_OutlineStyle(true, 0.9)         outline only (drop the fill) inside the lens, and the
//                                            opacity a texel needs to count as outline; false = vanilla

#include <RED4ext/RED4ext.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LensMaskDxil.h"
#include "Vtbl.h"

namespace
{
const RED4ext::v1::Sdk* g_sdk = nullptr;
RED4ext::v1::PluginHandle g_handle = nullptr;

void Info(const char* aFormat, ...)
{
    if (!g_sdk)
        return;
    char buf[1024];
    va_list args;
    va_start(args, aFormat);
    vsnprintf(buf, sizeof(buf), aFormat, args);
    va_end(args);
    g_sdk->logger->Info(g_handle, buf);
}

template<typename T>
bool HookSlot(void* aObject, int aSlot, T aDetour, T* aOriginal, const char* aName)
{
    void** vtable = *reinterpret_cast<void***>(aObject);
    bool ok = g_sdk->hooking->Attach(g_handle, vtable[aSlot], reinterpret_cast<void*>(aDetour),
                                     reinterpret_cast<void**>(aOriginal));
    if (!ok)
        Info("hook %s FAILED", aName);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Settings, set from script / the CET console.

struct Lens
{
    bool active = false;
    float zoom = 1.f;
    // Outer limit of the area searched for lens glass (share of screen height). The lens itself
    // comes from the mask of the scope glass drawn each frame.
    float radiusX = 0.45f;
    float radiusY = 0.45f;
    float factor = 1.f;    // magnification = zoom * factor
    float centerX = 0.5f;  // share of screen width / height
    float centerY = 0.5f;
    bool outlineOnly = true;
    float cutoff = 0.9f;   // opacity a texel needs to count as outline
};
std::mutex g_lensMutex;
Lens g_lens;

// ---------------------------------------------------------------------------------------------
// Finding the outline layer: the UAV bound when the overlay shader runs.

constexpr uint64_t OverlayShaderHash = 0xc78f229d71c24f5dull; // FNV-1a of its bytecode, game 2.5

uint64_t Fnv1a(const void* aData, size_t aSize)
{
    uint64_t h = 1469598103934665603ull;
    auto* p = static_cast<const uint8_t*>(aData);
    for (size_t i = 0; i < aSize; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// True if a compiled shader's container (DXBC header: magic, hash, version, size, part count,
// part offsets; each part starts with its four-character code) has the given part.
bool HasShaderPart(const D3D12_SHADER_BYTECODE& aCode, const char (&aFourCC)[5])
{
    auto* p = static_cast<const uint8_t*>(aCode.pShaderBytecode);
    if (!p || aCode.BytecodeLength < 32 || memcmp(p, "DXBC", 4) != 0)
        return false;
    uint32_t count;
    memcpy(&count, p + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * (i + 1) <= aCode.BytecodeLength; ++i)
    {
        uint32_t offset;
        memcpy(&offset, p + 32 + 4 * i, 4);
        if (offset + 4 <= aCode.BytecodeLength && memcmp(p + offset, aFourCC, 4) == 0)
            return true;
    }
    return false;
}

std::shared_mutex g_psoMutex;
std::unordered_set<void*> g_overlayPsos;

UINT g_viewStep = 0;
std::shared_mutex g_descMutex;
std::unordered_map<SIZE_T, ID3D12Resource*> g_uavs; // CPU descriptor -> texture, UAVs only

struct HeapInfo
{
    SIZE_T cpu;
    UINT64 gpu;
    UINT count;
};
std::shared_mutex g_heapMutex;
std::vector<HeapInfo> g_heaps;

struct TableEntry
{
    UINT offset;
    char kind;
};
std::shared_mutex g_rsMutex;
std::unordered_map<void*, std::vector<std::vector<TableEntry>>> g_rootSigs;

using DeserializerFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
DeserializerFn g_deserializer = nullptr;

void RegisterRootSig(void* aRs, const void* aBlob, SIZE_T aSize)
{
    if (!g_deserializer || !aRs || !aBlob)
        return;
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    if (FAILED(g_deserializer(aBlob, aSize, IID_PPV_ARGS(&des))))
        return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    std::vector<std::vector<TableEntry>> tables;
    if (SUCCEEDED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)) && desc)
    {
        const auto& d = desc->Desc_1_1;
        tables.resize(d.NumParameters);
        for (UINT p = 0; p < d.NumParameters; ++p)
        {
            const auto& param = d.pParameters[p];
            if (param.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                continue;
            UINT running = 0;
            for (UINT r = 0; r < param.DescriptorTable.NumDescriptorRanges; ++r)
            {
                const auto& range = param.DescriptorTable.pDescriptorRanges[r];
                UINT start = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                 ? running
                                 : range.OffsetInDescriptorsFromTableStart;
                UINT count = range.NumDescriptors == UINT_MAX ? 8 : std::min<UINT>(range.NumDescriptors, 16);
                char kind = range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? 'U' : '-';
                for (UINT i = 0; i < count; ++i)
                    tables[p].push_back(TableEntry{start + i, kind});
                running = start + count;
            }
        }
    }
    des->Release();
    std::unique_lock lock(g_rsMutex);
    g_rootSigs[aRs] = std::move(tables);
}

// ---------------------------------------------------------------------------------------------
// Per command list: the compute state (to put it back after our dispatch) and the resource
// states this list has moved resources into.

struct ListState
{
    void* rootSig = nullptr;
    void* pso = nullptr;
    std::map<UINT, UINT64> tables, cbvs, srvs, uavs;
    std::map<UINT, std::array<uint32_t, 64>> consts;
    std::map<UINT, uint64_t> constMask;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;
    // Render targets, to put them back after drawing a lens into our mask.
    bool rtsKnown = false;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rts;
    bool hasDs = false;
    D3D12_CPU_DESCRIPTOR_HANDLE ds{};
};
std::mutex g_listMutex;
std::unordered_map<void*, ListState> g_lists;

// ---------------------------------------------------------------------------------------------
// Our compute pass.

constexpr const char* RemapShader = R"(
cbuffer P : register(b0)
{
    uint bw; uint bh; uint pitch; uint maskOn;
    float cx; float cy; float rx; float ry;
    float invM; float cutoff; float msx; float msy;   // msx/msy: mask size / layer size
    float x0; float y0; uint mbx; uint mby;           // box origin in the layer / in the mask
    uint mpitch; uint mbw; uint mbh; uint pad0;
};
ByteAddressBuffer Src : register(t0);
RWByteAddressBuffer Dst : register(u0);
ByteAddressBuffer Mask : register(t1); // the lens glass as drawn this frame (8-bit, 255 = lens)

bool InLens(float2 gp)
{
    if (maskOn == 0)
        return true; // no mask (fallback): the circle alone decides
    int2 m = int2(floor(gp * float2(msx, msy))) - int2(mbx, mby);
    if (m.x < 0 || m.y < 0 || m.x >= int(mbw) || m.y >= int(mbh))
        return false;
    uint a = uint(m.y) * mpitch + uint(m.x);
    return ((Mask.Load(a & ~3u) >> ((a & 3u) * 8)) & 255u) >= 128u;
}

// cutoff > 0: outline only. The game's layer holds a solid outline (fully opaque) around a
// hatched, half-transparent fill and a faint trailing pattern; keeping only the texels at or
// above the cutoff keeps the outline and drops the rest.
float4 Texel(int2 p)
{
    p = clamp(p, int2(0, 0), int2(bw - 1, bh - 1));
    uint v = Src.Load(p.y * pitch + p.x * 4);
    float4 c = float4(v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24) / 255.0;
    if (cutoff > 0.0 && c.a < cutoff)
        return 0;
    return c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= bw || id.y >= bh)
        return;
    uint at = id.y * pitch + id.x * 4;
    float2 d = float2(id.xy) + 0.5 - float2(cx, cy);
    float2 e = d / float2(rx, ry);
    if (dot(e, e) > 1.0 || !InLens(float2(x0, y0) + float2(id.xy) + 0.5))
    {
        Dst.Store(at, Src.Load(at));
        return;
    }
    float2 s = float2(cx, cy) + d * invM - 0.5;
    int2 i = int2(floor(s));
    float2 f = s - i;
    float4 c = lerp(lerp(Texel(i), Texel(i + int2(1, 0)), f.x),
                    lerp(Texel(i + int2(0, 1)), Texel(i + int2(1, 1)), f.x), f.y);
    uint4 u = uint4(round(saturate(c) * 255.0));
    Dst.Store(at, u.x | (u.y << 8) | (u.z << 16) | (u.w << 24));
}
)";

// Draws the lens glass into our mask: same vertex shader and geometry as the game's glass draw.
constexpr const char* MaskShader = R"(
float4 main() : SV_Target
{
    return 1.0;
}
)";

// Pixel shader of the game's scope glass (base\materials\glass_scope.mt) in the transparent pass,
// FNV-1a of its bytecode, game 2.5. Seen with and without DLSS; any scope using that material
// (modded ones too) draws with it.
constexpr uint64_t LensGlassShaderHash = 0x3270f6703ad3bf1full;

ID3D12Device* g_device = nullptr;
ID3D12RootSignature* g_ourRootSig = nullptr;
ID3D12PipelineState* g_ourPso = nullptr;
ID3DBlob* g_maskPs = nullptr;
ID3D12Resource* g_bufSrc = nullptr;
ID3D12Resource* g_bufDst = nullptr;
UINT64 g_bufSize = 0;
ID3D12Resource* g_bufMask = nullptr;
UINT64 g_bufMaskSize = 0;

// Game glass pipeline -> our twin that writes the mask.
std::shared_mutex g_lensPsoMutex;
std::unordered_map<void*, ID3D12PipelineState*> g_lensPsos;

// Render target view -> texture, to size the mask like the target the lens is drawn into.
std::shared_mutex g_rtvMutex;
std::unordered_map<SIZE_T, ID3D12Resource*> g_rtvs;
UINT g_rtvStep = 0;

// Our lens mask: an 8-bit render target, kept in RENDER_TARGET state between uses. The lens
// draws fill it; the remap reads and then clears it, so it never carries a stale lens.
std::mutex g_maskMutex;
ID3D12Resource* g_mask = nullptr;
ID3D12DescriptorHeap* g_maskRtvHeap = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_maskRtv{};
UINT g_maskW = 0, g_maskH = 0;
bool g_maskDrawn = false;
std::atomic<bool> g_lensActive{false};
std::atomic<bool> g_ready{false};
std::atomic<uint64_t> g_applied{0};

// Originals, called directly so our own commands don't go through our hooks.
#define ORIG(name) decltype(&Hk##name) g_##name = nullptr

using List = ID3D12GraphicsCommandList;
void STDMETHODCALLTYPE HkSetPipelineState(List* aSelf, ID3D12PipelineState* aPso);
ORIG(SetPipelineState);
void STDMETHODCALLTYPE HkResourceBarrier(List* aSelf, UINT aCount, const D3D12_RESOURCE_BARRIER* aBarriers);
ORIG(ResourceBarrier);
void STDMETHODCALLTYPE HkSetComputeRootSignature(List* aSelf, ID3D12RootSignature* aRs);
ORIG(SetComputeRootSignature);
void STDMETHODCALLTYPE HkSetComputeRootDescriptorTable(List* aSelf, UINT aIndex, D3D12_GPU_DESCRIPTOR_HANDLE aBase);
ORIG(SetComputeRootDescriptorTable);
void STDMETHODCALLTYPE HkSetComputeRoot32BitConstant(List* aSelf, UINT aIndex, UINT aValue, UINT aOffset);
ORIG(SetComputeRoot32BitConstant);
void STDMETHODCALLTYPE HkSetComputeRoot32BitConstants(List* aSelf, UINT aIndex, UINT aCount, const void* aData,
                                                      UINT aOffset);
ORIG(SetComputeRoot32BitConstants);
void STDMETHODCALLTYPE HkSetComputeRootConstantBufferView(List* aSelf, UINT aIndex, D3D12_GPU_VIRTUAL_ADDRESS aVa);
ORIG(SetComputeRootConstantBufferView);
void STDMETHODCALLTYPE HkSetComputeRootShaderResourceView(List* aSelf, UINT aIndex, D3D12_GPU_VIRTUAL_ADDRESS aVa);
ORIG(SetComputeRootShaderResourceView);
void STDMETHODCALLTYPE HkSetComputeRootUnorderedAccessView(List* aSelf, UINT aIndex, D3D12_GPU_VIRTUAL_ADDRESS aVa);
ORIG(SetComputeRootUnorderedAccessView);
void STDMETHODCALLTYPE HkDispatch(List* aSelf, UINT aX, UINT aY, UINT aZ);
ORIG(Dispatch);
void STDMETHODCALLTYPE HkDrawInstanced(List* aSelf, UINT aVerts, UINT aInst, UINT aStartVert, UINT aStartInst);
ORIG(DrawInstanced);
void STDMETHODCALLTYPE HkDrawIndexedInstanced(List* aSelf, UINT aIdx, UINT aInst, UINT aStartIdx, INT aBase,
                                              UINT aStartInst);
ORIG(DrawIndexedInstanced);
void STDMETHODCALLTYPE HkOMSetRenderTargets(List* aSelf, UINT aCount, const D3D12_CPU_DESCRIPTOR_HANDLE* aRts,
                                            BOOL aSingleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* aDs);
ORIG(OMSetRenderTargets);
// Not hooked: taken straight from the command list vtable.
using CopyTextureRegionFn = void(STDMETHODCALLTYPE*)(List*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                      const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
CopyTextureRegionFn g_CopyTextureRegion = nullptr;
using ClearRenderTargetViewFn = void(STDMETHODCALLTYPE*)(List*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT,
                                                          const D3D12_RECT*);
ClearRenderTargetViewFn g_ClearRenderTargetView = nullptr;

bool CreateOurPass()
{
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
                           : nullptr;
    if (!compile || !serialize)
    {
        Info("no shader compiler or root signature serializer; fix off");
        return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(compile(RemapShader, strlen(RemapShader), "SZLOutlineRemap", nullptr, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        Info("shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        if (errors)
            errors->Release();
        return false;
    }
    if (FAILED(compile(MaskShader, strlen(MaskShader), "SZLLensMask", nullptr, nullptr, "main", "ps_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_maskPs, &errors)))
    {
        Info("mask shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        if (errors)
            errors->Release();
        code->Release();
        return false;
    }
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 20};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {0, 0};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC rsd{4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rsBlob = nullptr;
    bool ok = SUCCEEDED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)) &&
              SUCCEEDED(g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                      IID_PPV_ARGS(&g_ourRootSig)));
    if (rsBlob)
        rsBlob->Release();
    if (ok)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_ourRootSig;
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ok = SUCCEEDED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_ourPso)));
    }
    code->Release();
    Info("outline remap pass %s", ok ? "ready" : "FAILED");
    return ok;
}

bool EnsureBuffers(UINT64 aSize)
{
    if (g_bufSize >= aSize)
        return true;
    if (g_bufSrc)
        g_bufSrc->Release();
    if (g_bufDst)
        g_bufDst->Release();
    g_bufSrc = g_bufDst = nullptr;
    g_bufSize = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = aSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Both start in COPY_DEST / UNORDERED_ACCESS, and every use below returns them there.
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&g_bufSrc))) ||
        FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&g_bufDst))))
    {
        Info("could not create the remap buffers (%llu bytes)", static_cast<unsigned long long>(aSize));
        return false;
    }
    g_bufSize = aSize;
    return true;
}

bool EnsureMaskBuffer(UINT64 aSize)
{
    if (g_bufMaskSize >= aSize)
        return true;
    if (g_bufMask)
        g_bufMask->Release();
    g_bufMask = nullptr;
    g_bufMaskSize = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = aSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&g_bufMask))))
        return false;
    g_bufMaskSize = aSize;
    return true;
}

// Called with g_maskMutex held, while recording a lens draw: makes sure the mask matches the
// target the lens is drawn into. A new mask is cleared in this list before its first use.
bool EnsureMask(List* aList, UINT aW, UINT aH)
{
    if (g_mask && g_maskW == aW && g_maskH == aH)
        return true;
    // A replaced mask may still be referenced by lists already recorded: kept alive (only
    // happens when the resolution changes).
    g_mask = nullptr;
    if (!g_maskRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_maskRtvHeap))))
            return false;
        g_maskRtv = g_maskRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = aW;
    td.Height = aH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8_UNORM, {0, 0, 0, 0}};
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 &clear, IID_PPV_ARGS(&g_mask))))
    {
        g_mask = nullptr;
        return false;
    }
    g_device->CreateRenderTargetView(g_mask, nullptr, g_maskRtv);
    const FLOAT zero[4] = {0, 0, 0, 0};
    g_ClearRenderTargetView(aList, g_maskRtv, zero, 0, nullptr);
    g_maskW = aW;
    g_maskH = aH;
    g_maskDrawn = false;
    Info("lens mask %ux%u", aW, aH);
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* aRes, D3D12_RESOURCE_STATES aBefore, D3D12_RESOURCE_STATES aAfter)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = aRes;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = aBefore;
    b.Transition.StateAfter = aAfter;
    return b;
}

ID3D12Resource* FindOverlay(const ListState& aState)
{
    std::vector<std::vector<TableEntry>> layout;
    {
        std::shared_lock lock(g_rsMutex);
        auto it = g_rootSigs.find(aState.rootSig);
        if (it == g_rootSigs.end())
            return nullptr;
        layout = it->second;
    }
    for (auto& [index, base] : aState.tables)
    {
        if (index >= layout.size())
            continue;
        SIZE_T cpu = 0;
        {
            std::shared_lock lock(g_heapMutex);
            for (auto& h : g_heaps)
                if (base >= h.gpu && base < h.gpu + static_cast<UINT64>(h.count) * g_viewStep)
                    cpu = h.cpu + static_cast<SIZE_T>(base - h.gpu);
        }
        if (!cpu)
            continue;
        std::shared_lock lock(g_descMutex);
        for (auto& e : layout[index])
        {
            if (e.kind != 'U')
                continue;
            auto it = g_uavs.find(cpu + static_cast<SIZE_T>(e.offset) * g_viewStep);
            if (it == g_uavs.end() || !it->second)
                continue;
            D3D12_RESOURCE_DESC d = it->second->GetDesc();
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= 640 &&
                (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS))
                return it->second;
        }
    }
    return nullptr;
}

void Restore(List* aList, const ListState& s)
{
    g_SetComputeRootSignature(aList, static_cast<ID3D12RootSignature*>(s.rootSig));
    for (auto& [i, v] : s.tables)
        g_SetComputeRootDescriptorTable(aList, i, D3D12_GPU_DESCRIPTOR_HANDLE{v});
    for (auto& [i, v] : s.cbvs)
        g_SetComputeRootConstantBufferView(aList, i, v);
    for (auto& [i, v] : s.srvs)
        g_SetComputeRootShaderResourceView(aList, i, v);
    for (auto& [i, v] : s.uavs)
        g_SetComputeRootUnorderedAccessView(aList, i, v);
    for (auto& [i, values] : s.consts)
    {
        uint64_t mask = s.constMask.at(i);
        for (UINT o = 0; o < 64;)
        {
            if (!(mask >> o & 1))
            {
                ++o;
                continue;
            }
            UINT start = o;
            while (o < 64 && (mask >> o & 1))
                ++o;
            g_SetComputeRoot32BitConstants(aList, i, o - start, values.data() + start, start);
        }
    }
    if (s.pso)
        g_SetPipelineState(aList, static_cast<ID3D12PipelineState*>(s.pso));
}

void AfterOverlay(List* aList)
{
    Lens lens;
    {
        std::lock_guard lock(g_lensMutex);
        lens = g_lens;
    }
    float m = lens.zoom * lens.factor;
    if (!lens.active || m <= 1.001f || !g_ready)
        return;
    // Only where scope glass was drawn this frame. If no glass twin could be made at all, fall
    // back to a fixed circle (the 0.2 behaviour) rather than no fix.
    {
        std::lock_guard lock(g_maskMutex);
        if (!g_mask || !g_maskDrawn)
        {
            std::shared_lock twins(g_lensPsoMutex);
            if (!g_lensPsos.empty())
                return;
            lens.radiusX = lens.radiusY = 0.31f;
        }
    }

    ListState s;
    {
        std::lock_guard lock(g_listMutex);
        auto it = g_lists.find(aList);
        if (it == g_lists.end() || !it->second.rootSig)
            return;
        s = it->second;
    }
    ID3D12Resource* overlay = FindOverlay(s);
    if (!overlay)
        return;
    auto st = s.states.find(overlay);
    if (st == s.states.end())
        return; // not moved in this list: its state is unknown, so leave it alone
    D3D12_RESOURCE_STATES state = st->second;

    D3D12_RESOURCE_DESC d = overlay->GetDesc();
    const float w = static_cast<float>(d.Width), h = static_cast<float>(d.Height);
    const float cx = lens.centerX * w, cy = lens.centerY * h;
    const float rx = lens.radiusX * h, ry = lens.radiusY * h;
    UINT x0 = static_cast<UINT>(std::clamp(std::floor(cx - rx), 0.f, w));
    UINT x1 = static_cast<UINT>(std::clamp(std::ceil(cx + rx), 0.f, w));
    UINT y0 = static_cast<UINT>(std::clamp(std::floor(cy - ry), 0.f, h));
    UINT y1 = static_cast<UINT>(std::clamp(std::ceil(cy + ry), 0.f, h));
    if (x1 <= x0 + 1 || y1 <= y0 + 1)
        return;
    UINT bw = x1 - x0, bh = y1 - y0;
    UINT pitch = (bw * 4 + 255) & ~255u;
    if (!EnsureBuffers(static_cast<UINT64>(pitch) * bh))
        return;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{0, {d.Format, bw, bh, 1, pitch}};
    D3D12_BOX box{x0, y0, 0, x1, y1, 1};
    D3D12_TEXTURE_COPY_LOCATION tex{overlay, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    tex.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{g_bufSrc, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION dst{g_bufDst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;

    // 1. Lens box of the layer -> source buffer.
    D3D12_RESOURCE_BARRIER b1 = Transition(overlay, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        g_ResourceBarrier(aList, 1, &b1);
    g_CopyTextureRegion(aList, &src, 0, 0, 0, &tex, &box);

    // 1b. The lens mask's matching box -> mask buffer; then the mask is cleared for the next
    // frame. Without a mask (no scope glass drawn yet) nothing is magnified.
    struct
    {
        UINT bw, bh, pitch, maskOn;
        float cx, cy, rx, ry, invM, cutoff, msx, msy, x0, y0;
        UINT mbx, mby, mpitch, mbw, mbh, pad0;
    } c{bw, bh, pitch, 0, cx - x0, cy - y0, rx, ry, 1.f / m, lens.outlineOnly ? lens.cutoff : 0.f, 1, 1,
        static_cast<float>(x0), static_cast<float>(y0), 0, 0, 0, 0, 0, 0};
    bool maskUsed = false;
    {
        std::lock_guard lock(g_maskMutex);
        if (g_mask && g_maskDrawn)
        {
            c.msx = g_maskW / w;
            c.msy = g_maskH / h;
            UINT mx0 = std::min(static_cast<UINT>(x0 * c.msx), g_maskW - 1);
            UINT my0 = std::min(static_cast<UINT>(y0 * c.msy), g_maskH - 1);
            UINT mx1 = std::min(static_cast<UINT>(std::ceil(x1 * c.msx)), g_maskW);
            UINT my1 = std::min(static_cast<UINT>(std::ceil(y1 * c.msy)), g_maskH);
            UINT mbw = mx1 > mx0 ? mx1 - mx0 : 0, mbh = my1 > my0 ? my1 - my0 : 0;
            UINT mpitch = (mbw + 255) & ~255u;
            if (mbw && mbh && EnsureMaskBuffer(static_cast<UINT64>(mpitch) * mbh))
            {
                D3D12_TEXTURE_COPY_LOCATION mtex{g_mask, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                mtex.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION mbuf{g_bufMask, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                mbuf.PlacedFootprint = {0, {DXGI_FORMAT_R8_UNORM, mbw, mbh, 1, mpitch}};
                D3D12_BOX mbox{mx0, my0, 0, mx1, my1, 1};
                D3D12_RESOURCE_BARRIER m1 = Transition(g_mask, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
                g_ResourceBarrier(aList, 1, &m1);
                g_CopyTextureRegion(aList, &mbuf, 0, 0, 0, &mtex, &mbox);
                D3D12_RESOURCE_BARRIER m2[2] = {
                    Transition(g_mask, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    Transition(g_bufMask, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
                g_ResourceBarrier(aList, 2, m2);
                const FLOAT zero[4] = {0, 0, 0, 0};
                g_ClearRenderTargetView(aList, g_maskRtv, zero, 0, nullptr);
                g_maskDrawn = false;
                c.maskOn = 1;
                c.mbx = mx0;
                c.mby = my0;
                c.mpitch = mpitch;
                c.mbw = mbw;
                c.mbh = mbh;
                maskUsed = true;
            }
        }
    }

    // 2. Remap into the second buffer.
    D3D12_RESOURCE_BARRIER b2[2] = {
        Transition(g_bufSrc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(overlay, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    g_ResourceBarrier(aList, 2, b2);
    g_SetComputeRootSignature(aList, g_ourRootSig);
    g_SetPipelineState(aList, g_ourPso);
    g_SetComputeRoot32BitConstants(aList, 0, 20, &c, 0);
    g_SetComputeRootShaderResourceView(aList, 1, g_bufSrc->GetGPUVirtualAddress());
    g_SetComputeRootUnorderedAccessView(aList, 2, g_bufDst->GetGPUVirtualAddress());
    g_SetComputeRootShaderResourceView(aList, 3, (maskUsed ? g_bufMask : g_bufSrc)->GetGPUVirtualAddress());
    g_Dispatch(aList, (bw + 7) / 8, (bh + 7) / 8, 1);

    // 3. Result -> the layer, then everything back to how it was.
    D3D12_RESOURCE_BARRIER b3 = Transition(g_bufDst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_ResourceBarrier(aList, 1, &b3);
    D3D12_BOX all{0, 0, 0, bw, bh, 1};
    g_CopyTextureRegion(aList, &tex, x0, y0, 0, &dst, &all);
    D3D12_RESOURCE_BARRIER b4[3] = {
        Transition(overlay, D3D12_RESOURCE_STATE_COPY_DEST, state),
        Transition(g_bufDst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(g_bufSrc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    g_ResourceBarrier(aList, 3, b4);
    if (maskUsed)
    {
        D3D12_RESOURCE_BARRIER b5 = Transition(g_bufMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_COPY_DEST);
        g_ResourceBarrier(aList, 1, &b5);
    }
    Restore(aList, s);

    if (g_applied++ == 0)
        Info("first remap: layer %llux%u, lens box %ux%u at %u,%u, magnification %.2f",
             static_cast<unsigned long long>(d.Width), d.Height, bw, bh, x0, y0, m);
}

// ---------------------------------------------------------------------------------------------
// Hooks

HRESULT STDMETHODCALLTYPE HkCreateComputePipelineState(ID3D12Device* aSelf, const D3D12_COMPUTE_PIPELINE_STATE_DESC* aDesc,
                                                       REFIID aRiid, void** aOut);
ORIG(CreateComputePipelineState);
HRESULT STDMETHODCALLTYPE HkCreateComputePipelineState(ID3D12Device* aSelf, const D3D12_COMPUTE_PIPELINE_STATE_DESC* aDesc,
                                                       REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateComputePipelineState(aSelf, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut && aDesc->CS.pShaderBytecode &&
        Fnv1a(aDesc->CS.pShaderBytecode, aDesc->CS.BytecodeLength) == OverlayShaderHash)
    {
        std::unique_lock lock(g_psoMutex);
        g_overlayPsos.insert(*aOut);
        Info("overlay shader found (pipeline %p)", *aOut);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateGraphicsPipelineState(ID3D12Device* aSelf, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* aDesc,
                                                        REFIID aRiid, void** aOut);
ORIG(CreateGraphicsPipelineState);
HRESULT STDMETHODCALLTYPE HkCreateGraphicsPipelineState(ID3D12Device* aSelf, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* aDesc,
                                                        REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateGraphicsPipelineState(aSelf, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut && g_maskPs && aDesc->PS.pShaderBytecode &&
        Fnv1a(aDesc->PS.pShaderBytecode, aDesc->PS.BytecodeLength) == LensGlassShaderHash)
    {
        // The twin: the game's root signature, vertex shader, input layout and rasterizer, with
        // our pixel shader writing 1 into a single 8-bit target, no blending, no depth.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *aDesc;
        // One pipeline can't mix shader formats: match the game's vertex shader (DXIL or DXBC).
        bool dxil = HasShaderPart(aDesc->VS, "DXIL");
        d.PS = dxil ? D3D12_SHADER_BYTECODE{kLensMaskDxil, sizeof(kLensMaskDxil)}
                    : D3D12_SHADER_BYTECODE{g_maskPs->GetBufferPointer(), g_maskPs->GetBufferSize()};
        d.StreamOutput = {};
        d.BlendState = {};
        d.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        d.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        d.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        d.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        d.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        d.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        d.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        // Depth and stencil off, but every field still a valid value (zero is not a valid
        // comparison function or stencil operation).
        d.DepthStencilState = {};
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        d.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        for (auto* face : {&d.DepthStencilState.FrontFace, &d.DepthStencilState.BackFace})
        {
            face->StencilFailOp = D3D12_STENCIL_OP_KEEP;
            face->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
            face->StencilPassOp = D3D12_STENCIL_OP_KEEP;
            face->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
        // Unused blend slots also get valid values.
        for (UINT i = 1; i < 8; ++i)
            d.BlendState.RenderTarget[i] = d.BlendState.RenderTarget[0];
        d.NumRenderTargets = 1;
        for (auto& f : d.RTVFormats)
            f = DXGI_FORMAT_UNKNOWN;
        d.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        d.DSVFormat = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc = {1, 0};
        d.CachedPSO = {};
        d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ID3D12PipelineState* twin = nullptr;
        ID3D12PipelineState* game = nullptr;
        static_cast<IUnknown*>(*aOut)->QueryInterface(IID_PPV_ARGS(&game));
        HRESULT th = game ? g_CreateGraphicsPipelineState(aSelf, &d, IID_PPV_ARGS(&twin)) : E_NOINTERFACE;
        if (SUCCEEDED(th))
        {
            std::unique_lock lock(g_lensPsoMutex);
            g_lensPsos[game] = twin;
            Info("scope glass pipeline %p: lens mask twin ready (%s)", game, dxil ? "DXIL" : "DXBC");
        }
        else
            Info("scope glass pipeline %p: lens mask twin FAILED, error 0x%08x (%s, %u input elements, topology %d)",
                 game, static_cast<unsigned>(th), dxil ? "DXIL" : "DXBC", aDesc->InputLayout.NumElements,
                 aDesc->PrimitiveTopologyType);
        if (game)
            game->Release();
    }
    return hr;
}

void STDMETHODCALLTYPE HkCreateRenderTargetView(ID3D12Device* aSelf, ID3D12Resource* aRes,
                                                const D3D12_RENDER_TARGET_VIEW_DESC* aDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE aHandle);
ORIG(CreateRenderTargetView);
void STDMETHODCALLTYPE HkCreateRenderTargetView(ID3D12Device* aSelf, ID3D12Resource* aRes,
                                                const D3D12_RENDER_TARGET_VIEW_DESC* aDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE aHandle)
{
    g_CreateRenderTargetView(aSelf, aRes, aDesc, aHandle);
    std::unique_lock lock(g_rtvMutex);
    g_rtvs[aHandle.ptr] = aRes;
}

HRESULT STDMETHODCALLTYPE HkCreateDescriptorHeap(ID3D12Device* aSelf, const D3D12_DESCRIPTOR_HEAP_DESC* aDesc,
                                                 REFIID aRiid, void** aOut);
ORIG(CreateDescriptorHeap);
HRESULT STDMETHODCALLTYPE HkCreateDescriptorHeap(ID3D12Device* aSelf, const D3D12_DESCRIPTOR_HEAP_DESC* aDesc,
                                                 REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateDescriptorHeap(aSelf, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut && aDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        (aDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
    {
        ID3D12DescriptorHeap* heap = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*aOut)->QueryInterface(IID_PPV_ARGS(&heap))))
        {
            HeapInfo info{heap->GetCPUDescriptorHandleForHeapStart().ptr, heap->GetGPUDescriptorHandleForHeapStart().ptr,
                          aDesc->NumDescriptors};
            heap->Release();
            std::unique_lock lock(g_heapMutex);
            g_heaps.push_back(info);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateRootSignature(ID3D12Device* aSelf, UINT aMask, const void* aBlob, SIZE_T aSize,
                                                REFIID aRiid, void** aOut);
ORIG(CreateRootSignature);
HRESULT STDMETHODCALLTYPE HkCreateRootSignature(ID3D12Device* aSelf, UINT aMask, const void* aBlob, SIZE_T aSize,
                                                REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateRootSignature(aSelf, aMask, aBlob, aSize, aRiid, aOut);
    if (SUCCEEDED(hr) && aOut && *aOut)
        RegisterRootSig(*aOut, aBlob, aSize);
    return hr;
}

void STDMETHODCALLTYPE HkCreateUnorderedAccessView(ID3D12Device* aSelf, ID3D12Resource* aRes, ID3D12Resource* aCounter,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC* aDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE aHandle);
ORIG(CreateUnorderedAccessView);
void STDMETHODCALLTYPE HkCreateUnorderedAccessView(ID3D12Device* aSelf, ID3D12Resource* aRes, ID3D12Resource* aCounter,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC* aDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE aHandle)
{
    g_CreateUnorderedAccessView(aSelf, aRes, aCounter, aDesc, aHandle);
    std::unique_lock lock(g_descMutex);
    g_uavs[aHandle.ptr] = aRes;
}

void CopyRange(SIZE_T aDst, SIZE_T aSrc, UINT aCount)
{
    std::unique_lock lock(g_descMutex);
    for (UINT i = 0; i < aCount; ++i)
    {
        SIZE_T to = aDst + static_cast<SIZE_T>(i) * g_viewStep;
        auto it = g_uavs.find(aSrc + static_cast<SIZE_T>(i) * g_viewStep);
        if (it != g_uavs.end())
            g_uavs[to] = it->second;
        else
            g_uavs.erase(to);
    }
}

void STDMETHODCALLTYPE HkCopyDescriptorsSimple(ID3D12Device* aSelf, UINT aCount, D3D12_CPU_DESCRIPTOR_HANDLE aDst,
                                               D3D12_CPU_DESCRIPTOR_HANDLE aSrc, D3D12_DESCRIPTOR_HEAP_TYPE aType);
ORIG(CopyDescriptorsSimple);
void STDMETHODCALLTYPE HkCopyDescriptorsSimple(ID3D12Device* aSelf, UINT aCount, D3D12_CPU_DESCRIPTOR_HANDLE aDst,
                                               D3D12_CPU_DESCRIPTOR_HANDLE aSrc, D3D12_DESCRIPTOR_HEAP_TYPE aType)
{
    g_CopyDescriptorsSimple(aSelf, aCount, aDst, aSrc, aType);
    if (aType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        CopyRange(aDst.ptr, aSrc.ptr, aCount);
}

void STDMETHODCALLTYPE HkCopyDescriptors(ID3D12Device* aSelf, UINT aDstRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* aDsts,
                                         const UINT* aDstSizes, UINT aSrcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* aSrcs,
                                         const UINT* aSrcSizes, D3D12_DESCRIPTOR_HEAP_TYPE aType);
ORIG(CopyDescriptors);
void STDMETHODCALLTYPE HkCopyDescriptors(ID3D12Device* aSelf, UINT aDstRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* aDsts,
                                         const UINT* aDstSizes, UINT aSrcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* aSrcs,
                                         const UINT* aSrcSizes, D3D12_DESCRIPTOR_HEAP_TYPE aType)
{
    g_CopyDescriptors(aSelf, aDstRanges, aDsts, aDstSizes, aSrcRanges, aSrcs, aSrcSizes, aType);
    if (aType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !aDsts || !aSrcs)
        return;
    UINT di = 0, dOff = 0, si = 0, sOff = 0;
    while (di < aDstRanges && si < aSrcRanges)
    {
        UINT dSize = aDstSizes ? aDstSizes[di] : 1;
        UINT sSize = aSrcSizes ? aSrcSizes[si] : 1;
        UINT n = std::min(dSize - dOff, sSize - sOff);
        CopyRange(aDsts[di].ptr + static_cast<SIZE_T>(dOff) * g_viewStep,
                  aSrcs[si].ptr + static_cast<SIZE_T>(sOff) * g_viewStep, n);
        dOff += n;
        sOff += n;
        if (dOff >= dSize)
        {
            ++di;
            dOff = 0;
        }
        if (sOff >= sSize)
        {
            ++si;
            sOff = 0;
        }
    }
}

HRESULT STDMETHODCALLTYPE HkReset(List* aSelf, ID3D12CommandAllocator* aAlloc, ID3D12PipelineState* aPso);
ORIG(Reset);
HRESULT STDMETHODCALLTYPE HkReset(List* aSelf, ID3D12CommandAllocator* aAlloc, ID3D12PipelineState* aPso)
{
    {
        std::lock_guard lock(g_listMutex);
        auto& s = g_lists[aSelf];
        s = ListState{};
        s.pso = aPso;
    }
    return g_Reset(aSelf, aAlloc, aPso);
}

void STDMETHODCALLTYPE HkSetPipelineState(List* aSelf, ID3D12PipelineState* aPso)
{
    {
        std::lock_guard lock(g_listMutex);
        g_lists[aSelf].pso = aPso;
    }
    g_SetPipelineState(aSelf, aPso);
}

void STDMETHODCALLTYPE HkResourceBarrier(List* aSelf, UINT aCount, const D3D12_RESOURCE_BARRIER* aBarriers)
{
    if (aBarriers)
    {
        std::lock_guard lock(g_listMutex);
        auto& s = g_lists[aSelf];
        for (UINT i = 0; i < aCount; ++i)
            if (aBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                s.states[aBarriers[i].Transition.pResource] = aBarriers[i].Transition.StateAfter;
    }
    g_ResourceBarrier(aSelf, aCount, aBarriers);
}

void STDMETHODCALLTYPE HkSetComputeRootSignature(List* aSelf, ID3D12RootSignature* aRs)
{
    {
        std::lock_guard lock(g_listMutex);
        auto& s = g_lists[aSelf];
        s.rootSig = aRs;
        s.tables.clear();
        s.cbvs.clear();
        s.srvs.clear();
        s.uavs.clear();
        s.consts.clear();
        s.constMask.clear();
    }
    g_SetComputeRootSignature(aSelf, aRs);
}

void STDMETHODCALLTYPE HkSetComputeRootDescriptorTable(List* aSelf, UINT aIndex, D3D12_GPU_DESCRIPTOR_HANDLE aBase)
{
    {
        std::lock_guard lock(g_listMutex);
        g_lists[aSelf].tables[aIndex] = aBase.ptr;
    }
    g_SetComputeRootDescriptorTable(aSelf, aIndex, aBase);
}

void SetConsts(List* aList, UINT aIndex, UINT aCount, const uint32_t* aData, UINT aOffset)
{
    std::lock_guard lock(g_listMutex);
    auto& s = g_lists[aList];
    auto& values = s.consts[aIndex];
    auto& mask = s.constMask[aIndex];
    for (UINT i = 0; i < aCount && aOffset + i < 64; ++i)
    {
        values[aOffset + i] = aData[i];
        mask |= 1ull << (aOffset + i);
    }
}

void STDMETHODCALLTYPE HkSetComputeRoot32BitConstant(List* aSelf, UINT aIndex, UINT aValue, UINT aOffset)
{
    SetConsts(aSelf, aIndex, 1, &aValue, aOffset);
    g_SetComputeRoot32BitConstant(aSelf, aIndex, aValue, aOffset);
}

void STDMETHODCALLTYPE HkSetComputeRoot32BitConstants(List* aSelf, UINT aIndex, UINT aCount, const void* aData,
                                                      UINT aOffset)
{
    if (aData)
        SetConsts(aSelf, aIndex, aCount, static_cast<const uint32_t*>(aData), aOffset);
    g_SetComputeRoot32BitConstants(aSelf, aIndex, aCount, aData, aOffset);
}

void STDMETHODCALLTYPE HkSetComputeRootConstantBufferView(List* aSelf, UINT aIndex, D3D12_GPU_VIRTUAL_ADDRESS aVa)
{
    {
        std::lock_guard lock(g_listMutex);
        g_lists[aSelf].cbvs[aIndex] = aVa;
    }
    g_SetComputeRootConstantBufferView(aSelf, aIndex, aVa);
}

void STDMETHODCALLTYPE HkSetComputeRootShaderResourceView(List* aSelf, UINT aIndex, D3D12_GPU_VIRTUAL_ADDRESS aVa)
{
    {
        std::lock_guard lock(g_listMutex);
        g_lists[aSelf].srvs[aIndex] = aVa;
    }
    g_SetComputeRootShaderResourceView(aSelf, aIndex, aVa);
}

void STDMETHODCALLTYPE HkSetComputeRootUnorderedAccessView(List* aSelf, UINT aIndex, D3D12_GPU_VIRTUAL_ADDRESS aVa)
{
    {
        std::lock_guard lock(g_listMutex);
        g_lists[aSelf].uavs[aIndex] = aVa;
    }
    g_SetComputeRootUnorderedAccessView(aSelf, aIndex, aVa);
}

void STDMETHODCALLTYPE HkOMSetRenderTargets(List* aSelf, UINT aCount, const D3D12_CPU_DESCRIPTOR_HANDLE* aRts,
                                            BOOL aSingleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* aDs)
{
    {
        std::lock_guard lock(g_listMutex);
        auto& s = g_lists[aSelf];
        s.rtsKnown = true;
        s.rts.clear();
        for (UINT i = 0; i < aCount && aRts; ++i)
            s.rts.push_back({aSingleRange ? aRts[0].ptr + static_cast<SIZE_T>(i) * g_rtvStep : aRts[i].ptr});
        s.hasDs = aDs != nullptr;
        if (aDs)
            s.ds = *aDs;
    }
    g_OMSetRenderTargets(aSelf, aCount, aRts, aSingleRange, aDs);
}

// After a scope glass draw: the same draw again with the twin pipeline into our lens mask, then
// the game's render targets and pipeline are put back. Does nothing when the lens fix is off or
// the draw's targets are unknown.
template<typename Redraw>
void LensMaskDraw(List* aList, Redraw aRedraw)
{
    if (!g_lensActive.load(std::memory_order_relaxed) || !g_ready)
        return;
    ListState s;
    {
        std::lock_guard lock(g_listMutex);
        auto it = g_lists.find(aList);
        if (it == g_lists.end() || !it->second.rtsKnown || it->second.rts.empty())
            return;
        s.pso = it->second.pso;
        s.rts = it->second.rts;
        s.hasDs = it->second.hasDs;
        s.ds = it->second.ds;
    }
    ID3D12PipelineState* twin = nullptr;
    {
        std::shared_lock lock(g_lensPsoMutex);
        auto it = g_lensPsos.find(s.pso);
        if (it == g_lensPsos.end())
            return;
        twin = it->second;
    }
    ID3D12Resource* target = nullptr;
    {
        std::shared_lock lock(g_rtvMutex);
        auto it = g_rtvs.find(s.rts[0].ptr);
        if (it == g_rtvs.end() || !it->second)
            return;
        target = it->second;
    }
    D3D12_RESOURCE_DESC td = target->GetDesc();
    std::lock_guard lock(g_maskMutex);
    if (!EnsureMask(aList, static_cast<UINT>(td.Width), td.Height))
        return;
    g_OMSetRenderTargets(aList, 1, &g_maskRtv, FALSE, nullptr);
    g_SetPipelineState(aList, twin);
    aRedraw();
    g_OMSetRenderTargets(aList, static_cast<UINT>(s.rts.size()), s.rts.data(), FALSE, s.hasDs ? &s.ds : nullptr);
    g_SetPipelineState(aList, static_cast<ID3D12PipelineState*>(s.pso));
    g_maskDrawn = true;
}

void STDMETHODCALLTYPE HkDrawInstanced(List* aSelf, UINT aVerts, UINT aInst, UINT aStartVert, UINT aStartInst)
{
    g_DrawInstanced(aSelf, aVerts, aInst, aStartVert, aStartInst);
    LensMaskDraw(aSelf, [&] { g_DrawInstanced(aSelf, aVerts, aInst, aStartVert, aStartInst); });
}

void STDMETHODCALLTYPE HkDrawIndexedInstanced(List* aSelf, UINT aIdx, UINT aInst, UINT aStartIdx, INT aBase,
                                              UINT aStartInst)
{
    g_DrawIndexedInstanced(aSelf, aIdx, aInst, aStartIdx, aBase, aStartInst);
    LensMaskDraw(aSelf, [&] { g_DrawIndexedInstanced(aSelf, aIdx, aInst, aStartIdx, aBase, aStartInst); });
}

void STDMETHODCALLTYPE HkDispatch(List* aSelf, UINT aX, UINT aY, UINT aZ)
{
    g_Dispatch(aSelf, aX, aY, aZ);
    void* pso;
    {
        std::lock_guard lock(g_listMutex);
        pso = g_lists[aSelf].pso;
    }
    bool overlay;
    {
        std::shared_lock lock(g_psoMutex);
        overlay = g_overlayPsos.count(pso) != 0;
    }
    if (overlay)
        AfterOverlay(aSelf);
}

// ---------------------------------------------------------------------------------------------
// Installing the hooks on the first real device.

std::atomic<bool> g_installed{false};

void InstallHooks(ID3D12Device* aDevice)
{
    g_device = aDevice;
    g_device->AddRef();
    g_viewStep = aDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll"))
        g_deserializer = reinterpret_cast<DeserializerFn>(
            GetProcAddress(d3d12, "D3D12CreateVersionedRootSignatureDeserializer"));

    g_rtvStep = aDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    bool ok = g_deserializer != nullptr;
    ok &= HookSlot(aDevice, kDevice_CreateGraphicsPipelineState, &HkCreateGraphicsPipelineState,
                   &g_CreateGraphicsPipelineState, "CreateGraphicsPipelineState");
    ok &= HookSlot(aDevice, kDevice_CreateRenderTargetView, &HkCreateRenderTargetView, &g_CreateRenderTargetView,
                   "CreateRenderTargetView");
    ok &= HookSlot(aDevice, kDevice_CreateComputePipelineState, &HkCreateComputePipelineState,
                   &g_CreateComputePipelineState, "CreateComputePipelineState");
    ok &= HookSlot(aDevice, kDevice_CreateDescriptorHeap, &HkCreateDescriptorHeap, &g_CreateDescriptorHeap,
                   "CreateDescriptorHeap");
    ok &= HookSlot(aDevice, kDevice_CreateRootSignature, &HkCreateRootSignature, &g_CreateRootSignature,
                   "CreateRootSignature");
    ok &= HookSlot(aDevice, kDevice_CreateUnorderedAccessView, &HkCreateUnorderedAccessView,
                   &g_CreateUnorderedAccessView, "CreateUnorderedAccessView");
    ok &= HookSlot(aDevice, kDevice_CopyDescriptors, &HkCopyDescriptors, &g_CopyDescriptors, "CopyDescriptors");
    ok &= HookSlot(aDevice, kDevice_CopyDescriptorsSimple, &HkCopyDescriptorsSimple, &g_CopyDescriptorsSimple,
                   "CopyDescriptorsSimple");

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(aDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(aDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))))
    {
        list->Close();
        void** vtable = *reinterpret_cast<void***>(list);
        g_CopyTextureRegion = reinterpret_cast<decltype(g_CopyTextureRegion)>(vtable[kList_CopyTextureRegion]);
        g_ClearRenderTargetView = reinterpret_cast<decltype(g_ClearRenderTargetView)>(vtable[kList_ClearRenderTargetView]);
        ok &= HookSlot(list, kList_DrawInstanced, &HkDrawInstanced, &g_DrawInstanced, "DrawInstanced");
        ok &= HookSlot(list, kList_DrawIndexedInstanced, &HkDrawIndexedInstanced, &g_DrawIndexedInstanced,
                       "DrawIndexedInstanced");
        ok &= HookSlot(list, kList_OMSetRenderTargets, &HkOMSetRenderTargets, &g_OMSetRenderTargets,
                       "OMSetRenderTargets");
        ok &= HookSlot(list, kList_Reset, &HkReset, &g_Reset, "Reset");
        ok &= HookSlot(list, kList_Dispatch, &HkDispatch, &g_Dispatch, "Dispatch");
        ok &= HookSlot(list, kList_SetPipelineState, &HkSetPipelineState, &g_SetPipelineState, "SetPipelineState");
        ok &= HookSlot(list, kList_ResourceBarrier, &HkResourceBarrier, &g_ResourceBarrier, "ResourceBarrier");
        ok &= HookSlot(list, kList_SetComputeRootSignature, &HkSetComputeRootSignature, &g_SetComputeRootSignature,
                       "SetComputeRootSignature");
        ok &= HookSlot(list, kList_SetComputeRootDescriptorTable, &HkSetComputeRootDescriptorTable,
                       &g_SetComputeRootDescriptorTable, "SetComputeRootDescriptorTable");
        ok &= HookSlot(list, kList_SetComputeRoot32BitConstant, &HkSetComputeRoot32BitConstant,
                       &g_SetComputeRoot32BitConstant, "SetComputeRoot32BitConstant");
        ok &= HookSlot(list, kList_SetComputeRoot32BitConstants, &HkSetComputeRoot32BitConstants,
                       &g_SetComputeRoot32BitConstants, "SetComputeRoot32BitConstants");
        ok &= HookSlot(list, kList_SetComputeRootConstantBufferView, &HkSetComputeRootConstantBufferView,
                       &g_SetComputeRootConstantBufferView, "SetComputeRootConstantBufferView");
        ok &= HookSlot(list, kList_SetComputeRootShaderResourceView, &HkSetComputeRootShaderResourceView,
                       &g_SetComputeRootShaderResourceView, "SetComputeRootShaderResourceView");
        ok &= HookSlot(list, kList_SetComputeRootUnorderedAccessView, &HkSetComputeRootUnorderedAccessView,
                       &g_SetComputeRootUnorderedAccessView, "SetComputeRootUnorderedAccessView");
    }
    else
        ok = false;
    if (list)
        list->Release();
    if (alloc)
        alloc->Release();

    g_ready = ok && CreateOurPass();
    Info("outline fix %s", g_ready ? "ready" : "OFF (setup failed)");
}

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
CreateDeviceFn g_CreateDevice = nullptr;

HRESULT WINAPI HkCreateDevice(IUnknown* aAdapter, D3D_FEATURE_LEVEL aLevel, REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateDevice(aAdapter, aLevel, aRiid, aOut);
    if (SUCCEEDED(hr) && aOut && *aOut && !g_installed.exchange(true))
    {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*aOut)->QueryInterface(IID_PPV_ARGS(&device))))
        {
            InstallHooks(device);
            device->Release();
        }
        else
            g_installed = false;
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Script functions

void OutlineLens(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool active = false;
    float zoom = 1.f;
    RED4ext::GetParameter(aFrame, &active);
    RED4ext::GetParameter(aFrame, &zoom);
    aFrame->code++;
    std::lock_guard lock(g_lensMutex);
    if (g_lens.active != active || g_lens.zoom != zoom)
        Info("lens %s, zoom %.2f", active ? "on" : "off", zoom);
    g_lens.active = active;
    g_lens.zoom = zoom;
    g_lensActive = active;
}

void OutlineTune(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    float rx = 0, ry = 0, factor = 0;
    RED4ext::GetParameter(aFrame, &rx);
    RED4ext::GetParameter(aFrame, &ry);
    RED4ext::GetParameter(aFrame, &factor);
    aFrame->code++;
    std::lock_guard lock(g_lensMutex);
    g_lens.radiusX = rx;
    g_lens.radiusY = ry;
    g_lens.factor = factor;
    Info("tune: radius %.3f x %.3f, factor %.3f", rx, ry, factor);
}

void OutlineCenter(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    float x = 0.5f, y = 0.5f;
    RED4ext::GetParameter(aFrame, &x);
    RED4ext::GetParameter(aFrame, &y);
    aFrame->code++;
    std::lock_guard lock(g_lensMutex);
    g_lens.centerX = x;
    g_lens.centerY = y;
    Info("centre %.3f, %.3f", x, y);
}

void OutlineStyle(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool outlineOnly = true;
    float cutoff = 0.9f;
    RED4ext::GetParameter(aFrame, &outlineOnly);
    RED4ext::GetParameter(aFrame, &cutoff);
    aFrame->code++;
    std::lock_guard lock(g_lensMutex);
    g_lens.outlineOnly = outlineOnly;
    g_lens.cutoff = std::clamp(cutoff, 0.01f, 1.f);
    Info("style: %s, cutoff %.2f", outlineOnly ? "outline only" : "vanilla (fill)", g_lens.cutoff);
}

void RegisterTypes()
{
}

void PostRegisterTypes()
{
    auto rtti = RED4ext::CRTTISystem::Get();
    const RED4ext::CBaseFunction::Flags flags = {.isNative = true, .isStatic = true};
    {
        auto f = RED4ext::CGlobalFunction::Create("SZL_OutlineLens", "SZL_OutlineLens", &OutlineLens);
        f->flags = flags;
        f->AddParam("Bool", "active");
        f->AddParam("Float", "zoom");
        rtti->RegisterFunction(f);
    }
    {
        auto f = RED4ext::CGlobalFunction::Create("SZL_OutlineTune", "SZL_OutlineTune", &OutlineTune);
        f->flags = flags;
        f->AddParam("Float", "radiusX");
        f->AddParam("Float", "radiusY");
        f->AddParam("Float", "factor");
        rtti->RegisterFunction(f);
    }
    {
        auto f = RED4ext::CGlobalFunction::Create("SZL_OutlineStyle", "SZL_OutlineStyle", &OutlineStyle);
        f->flags = flags;
        f->AddParam("Bool", "outlineOnly");
        f->AddParam("Float", "cutoff");
        rtti->RegisterFunction(f);
    }
    {
        auto f = RED4ext::CGlobalFunction::Create("SZL_OutlineCenter", "SZL_OutlineCenter", &OutlineCenter);
        f->flags = flags;
        f->AddParam("Float", "x");
        f->AddParam("Float", "y");
        rtti->RegisterFunction(f);
    }
}
} // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    if (aReason == RED4ext::v1::EMainReason::Load)
    {
        g_handle = aHandle;
        g_sdk = aSdk;
        auto rtti = RED4ext::CRTTISystem::Get();
        rtti->AddRegisterCallback(RegisterTypes);
        rtti->AddPostRegisterCallback(PostRegisterTypes);
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto target = d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
        bool ok = target && aSdk->hooking->Attach(aHandle, reinterpret_cast<void*>(target),
                                                  reinterpret_cast<void*>(&HkCreateDevice),
                                                  reinterpret_cast<void**>(&g_CreateDevice));
        Info("SZL Outline Fix 0.3.1 loaded; D3D12CreateDevice hook %s", ok ? "ok" : "FAILED");
    }
    else if (aReason == RED4ext::v1::EMainReason::Unload)
    {
        g_sdk = nullptr;
        g_handle = nullptr;
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"SZLOutlineFix";
    aInfo->author = L"oitenk";
    aInfo->version = RED4EXT_V1_SEMVER(0, 3, 1);
    aInfo->runtime = RED4EXT_V1_FILEVER(3, 0, 88, 46235);
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
