#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const int kDevice_CreateComputePipelineState;
extern const int kDevice_CreateGraphicsPipelineState;
extern const int kDevice_CreateRenderTargetView;
extern const int kDevice_CreateDescriptorHeap;
extern const int kDevice_CreateRootSignature;
extern const int kDevice_CreateUnorderedAccessView;
extern const int kDevice_CopyDescriptors;
extern const int kDevice_CopyDescriptorsSimple;

extern const int kList_Reset;
extern const int kList_Dispatch;
extern const int kList_SetPipelineState;
extern const int kList_DrawInstanced;
extern const int kList_DrawIndexedInstanced;
extern const int kList_OMSetRenderTargets;
extern const int kList_ClearRenderTargetView;
extern const int kList_CopyTextureRegion;
extern const int kList_ResourceBarrier;
extern const int kList_SetComputeRootSignature;
extern const int kList_SetComputeRootDescriptorTable;
extern const int kList_SetComputeRoot32BitConstant;
extern const int kList_SetComputeRoot32BitConstants;
extern const int kList_SetComputeRootConstantBufferView;
extern const int kList_SetComputeRootShaderResourceView;
extern const int kList_SetComputeRootUnorderedAccessView;

#ifdef __cplusplus
}
#endif
