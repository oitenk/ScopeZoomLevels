// SZL Inspector - a diagnostic RED4ext plugin for the Scope Zoom Levels outline fix.
//
// It logs how the game draws a frame, from the inside, because frame capture tools can't see this
// game's D3D12 device (NVIDIA Streamline sits in between). Press Ctrl+F11 in game: the next two
// frames are written to red4ext\logs\szl-inspector-<time>.txt, in the order the GPU runs them:
// the game's render step labels (PIX markers), the pipeline (with shader hashes) and render
// targets of every draw and dispatch, clears, copies and resource transitions.
//
// How: the system d3d12.dll's D3D12CreateDevice is hooked when the plugin loads. On the first
// real device, the methods of the device, command queue, command list and pipeline library are
// hooked through their vtables (the slots come from Vtbl.c). Outside a capture the hooks only
// keep two small tables (pipeline -> shader hashes, render target view -> texture).
//
// It changes nothing in what the game draws. Not for release.

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/GpuApi/DeviceData.hpp>

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

std::string Fmt(const char* aFormat, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, aFormat);
    vsnprintf(buf, sizeof(buf), aFormat, args);
    va_end(args);
    return buf;
}

template<typename T>
bool HookSlot(void* aObject, int aSlot, T aDetour, T* aOriginal, const char* aName)
{
    void** vtable = *reinterpret_cast<void***>(aObject);
    void* target = vtable[aSlot];
    bool ok = g_sdk->hooking->Attach(g_handle, target, reinterpret_cast<void*>(aDetour),
                                     reinterpret_cast<void**>(aOriginal));
    Info("hook %s (slot %d, %p): %s", aName, aSlot, target, ok ? "ok" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Pipelines: shader hashes, so a pipeline can be recognised again in the next game session.

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

std::string ShaderHash(const D3D12_SHADER_BYTECODE& aCode)
{
    if (!aCode.pShaderBytecode || aCode.BytecodeLength == 0)
        return "-";
    return Fmt("%016llx", static_cast<unsigned long long>(Fnv1a(aCode.pShaderBytecode, aCode.BytecodeLength)));
}

struct PsoInfo
{
    uint32_t id = 0;
    std::string text;
};

std::shared_mutex g_psoMutex;
std::unordered_map<void*, PsoInfo> g_psos;
std::atomic<uint32_t> g_psoNext{1};

std::string Formats(UINT aCount, const DXGI_FORMAT* aFormats, DXGI_FORMAT aDepth)
{
    std::string s = "rt[";
    for (UINT i = 0; i < aCount && i < 8; ++i)
        s += Fmt(i ? ",%d" : "%d", aFormats[i]);
    s += Fmt("] ds%d", aDepth);
    return s;
}

void RegisterPso(void* aPso, const char* aKind, const std::string& aText)
{
    if (!aPso)
        return;
    std::unique_lock lock(g_psoMutex);
    g_psos[aPso] = PsoInfo{g_psoNext++, std::string(aKind) + " " + aText};
}

std::string GraphicsText(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d)
{
    return "vs=" + ShaderHash(d.VS) + " ps=" + ShaderHash(d.PS) + " " +
           Formats(d.NumRenderTargets, d.RTVFormats, d.DSVFormat);
}

// Pipeline state streams: walk the subobjects the way d3dx12's stream structs lay them out
// (each one pointer-aligned: the type, then its payload). The padding is the point here.
#pragma warning(disable : 4324)
template<typename Inner>
constexpr size_t SubSize()
{
    struct alignas(void*) S
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        Inner inner;
    };
    return sizeof(S);
}

template<typename Inner>
constexpr size_t SubOffset()
{
    struct alignas(void*) S
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        Inner inner;
    };
    return offsetof(S, inner);
}

std::string StreamText(const D3D12_PIPELINE_STATE_STREAM_DESC& aDesc)
{
    auto* p = static_cast<const uint8_t*>(aDesc.pPipelineStateSubobjectStream);
    auto* end = p + aDesc.SizeInBytes;
    std::string vs = "-", ps = "-", cs = "-", ms = "-", as = "-";
    D3D12_RT_FORMAT_ARRAY rts{};
    DXGI_FORMAT ds = DXGI_FORMAT_UNKNOWN;
    bool complete = true;
    while (p && p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end)
    {
        auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
        size_t size = 0;
        switch (type)
        {
#define SUB(T, Inner, Action)                                                                       \
    case T:                                                                                         \
    {                                                                                               \
        const auto& v = *reinterpret_cast<const Inner*>(p + SubOffset<Inner>());                    \
        (void)v;                                                                                    \
        Action;                                                                                     \
        size = SubSize<Inner>();                                                                    \
        break;                                                                                      \
    }
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, uintptr_t, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE, vs = ShaderHash(v))
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE, ps = ShaderHash(v))
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_SHADER_BYTECODE, cs = ShaderHash(v))
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE, as = ShaderHash(v))
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE, ms = ShaderHash(v))
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY, rts = v)
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT, ds = v)
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, D3D12_PIPELINE_STATE_FLAGS, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1, D3D12_RASTERIZER_DESC1, {})
            SUB(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, D3D12_RASTERIZER_DESC2, {})
#undef SUB
        default:
            complete = false;
            break;
        }
        if (!size)
            break;
        p += size;
    }
    std::string s = cs != "-" ? "cs=" + cs : "vs=" + vs + " ps=" + ps;
    if (ms != "-")
        s += " ms=" + ms + " as=" + as;
    if (cs == "-")
        s += " " + Formats(rts.NumRenderTargets, rts.RTFormats, ds);
    if (!complete)
        s += " (stream not fully read)";
    return s;
}

std::string PsoText(void* aPso)
{
    if (!aPso)
        return "PSO null";
    std::shared_lock lock(g_psoMutex);
    auto it = g_psos.find(aPso);
    if (it == g_psos.end())
        return Fmt("PSO %p (created before the hooks, or by an unhooked path)", aPso);
    return Fmt("PSO #%u %s", it->second.id, it->second.text.c_str());
}

// ---------------------------------------------------------------------------------------------
// Render target and depth views: CPU handle -> texture.

struct ViewInfo
{
    ID3D12Resource* resource = nullptr;
    int format = 0;
};

std::shared_mutex g_viewMutex;
std::unordered_map<SIZE_T, ViewInfo> g_views;
UINT g_rtvStep = 0;

std::string ResourceText(ID3D12Resource* aRes)
{
    if (!aRes)
        return "null";
    D3D12_RESOURCE_DESC d = aRes->GetDesc();
    wchar_t wname[128]{};
    UINT size = sizeof(wname) - sizeof(wchar_t);
    std::string name;
    if (SUCCEEDED(aRes->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, wname)))
    {
        for (wchar_t c : std::wstring(wname))
            name += (c > 31 && c < 127) ? static_cast<char>(c) : '?';
    }
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return Fmt("%p buffer %llu B%s%s", aRes, static_cast<unsigned long long>(d.Width), name.empty() ? "" : " ",
                   name.c_str());
    return Fmt("%p %llux%u%s fmt%d mips%u%s%s", aRes, static_cast<unsigned long long>(d.Width), d.Height,
               d.DepthOrArraySize > 1 ? Fmt("x%u", d.DepthOrArraySize).c_str() : "", d.Format, d.MipLevels,
               name.empty() ? "" : " ", name.c_str());
}

std::string ViewText(SIZE_T aHandle)
{
    std::shared_lock lock(g_viewMutex);
    auto it = g_views.find(aHandle);
    if (it == g_views.end())
        return Fmt("view %llx (unknown)", static_cast<unsigned long long>(aHandle));
    return Fmt("view fmt%d -> ", it->second.format) + ResourceText(it->second.resource);
}

// ---------------------------------------------------------------------------------------------
// Capture: per command list recording, written in submission order.

enum State : int
{
    Idle,
    Armed,
    Recording,
    Writing
};
std::atomic<int> g_state{Idle};

struct ListRecord
{
    std::vector<std::string> lines;
    bool fromReset = false;
    int depth = 0;
};

std::mutex g_listMutex;
std::unordered_map<void*, ListRecord> g_lists;
std::vector<std::string> g_out;
uint32_t g_submitCount = 0;
uint64_t g_armFrame = 0;
uint64_t g_startFrame = 0;
std::chrono::steady_clock::time_point g_startTime;
constexpr size_t MaxLines = 1500000;

inline bool Capturing()
{
    int s = g_state.load(std::memory_order_relaxed);
    return s == Armed || s == Recording;
}

void Add(void* aList, std::string aLine, int aDepthChange = 0)
{
    std::lock_guard lock(g_listMutex);
    auto& rec = g_lists[aList];
    if (aDepthChange < 0 && rec.depth > 0)
        rec.depth += aDepthChange;
    rec.lines.push_back(std::string(static_cast<size_t>(rec.depth) * 2, ' ') + aLine);
    if (aDepthChange > 0)
        rec.depth += aDepthChange;
}

std::string MarkerText(UINT aMeta, const void* aData, UINT aSize)
{
    if (!aData || !aSize)
        return "";
    auto* b = static_cast<const uint8_t*>(aData);
    auto ascii = [](const uint8_t* p, size_t n) {
        std::string s;
        for (size_t i = 0; i < n && p[i]; ++i)
            s += (p[i] > 31 && p[i] < 127) ? static_cast<char>(p[i]) : '?';
        return s;
    };
    auto wide = [](const uint8_t* p, size_t n) {
        std::string s;
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            uint16_t c = static_cast<uint16_t>(p[i] | (p[i + 1] << 8));
            if (!c)
                break;
            s += (c > 31 && c < 127) ? static_cast<char>(c) : '?';
        }
        return s;
    };
    if (aMeta == 0)
        return wide(b, aSize);
    if (aMeta == 1)
        return ascii(b, aSize);
    if (aMeta == 2 && aSize > 24)
    {
        // WinPixEventRuntime blob: event info, colour, string info, then the string itself.
        uint64_t info = *reinterpret_cast<const uint64_t*>(b + 16);
        bool isAnsi = (info >> 49) & 1;
        return isAnsi ? ascii(b + 24, aSize - 24) : wide(b + 24, aSize - 24);
    }
    std::string hex = Fmt("meta%u:", aMeta);
    for (UINT i = 0; i < aSize && i < 32; ++i)
        hex += Fmt("%02x", b[i]);
    return hex;
}

uint64_t CurrentFrame()
{
    auto* dd = RED4ext::GpuApi::GetDeviceData();
    if (!dd)
        return 0;
    for (auto& r : dd->swapChains.resources)
    {
        if (r.instance.swapChain)
            return r.instance.presentFenceNextCompletionFrame;
    }
    return 0;
}

std::filesystem::path LogsFolder()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path().parent_path().parent_path() / "red4ext" / "logs";
}

void WriteCapture()
{
    std::vector<std::string> out;
    {
        std::lock_guard lock(g_listMutex);
        out.swap(g_out);
        g_lists.clear();
    }
    size_t knownPsos;
    {
        std::shared_lock lock(g_psoMutex);
        knownPsos = g_psos.size();
    }
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    auto path = LogsFolder() / (std::string("szl-inspector-") + stamp + ".txt");
    {
        std::ofstream f(path);
        f << "SZL Inspector capture, frames " << g_startFrame << ".." << g_startFrame + 1 << ", " << g_submitCount
          << " submissions, " << knownPsos << " pipelines known, " << out.size() << " lines\n";
        for (auto& line : out)
            f << line << '\n';
    }
    Info("capture written: %s (%zu lines)", path.string().c_str(), out.size());
    g_state = Idle;
}

void OnSubmit(ID3D12CommandQueue* aQueue, UINT aCount, ID3D12CommandList* const* aLists)
{
    static bool keyWasDown = false;
    bool keyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
    uint64_t frame = CurrentFrame();

    if (keyDown && !keyWasDown && g_state == Idle)
    {
        std::lock_guard lock(g_listMutex);
        g_lists.clear();
        g_out.clear();
        g_submitCount = 0;
        g_armFrame = frame;
        g_state = Armed;
        Info("capture armed at frame %llu", static_cast<unsigned long long>(frame));
    }
    keyWasDown = keyDown;

    int state = g_state;
    if (state == Armed && frame != g_armFrame)
    {
        g_startFrame = frame;
        g_startTime = std::chrono::steady_clock::now();
        g_state = state = Recording;
    }
    if (state == Recording)
    {
        bool done = frame >= g_startFrame + 2 ||
                    std::chrono::steady_clock::now() - g_startTime > std::chrono::seconds(3);
        std::lock_guard lock(g_listMutex);
        if (done || g_out.size() > MaxLines)
        {
            g_state = Writing;
            std::thread(WriteCapture).detach();
            return;
        }
        D3D12_COMMAND_QUEUE_DESC qd = aQueue->GetDesc();
        const char* type = qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT    ? "direct"
                           : qd.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute"
                           : qd.Type == D3D12_COMMAND_LIST_TYPE_COPY    ? "copy"
                                                                        : "other";
        for (UINT i = 0; i < aCount; ++i)
        {
            auto it = g_lists.find(aLists[i]);
            g_out.push_back(Fmt("== frame %llu submit %u queue %p (%s) list %u/%u %p%s",
                                static_cast<unsigned long long>(frame), g_submitCount, aQueue, type, i + 1, aCount,
                                aLists[i],
                                it == g_lists.end()        ? " (nothing recorded)"
                                : !it->second.fromReset    ? " (recording started mid-list)"
                                                           : ""));
            if (it != g_lists.end())
            {
                for (auto& line : it->second.lines)
                    g_out.push_back(std::move(line));
                g_lists.erase(it);
            }
        }
        ++g_submitCount;
    }
    else if (state == Armed)
    {
        // Lists from the frame before the capture: drop them.
        std::lock_guard lock(g_listMutex);
        for (UINT i = 0; i < aCount; ++i)
            g_lists.erase(aLists[i]);
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks

#define ORIG(name) decltype(&Hk##name) g_##name = nullptr

// Device
HRESULT STDMETHODCALLTYPE HkCreateGraphicsPipelineState(ID3D12Device* aSelf, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* aDesc,
                                                        REFIID aRiid, void** aOut);
ORIG(CreateGraphicsPipelineState);
HRESULT STDMETHODCALLTYPE HkCreateGraphicsPipelineState(ID3D12Device* aSelf, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* aDesc,
                                                        REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateGraphicsPipelineState(aSelf, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut)
        RegisterPso(*aOut, "gfx", GraphicsText(*aDesc));
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateComputePipelineState(ID3D12Device* aSelf, const D3D12_COMPUTE_PIPELINE_STATE_DESC* aDesc,
                                                       REFIID aRiid, void** aOut);
ORIG(CreateComputePipelineState);
HRESULT STDMETHODCALLTYPE HkCreateComputePipelineState(ID3D12Device* aSelf, const D3D12_COMPUTE_PIPELINE_STATE_DESC* aDesc,
                                                       REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreateComputePipelineState(aSelf, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut)
        RegisterPso(*aOut, "cmp", "cs=" + ShaderHash(aDesc->CS));
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreatePipelineState(ID3D12Device2* aSelf, const D3D12_PIPELINE_STATE_STREAM_DESC* aDesc,
                                                REFIID aRiid, void** aOut);
ORIG(CreatePipelineState);
HRESULT STDMETHODCALLTYPE HkCreatePipelineState(ID3D12Device2* aSelf, const D3D12_PIPELINE_STATE_STREAM_DESC* aDesc,
                                                REFIID aRiid, void** aOut)
{
    HRESULT hr = g_CreatePipelineState(aSelf, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut)
        RegisterPso(*aOut, "stream", StreamText(*aDesc));
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
    std::unique_lock lock(g_viewMutex);
    g_views[aHandle.ptr] = ViewInfo{aRes, aDesc ? static_cast<int>(aDesc->Format) : -1};
}

void STDMETHODCALLTYPE HkCreateDepthStencilView(ID3D12Device* aSelf, ID3D12Resource* aRes,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* aDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE aHandle);
ORIG(CreateDepthStencilView);
void STDMETHODCALLTYPE HkCreateDepthStencilView(ID3D12Device* aSelf, ID3D12Resource* aRes,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* aDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE aHandle)
{
    g_CreateDepthStencilView(aSelf, aRes, aDesc, aHandle);
    std::unique_lock lock(g_viewMutex);
    g_views[aHandle.ptr] = ViewInfo{aRes, aDesc ? static_cast<int>(aDesc->Format) : -1};
}

// Pipeline library
HRESULT STDMETHODCALLTYPE HkLoadGraphicsPipeline(ID3D12PipelineLibrary* aSelf, LPCWSTR aName,
                                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC* aDesc, REFIID aRiid,
                                                 void** aOut);
ORIG(LoadGraphicsPipeline);
HRESULT STDMETHODCALLTYPE HkLoadGraphicsPipeline(ID3D12PipelineLibrary* aSelf, LPCWSTR aName,
                                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC* aDesc, REFIID aRiid,
                                                 void** aOut)
{
    HRESULT hr = g_LoadGraphicsPipeline(aSelf, aName, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut)
        RegisterPso(*aOut, "gfx(lib)", GraphicsText(*aDesc));
    return hr;
}

HRESULT STDMETHODCALLTYPE HkLoadComputePipeline(ID3D12PipelineLibrary* aSelf, LPCWSTR aName,
                                                const D3D12_COMPUTE_PIPELINE_STATE_DESC* aDesc, REFIID aRiid,
                                                void** aOut);
ORIG(LoadComputePipeline);
HRESULT STDMETHODCALLTYPE HkLoadComputePipeline(ID3D12PipelineLibrary* aSelf, LPCWSTR aName,
                                                const D3D12_COMPUTE_PIPELINE_STATE_DESC* aDesc, REFIID aRiid,
                                                void** aOut)
{
    HRESULT hr = g_LoadComputePipeline(aSelf, aName, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut)
        RegisterPso(*aOut, "cmp(lib)", "cs=" + ShaderHash(aDesc->CS));
    return hr;
}

HRESULT STDMETHODCALLTYPE HkLoadPipeline(ID3D12PipelineLibrary1* aSelf, LPCWSTR aName,
                                         const D3D12_PIPELINE_STATE_STREAM_DESC* aDesc, REFIID aRiid, void** aOut);
ORIG(LoadPipeline);
HRESULT STDMETHODCALLTYPE HkLoadPipeline(ID3D12PipelineLibrary1* aSelf, LPCWSTR aName,
                                         const D3D12_PIPELINE_STATE_STREAM_DESC* aDesc, REFIID aRiid, void** aOut)
{
    HRESULT hr = g_LoadPipeline(aSelf, aName, aDesc, aRiid, aOut);
    if (SUCCEEDED(hr) && aDesc && aOut && *aOut)
        RegisterPso(*aOut, "stream(lib)", StreamText(*aDesc));
    return hr;
}

// Queue
void STDMETHODCALLTYPE HkExecuteCommandLists(ID3D12CommandQueue* aSelf, UINT aCount, ID3D12CommandList* const* aLists);
ORIG(ExecuteCommandLists);
void STDMETHODCALLTYPE HkExecuteCommandLists(ID3D12CommandQueue* aSelf, UINT aCount, ID3D12CommandList* const* aLists)
{
    OnSubmit(aSelf, aCount, aLists);
    g_ExecuteCommandLists(aSelf, aCount, aLists);
}

// Command list
using List = ID3D12GraphicsCommandList;

HRESULT STDMETHODCALLTYPE HkReset(List* aSelf, ID3D12CommandAllocator* aAlloc, ID3D12PipelineState* aPso);
ORIG(Reset);
HRESULT STDMETHODCALLTYPE HkReset(List* aSelf, ID3D12CommandAllocator* aAlloc, ID3D12PipelineState* aPso)
{
    if (Capturing())
    {
        {
            std::lock_guard lock(g_listMutex);
            g_lists[aSelf] = ListRecord{{}, true, 0};
        }
        if (aPso)
            Add(aSelf, "Reset with " + PsoText(aPso));
    }
    return g_Reset(aSelf, aAlloc, aPso);
}

void STDMETHODCALLTYPE HkSetPipelineState(List* aSelf, ID3D12PipelineState* aPso);
ORIG(SetPipelineState);
void STDMETHODCALLTYPE HkSetPipelineState(List* aSelf, ID3D12PipelineState* aPso)
{
    if (Capturing())
        Add(aSelf, PsoText(aPso));
    g_SetPipelineState(aSelf, aPso);
}

void STDMETHODCALLTYPE HkDrawInstanced(List* aSelf, UINT aVerts, UINT aInst, UINT aStartVert, UINT aStartInst);
ORIG(DrawInstanced);
void STDMETHODCALLTYPE HkDrawInstanced(List* aSelf, UINT aVerts, UINT aInst, UINT aStartVert, UINT aStartInst)
{
    if (Capturing())
        Add(aSelf, Fmt("Draw verts=%u inst=%u", aVerts, aInst));
    g_DrawInstanced(aSelf, aVerts, aInst, aStartVert, aStartInst);
}

void STDMETHODCALLTYPE HkDrawIndexedInstanced(List* aSelf, UINT aIdx, UINT aInst, UINT aStartIdx, INT aBase,
                                              UINT aStartInst);
ORIG(DrawIndexedInstanced);
void STDMETHODCALLTYPE HkDrawIndexedInstanced(List* aSelf, UINT aIdx, UINT aInst, UINT aStartIdx, INT aBase,
                                              UINT aStartInst)
{
    if (Capturing())
        Add(aSelf, Fmt("DrawIndexed idx=%u inst=%u", aIdx, aInst));
    g_DrawIndexedInstanced(aSelf, aIdx, aInst, aStartIdx, aBase, aStartInst);
}

void STDMETHODCALLTYPE HkDispatch(List* aSelf, UINT aX, UINT aY, UINT aZ);
ORIG(Dispatch);
void STDMETHODCALLTYPE HkDispatch(List* aSelf, UINT aX, UINT aY, UINT aZ)
{
    if (Capturing())
        Add(aSelf, Fmt("Dispatch %u x %u x %u", aX, aY, aZ));
    g_Dispatch(aSelf, aX, aY, aZ);
}

void STDMETHODCALLTYPE HkExecuteIndirect(List* aSelf, ID3D12CommandSignature* aSig, UINT aMax, ID3D12Resource* aArgs,
                                         UINT64 aArgsOffset, ID3D12Resource* aCount, UINT64 aCountOffset);
ORIG(ExecuteIndirect);
void STDMETHODCALLTYPE HkExecuteIndirect(List* aSelf, ID3D12CommandSignature* aSig, UINT aMax, ID3D12Resource* aArgs,
                                         UINT64 aArgsOffset, ID3D12Resource* aCount, UINT64 aCountOffset)
{
    if (Capturing())
        Add(aSelf, Fmt("ExecuteIndirect max=%u", aMax));
    g_ExecuteIndirect(aSelf, aSig, aMax, aArgs, aArgsOffset, aCount, aCountOffset);
}

void STDMETHODCALLTYPE HkCopyResource(List* aSelf, ID3D12Resource* aDst, ID3D12Resource* aSrc);
ORIG(CopyResource);
void STDMETHODCALLTYPE HkCopyResource(List* aSelf, ID3D12Resource* aDst, ID3D12Resource* aSrc)
{
    if (Capturing())
        Add(aSelf, "CopyResource " + ResourceText(aSrc) + " -> " + ResourceText(aDst));
    g_CopyResource(aSelf, aDst, aSrc);
}

void STDMETHODCALLTYPE HkCopyTextureRegion(List* aSelf, const D3D12_TEXTURE_COPY_LOCATION* aDst, UINT aX, UINT aY, UINT aZ,
                                           const D3D12_TEXTURE_COPY_LOCATION* aSrc, const D3D12_BOX* aBox);
ORIG(CopyTextureRegion);
void STDMETHODCALLTYPE HkCopyTextureRegion(List* aSelf, const D3D12_TEXTURE_COPY_LOCATION* aDst, UINT aX, UINT aY, UINT aZ,
                                           const D3D12_TEXTURE_COPY_LOCATION* aSrc, const D3D12_BOX* aBox)
{
    if (Capturing() && aDst && aSrc)
        Add(aSelf, "CopyTextureRegion " + ResourceText(aSrc->pResource) + " -> " + ResourceText(aDst->pResource));
    g_CopyTextureRegion(aSelf, aDst, aX, aY, aZ, aSrc, aBox);
}

const char* StateName(D3D12_RESOURCE_STATES s)
{
    switch (s)
    {
    case D3D12_RESOURCE_STATE_COMMON: return "common";
    case D3D12_RESOURCE_STATE_RENDER_TARGET: return "rt";
    case D3D12_RESOURCE_STATE_UNORDERED_ACCESS: return "uav";
    case D3D12_RESOURCE_STATE_DEPTH_WRITE: return "depthW";
    case D3D12_RESOURCE_STATE_DEPTH_READ: return "depthR";
    case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE: return "srvPS";
    case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE: return "srvNonPS";
    case D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE: return "srvAll";
    case D3D12_RESOURCE_STATE_COPY_DEST: return "copyDst";
    case D3D12_RESOURCE_STATE_COPY_SOURCE: return "copySrc";
    case D3D12_RESOURCE_STATE_GENERIC_READ: return "read";
    default: return nullptr;
    }
}

std::string StateText(D3D12_RESOURCE_STATES s)
{
    const char* n = StateName(s);
    return n ? n : Fmt("0x%x", static_cast<unsigned>(s));
}

void STDMETHODCALLTYPE HkResourceBarrier(List* aSelf, UINT aCount, const D3D12_RESOURCE_BARRIER* aBarriers);
ORIG(ResourceBarrier);
void STDMETHODCALLTYPE HkResourceBarrier(List* aSelf, UINT aCount, const D3D12_RESOURCE_BARRIER* aBarriers)
{
    if (Capturing() && aBarriers)
    {
        UINT other = 0;
        for (UINT i = 0; i < aCount; ++i)
        {
            const auto& b = aBarriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                Add(aSelf, "Barrier " + ResourceText(b.Transition.pResource) + " " +
                               StateText(b.Transition.StateBefore) + " -> " + StateText(b.Transition.StateAfter));
            else
                ++other;
        }
        if (other)
            Add(aSelf, Fmt("Barrier x%u (uav/aliasing)", other));
    }
    g_ResourceBarrier(aSelf, aCount, aBarriers);
}

void STDMETHODCALLTYPE HkOMSetRenderTargets(List* aSelf, UINT aCount, const D3D12_CPU_DESCRIPTOR_HANDLE* aRts,
                                            BOOL aSingleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* aDs);
ORIG(OMSetRenderTargets);
void STDMETHODCALLTYPE HkOMSetRenderTargets(List* aSelf, UINT aCount, const D3D12_CPU_DESCRIPTOR_HANDLE* aRts,
                                            BOOL aSingleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* aDs)
{
    if (Capturing())
    {
        Add(aSelf, Fmt("SetRenderTargets %u", aCount));
        for (UINT i = 0; i < aCount && aRts; ++i)
        {
            SIZE_T h = aSingleRange ? aRts[0].ptr + static_cast<SIZE_T>(i) * g_rtvStep : aRts[i].ptr;
            Add(aSelf, Fmt("  RT%u ", i) + ViewText(h));
        }
        if (aDs)
            Add(aSelf, "  DS " + ViewText(aDs->ptr));
    }
    g_OMSetRenderTargets(aSelf, aCount, aRts, aSingleRange, aDs);
}

void STDMETHODCALLTYPE HkClearRenderTargetView(List* aSelf, D3D12_CPU_DESCRIPTOR_HANDLE aRt, const FLOAT aColor[4],
                                               UINT aRects, const D3D12_RECT* aRect);
ORIG(ClearRenderTargetView);
void STDMETHODCALLTYPE HkClearRenderTargetView(List* aSelf, D3D12_CPU_DESCRIPTOR_HANDLE aRt, const FLOAT aColor[4],
                                               UINT aRects, const D3D12_RECT* aRect)
{
    if (Capturing())
        Add(aSelf, "ClearRT " + ViewText(aRt.ptr));
    g_ClearRenderTargetView(aSelf, aRt, aColor, aRects, aRect);
}

void STDMETHODCALLTYPE HkClearDepthStencilView(List* aSelf, D3D12_CPU_DESCRIPTOR_HANDLE aDs, D3D12_CLEAR_FLAGS aFlags,
                                               FLOAT aDepth, UINT8 aStencil, UINT aRects, const D3D12_RECT* aRect);
ORIG(ClearDepthStencilView);
void STDMETHODCALLTYPE HkClearDepthStencilView(List* aSelf, D3D12_CPU_DESCRIPTOR_HANDLE aDs, D3D12_CLEAR_FLAGS aFlags,
                                               FLOAT aDepth, UINT8 aStencil, UINT aRects, const D3D12_RECT* aRect)
{
    if (Capturing())
        Add(aSelf, Fmt("ClearDS flags=%d stencil=%u ", aFlags, aStencil) + ViewText(aDs.ptr));
    g_ClearDepthStencilView(aSelf, aDs, aFlags, aDepth, aStencil, aRects, aRect);
}

void STDMETHODCALLTYPE HkSetMarker(List* aSelf, UINT aMeta, const void* aData, UINT aSize);
ORIG(SetMarker);
void STDMETHODCALLTYPE HkSetMarker(List* aSelf, UINT aMeta, const void* aData, UINT aSize)
{
    if (Capturing())
        Add(aSelf, "* " + MarkerText(aMeta, aData, aSize));
    g_SetMarker(aSelf, aMeta, aData, aSize);
}

void STDMETHODCALLTYPE HkBeginEvent(List* aSelf, UINT aMeta, const void* aData, UINT aSize);
ORIG(BeginEvent);
void STDMETHODCALLTYPE HkBeginEvent(List* aSelf, UINT aMeta, const void* aData, UINT aSize)
{
    if (Capturing())
        Add(aSelf, "> " + MarkerText(aMeta, aData, aSize), +1);
    g_BeginEvent(aSelf, aMeta, aData, aSize);
}

void STDMETHODCALLTYPE HkEndEvent(List* aSelf);
ORIG(EndEvent);
void STDMETHODCALLTYPE HkEndEvent(List* aSelf)
{
    if (Capturing())
        Add(aSelf, "<", -1);
    g_EndEvent(aSelf);
}

void STDMETHODCALLTYPE HkBeginRenderPass(ID3D12GraphicsCommandList4* aSelf, UINT aCount,
                                         const D3D12_RENDER_PASS_RENDER_TARGET_DESC* aRts,
                                         const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* aDs,
                                         D3D12_RENDER_PASS_FLAGS aFlags);
ORIG(BeginRenderPass);
void STDMETHODCALLTYPE HkBeginRenderPass(ID3D12GraphicsCommandList4* aSelf, UINT aCount,
                                         const D3D12_RENDER_PASS_RENDER_TARGET_DESC* aRts,
                                         const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* aDs,
                                         D3D12_RENDER_PASS_FLAGS aFlags)
{
    if (Capturing())
    {
        Add(aSelf, Fmt("BeginRenderPass %u", aCount), +1);
        for (UINT i = 0; i < aCount && aRts; ++i)
            Add(aSelf, Fmt("RT%u ", i) + ViewText(aRts[i].cpuDescriptor.ptr));
        if (aDs)
            Add(aSelf, "DS " + ViewText(aDs->cpuDescriptor.ptr));
    }
    g_BeginRenderPass(aSelf, aCount, aRts, aDs, aFlags);
}

void STDMETHODCALLTYPE HkEndRenderPass(ID3D12GraphicsCommandList4* aSelf);
ORIG(EndRenderPass);
void STDMETHODCALLTYPE HkEndRenderPass(ID3D12GraphicsCommandList4* aSelf)
{
    if (Capturing())
        Add(aSelf, "EndRenderPass", -1);
    g_EndRenderPass(aSelf);
}

// ---------------------------------------------------------------------------------------------
// Installing the hooks on the first real device.

std::atomic<bool> g_installed{false};

void InstallHooks(ID3D12Device* aDevice)
{
    Info("installing hooks on device %p", aDevice);
    g_rtvStep = aDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    HookSlot(aDevice, kDevice_CreateGraphicsPipelineState, &HkCreateGraphicsPipelineState,
             &g_CreateGraphicsPipelineState, "CreateGraphicsPipelineState");
    HookSlot(aDevice, kDevice_CreateComputePipelineState, &HkCreateComputePipelineState,
             &g_CreateComputePipelineState, "CreateComputePipelineState");
    HookSlot(aDevice, kDevice_CreateRenderTargetView, &HkCreateRenderTargetView, &g_CreateRenderTargetView,
             "CreateRenderTargetView");
    HookSlot(aDevice, kDevice_CreateDepthStencilView, &HkCreateDepthStencilView, &g_CreateDepthStencilView,
             "CreateDepthStencilView");

    ID3D12Device2* device2 = nullptr;
    if (SUCCEEDED(aDevice->QueryInterface(IID_PPV_ARGS(&device2))))
    {
        HookSlot(device2, kDevice2_CreatePipelineState, &HkCreatePipelineState, &g_CreatePipelineState,
                 "CreatePipelineState");
        device2->Release();
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(aDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
    {
        HookSlot(queue, kQueue_ExecuteCommandLists, &HkExecuteCommandLists, &g_ExecuteCommandLists,
                 "ExecuteCommandLists");
        queue->Release();
    }
    else
        Info("could not create a queue to hook");

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList4* list = nullptr;
    if (SUCCEEDED(aDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(aDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))))
    {
        list->Close();
        HookSlot(list, kList_Reset, &HkReset, &g_Reset, "Reset");
        HookSlot(list, kList_SetPipelineState, &HkSetPipelineState, &g_SetPipelineState, "SetPipelineState");
        HookSlot(list, kList_DrawInstanced, &HkDrawInstanced, &g_DrawInstanced, "DrawInstanced");
        HookSlot(list, kList_DrawIndexedInstanced, &HkDrawIndexedInstanced, &g_DrawIndexedInstanced,
                 "DrawIndexedInstanced");
        HookSlot(list, kList_Dispatch, &HkDispatch, &g_Dispatch, "Dispatch");
        HookSlot(list, kList_ExecuteIndirect, &HkExecuteIndirect, &g_ExecuteIndirect, "ExecuteIndirect");
        HookSlot(list, kList_CopyResource, &HkCopyResource, &g_CopyResource, "CopyResource");
        HookSlot(list, kList_CopyTextureRegion, &HkCopyTextureRegion, &g_CopyTextureRegion, "CopyTextureRegion");
        HookSlot(list, kList_ResourceBarrier, &HkResourceBarrier, &g_ResourceBarrier, "ResourceBarrier");
        HookSlot(list, kList_OMSetRenderTargets, &HkOMSetRenderTargets, &g_OMSetRenderTargets, "OMSetRenderTargets");
        HookSlot(list, kList_ClearRenderTargetView, &HkClearRenderTargetView, &g_ClearRenderTargetView,
                 "ClearRenderTargetView");
        HookSlot(list, kList_ClearDepthStencilView, &HkClearDepthStencilView, &g_ClearDepthStencilView,
                 "ClearDepthStencilView");
        HookSlot(list, kList_SetMarker, &HkSetMarker, &g_SetMarker, "SetMarker");
        HookSlot(list, kList_BeginEvent, &HkBeginEvent, &g_BeginEvent, "BeginEvent");
        HookSlot(list, kList_EndEvent, &HkEndEvent, &g_EndEvent, "EndEvent");
        HookSlot(list, kList_BeginRenderPass, &HkBeginRenderPass, &g_BeginRenderPass, "BeginRenderPass");
        HookSlot(list, kList_EndRenderPass, &HkEndRenderPass, &g_EndRenderPass, "EndRenderPass");
    }
    else
        Info("could not create a command list to hook");
    if (list)
        list->Release();
    if (alloc)
        alloc->Release();

    ID3D12Device1* device1 = nullptr;
    ID3D12PipelineLibrary1* library = nullptr;
    if (SUCCEEDED(aDevice->QueryInterface(IID_PPV_ARGS(&device1))) &&
        SUCCEEDED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library))))
    {
        HookSlot(library, kLibrary_LoadGraphicsPipeline, &HkLoadGraphicsPipeline, &g_LoadGraphicsPipeline,
                 "LoadGraphicsPipeline");
        HookSlot(library, kLibrary_LoadComputePipeline, &HkLoadComputePipeline, &g_LoadComputePipeline,
                 "LoadComputePipeline");
        HookSlot(library, kLibrary1_LoadPipeline, &HkLoadPipeline, &g_LoadPipeline, "LoadPipeline");
    }
    else
        Info("no pipeline library to hook (not supported here)");
    if (library)
        library->Release();
    if (device1)
        device1->Release();
    Info("hooks installed; press Ctrl+F11 in game to log two frames");
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
} // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    if (aReason == RED4ext::v1::EMainReason::Load)
    {
        g_handle = aHandle;
        g_sdk = aSdk;
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto target = d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
        bool ok = target && aSdk->hooking->Attach(aHandle, reinterpret_cast<void*>(target),
                                                  reinterpret_cast<void*>(&HkCreateDevice),
                                                  reinterpret_cast<void**>(&g_CreateDevice));
        Info("SZL Inspector 0.1.0 loaded; D3D12CreateDevice hook %s", ok ? "ok" : "FAILED");
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
    aInfo->name = L"SZLInspector";
    aInfo->author = L"oitenk";
    aInfo->version = RED4EXT_V1_SEMVER(0, 1, 0);
    aInfo->runtime = RED4EXT_V1_FILEVER(3, 0, 88, 46235);
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
