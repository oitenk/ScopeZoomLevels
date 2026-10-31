/* Vtable slots of the D3D12 / DXGI methods the inspector hooks, read from the Windows SDK's C
   interface (each interface's Vtbl struct lists its methods in vtable order). */
#define CINTERFACE
#include <stddef.h>
#include <d3d12.h>

#include "Vtbl.h"

#define SLOT(vt, m) ((int)(offsetof(vt, m) / sizeof(void*)))

const int kDevice_CreateCommandQueue = SLOT(ID3D12DeviceVtbl, CreateCommandQueue);
const int kDevice_CreateCommandAllocator = SLOT(ID3D12DeviceVtbl, CreateCommandAllocator);
const int kDevice_CreateGraphicsPipelineState = SLOT(ID3D12DeviceVtbl, CreateGraphicsPipelineState);
const int kDevice_CreateComputePipelineState = SLOT(ID3D12DeviceVtbl, CreateComputePipelineState);
const int kDevice_CreateCommandList = SLOT(ID3D12DeviceVtbl, CreateCommandList);
const int kDevice_CreateRenderTargetView = SLOT(ID3D12DeviceVtbl, CreateRenderTargetView);
const int kDevice_CreateDepthStencilView = SLOT(ID3D12DeviceVtbl, CreateDepthStencilView);
const int kDevice_CreateDescriptorHeap = SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap);
const int kDevice_CreateRootSignature = SLOT(ID3D12DeviceVtbl, CreateRootSignature);
const int kDevice_CreateShaderResourceView = SLOT(ID3D12DeviceVtbl, CreateShaderResourceView);
const int kDevice_CreateUnorderedAccessView = SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView);
const int kDevice_CopyDescriptors = SLOT(ID3D12DeviceVtbl, CopyDescriptors);
const int kDevice_CopyDescriptorsSimple = SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple);
const int kDevice1_CreatePipelineLibrary = SLOT(ID3D12Device1Vtbl, CreatePipelineLibrary);
const int kDevice2_CreatePipelineState = SLOT(ID3D12Device2Vtbl, CreatePipelineState);

const int kQueue_ExecuteCommandLists = SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists);

const int kLibrary_LoadGraphicsPipeline = SLOT(ID3D12PipelineLibraryVtbl, LoadGraphicsPipeline);
const int kLibrary_LoadComputePipeline = SLOT(ID3D12PipelineLibraryVtbl, LoadComputePipeline);
const int kLibrary1_LoadPipeline = SLOT(ID3D12PipelineLibrary1Vtbl, LoadPipeline);

const int kList_Close = SLOT(ID3D12GraphicsCommandList4Vtbl, Close);
const int kList_Reset = SLOT(ID3D12GraphicsCommandList4Vtbl, Reset);
const int kList_DrawInstanced = SLOT(ID3D12GraphicsCommandList4Vtbl, DrawInstanced);
const int kList_DrawIndexedInstanced = SLOT(ID3D12GraphicsCommandList4Vtbl, DrawIndexedInstanced);
const int kList_Dispatch = SLOT(ID3D12GraphicsCommandList4Vtbl, Dispatch);
const int kList_CopyTextureRegion = SLOT(ID3D12GraphicsCommandList4Vtbl, CopyTextureRegion);
const int kList_CopyResource = SLOT(ID3D12GraphicsCommandList4Vtbl, CopyResource);
const int kList_SetPipelineState = SLOT(ID3D12GraphicsCommandList4Vtbl, SetPipelineState);
const int kList_ResourceBarrier = SLOT(ID3D12GraphicsCommandList4Vtbl, ResourceBarrier);
const int kList_OMSetRenderTargets = SLOT(ID3D12GraphicsCommandList4Vtbl, OMSetRenderTargets);
const int kList_ClearDepthStencilView = SLOT(ID3D12GraphicsCommandList4Vtbl, ClearDepthStencilView);
const int kList_ClearRenderTargetView = SLOT(ID3D12GraphicsCommandList4Vtbl, ClearRenderTargetView);
const int kList_SetComputeRootSignature = SLOT(ID3D12GraphicsCommandList4Vtbl, SetComputeRootSignature);
const int kList_SetGraphicsRootSignature = SLOT(ID3D12GraphicsCommandList4Vtbl, SetGraphicsRootSignature);
const int kList_SetComputeRootDescriptorTable = SLOT(ID3D12GraphicsCommandList4Vtbl, SetComputeRootDescriptorTable);
const int kList_SetGraphicsRootDescriptorTable = SLOT(ID3D12GraphicsCommandList4Vtbl, SetGraphicsRootDescriptorTable);
const int kList_SetMarker =SLOT(ID3D12GraphicsCommandList4Vtbl, SetMarker);
const int kList_BeginEvent = SLOT(ID3D12GraphicsCommandList4Vtbl, BeginEvent);
const int kList_EndEvent = SLOT(ID3D12GraphicsCommandList4Vtbl, EndEvent);
const int kList_ExecuteIndirect = SLOT(ID3D12GraphicsCommandList4Vtbl, ExecuteIndirect);
const int kList_BeginRenderPass = SLOT(ID3D12GraphicsCommandList4Vtbl, BeginRenderPass);
const int kList_EndRenderPass = SLOT(ID3D12GraphicsCommandList4Vtbl, EndRenderPass);
