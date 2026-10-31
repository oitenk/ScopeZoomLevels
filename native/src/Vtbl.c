/* Vtable slots of the D3D12 methods the outline fix hooks, read from the Windows SDK's C
   interface (each interface's Vtbl struct lists its methods in vtable order). */
#define CINTERFACE
#include <stddef.h>
#include <d3d12.h>

#include "Vtbl.h"

#define SLOT(vt, m) ((int)(offsetof(vt, m) / sizeof(void*)))

const int kDevice_CreateComputePipelineState = SLOT(ID3D12DeviceVtbl, CreateComputePipelineState);
const int kDevice_CreateGraphicsPipelineState = SLOT(ID3D12DeviceVtbl, CreateGraphicsPipelineState);
const int kDevice_CreateRenderTargetView = SLOT(ID3D12DeviceVtbl, CreateRenderTargetView);
const int kDevice_CreateDescriptorHeap = SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap);
const int kDevice_CreateRootSignature = SLOT(ID3D12DeviceVtbl, CreateRootSignature);
const int kDevice_CreateUnorderedAccessView = SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView);
const int kDevice_CopyDescriptors = SLOT(ID3D12DeviceVtbl, CopyDescriptors);
const int kDevice_CopyDescriptorsSimple = SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple);

const int kList_Reset = SLOT(ID3D12GraphicsCommandListVtbl, Reset);
const int kList_Dispatch = SLOT(ID3D12GraphicsCommandListVtbl, Dispatch);
const int kList_SetPipelineState = SLOT(ID3D12GraphicsCommandListVtbl, SetPipelineState);
const int kList_DrawInstanced = SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced);
const int kList_DrawIndexedInstanced = SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced);
const int kList_OMSetRenderTargets = SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets);
const int kList_ClearRenderTargetView = SLOT(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView);
const int kList_CopyBufferRegion = SLOT(ID3D12GraphicsCommandListVtbl, CopyBufferRegion);
const int kList_CopyTextureRegion = SLOT(ID3D12GraphicsCommandListVtbl, CopyTextureRegion);
const int kList_ResourceBarrier = SLOT(ID3D12GraphicsCommandListVtbl, ResourceBarrier);
const int kList_SetComputeRootSignature = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature);
const int kList_SetComputeRootDescriptorTable = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable);
const int kList_SetComputeRoot32BitConstant = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstant);
const int kList_SetComputeRoot32BitConstants = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstants);
const int kList_SetComputeRootConstantBufferView = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView);
const int kList_SetComputeRootShaderResourceView = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView);
const int kList_SetComputeRootUnorderedAccessView = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView);
