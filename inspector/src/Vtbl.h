#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const int kDevice_CreateCommandQueue;
extern const int kDevice_CreateCommandAllocator;
extern const int kDevice_CreateGraphicsPipelineState;
extern const int kDevice_CreateComputePipelineState;
extern const int kDevice_CreateCommandList;
extern const int kDevice_CreateRenderTargetView;
extern const int kDevice_CreateDepthStencilView;
extern const int kDevice1_CreatePipelineLibrary;
extern const int kDevice2_CreatePipelineState;

extern const int kQueue_ExecuteCommandLists;

extern const int kLibrary_LoadGraphicsPipeline;
extern const int kLibrary_LoadComputePipeline;
extern const int kLibrary1_LoadPipeline;

extern const int kList_Close;
extern const int kList_Reset;
extern const int kList_DrawInstanced;
extern const int kList_DrawIndexedInstanced;
extern const int kList_Dispatch;
extern const int kList_CopyTextureRegion;
extern const int kList_CopyResource;
extern const int kList_SetPipelineState;
extern const int kList_ResourceBarrier;
extern const int kList_OMSetRenderTargets;
extern const int kList_ClearDepthStencilView;
extern const int kList_ClearRenderTargetView;
extern const int kList_SetMarker;
extern const int kList_BeginEvent;
extern const int kList_EndEvent;
extern const int kList_ExecuteIndirect;
extern const int kList_BeginRenderPass;
extern const int kList_EndRenderPass;

#ifdef __cplusplus
}
#endif
