// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Session.h"
#include "Runtime.h"
#include "MotionShaders.h"
#include "GpuBankTimeline.h"
#include "DLSS/DlssOpticalFlow.h"
#include "DLSS/ZipDepth.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <mutex>
#include <stdexcept>
#include <source_location>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>
using Microsoft::WRL::ComPtr;
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"d3dcompiler.lib")
namespace mfg {
namespace {
// Optional environment override: MPCVR_MFG_MAX_HEIGHT=1080, 1440, 2160,
// or 0 to disable the cap. By default the MFG session keeps the full
// upstream size instead of clamping to a fixed manager-style height.
// Read once per newly created MFG session.
UINT MaxGenerationHeight() {
    wchar_t text[16]{};
    const DWORD length=GetEnvironmentVariableW(L"MPCVR_MFG_MAX_HEIGHT",text,_countof(text));
    if(!length || length>=_countof(text)) return std::numeric_limits<UINT>::max();
    wchar_t* end=nullptr;
    const unsigned long value=std::wcstoul(text,&end,10);
    if(end==text || *end!=L'\0') return std::numeric_limits<UINT>::max();
    if(value==0) return std::numeric_limits<UINT>::max();
    return value>=360 && value<=8192 ? static_cast<UINT>(value) : std::numeric_limits<UINT>::max();
}
bool AsyncProducerEnabled() {
    static const bool enabled=[] {
        wchar_t value[8]{};
        const DWORD n=GetEnvironmentVariableW(L"MPCVR_MFG_ASYNC_PRODUCER",value,_countof(value));
        if(n) return value[0]!=L'0';
        // Disabling the Live Flow group scheduler is the complete rollback
        // switch: use the old synchronous producer with the old flat queue.
        wchar_t group[8]{};
        return GetEnvironmentVariableW(L"MPCVR_MFG_LIVEFLOW_CORE",group,_countof(group))==0 ||
               group[0]!=L'0';
    }();
    return enabled;
}
void Check(HRESULT hr,const std::source_location& at=std::source_location::current()) {
    if(FAILED(hr)) throw std::runtime_error(std::format("MFG HRESULT 0x{:08X} at Session.cpp:{}",unsigned(hr),at.line()));
}
void Transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition.pResource=r;
    v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;v.Transition.StateBefore=a;v.Transition.StateAfter=b;l->ResourceBarrier(1,&v);
}
struct Shared {
    ComPtr<ID3D12Resource> d12;ComPtr<ID3D11Texture2D> d11;
    ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;
    void Create(ID3D12Device* d,ID3D11Device1* v,UINT w,UINT h,DXGI_FORMAT format,bool output) {
        // Match Plus's proven D3D11-origin NT-handle sharing path.
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;td.Format=format;
        td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(output)td.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        const auto created=v->CreateTexture2D(&td,nullptr,&d11);
        if(FAILED(created))throw std::runtime_error(std::format("Shared D3D11 texture format={} bind=0x{:X} misc=0x{:X}: 0x{:08X}",unsigned(format),td.BindFlags,td.MiscFlags,unsigned(created)));
        ComPtr<IDXGIResource1> resource;Check(d11.As(&resource));
        HANDLE handle=nullptr;Check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle));
        const HRESULT hr=d->OpenSharedHandle(handle,IID_PPV_ARGS(&d12));CloseHandle(handle);Check(hr);
        Check(v->CreateShaderResourceView(d11.Get(),nullptr,&srv));Check(v->CreateRenderTargetView(d11.Get(),nullptr,&rtv));
    }
};
ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;
    rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.Flags=flags;
    ComPtr<ID3D12Resource> r;Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
}
}
struct Session::Impl {
    ComPtr<ID3D11Device1> device11;ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12Device> device12;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    // Keep D3D11->D3D12 input and D3D12->CPU completion timelines separate.
    // On one shared fence an input signal could jump past a still-running
    // D3D12 completion value, making an unfinished bank look reusable.
    ComPtr<ID3D12Fence> fence,inputFence,presentFence;
    ComPtr<ID3D11Fence> inputFence11,presentFence11;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> copy,luma,vectors;ComPtr<ID3D11Buffer> constants;
    // Real frames retain the full swapchain resolution, while NGX works at a
    // capped resolution.  One native copy per bank also acts as the source SRV.
    // Do not reuse a single sourceCopy for frames still queued to Present.
    struct Bank {
        std::shared_ptr<OriginalSnapshot> original;
        Shared real,depth,motion;
        std::array<Shared,MaxGenerated> generated;
        std::array<ComPtr<ID3D12Resource>,MaxGenerated> disabled;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t serial=0;
        GpuBankTimeline timeline;
        unsigned generatedCount=0;
        bool checked=false,valid=false,multiplierRetry=false;
    };
    std::array<Bank,4> banks;
    unsigned activeBanks=3; // 2/3/4 selectable, 4 supports longer Live Flow group retention.
    ComPtr<ID3D12Resource> zero;
    CDlssOpticalFlow flow;
    CZipDepthHistory zipHistory;
    bool zipDepthActive = false;
    UINT w=0,h=0,sourceW=0,sourceH=0,multiplier=0,allocated=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC originalDesc{};
    std::shared_ptr<OriginalSnapshot> NewOriginal() {
        auto image=std::make_shared<OriginalSnapshot>();
        Check(device11->CreateTexture2D(&originalDesc,nullptr,&image->texture));
        Check(device11->CreateShaderResourceView(image->texture.Get(),nullptr,&image->view));
        return image;
    }
    // A queued source frame is holding the old texture: allocate a fresh one
    // rather than copying over pixels which have not been presented yet.
    void Capture(Bank& bank, ID3D11Texture2D* image) {
        if(!bank.original || bank.original.use_count() != 1) bank.original=NewOriginal();
        context->CopyResource(bank.original->texture.Get(),image);
    }
    uint64_t serial=0,value=0,pendingValue=0,inputValue=0,presentValue=0;
    int64_t previous=-1;
    FlowMotionCheck motionCheck;
    bool pending=false,asyncProducer=AsyncProducerEnabled();HANDLE event=nullptr;
    uint64_t busyFallbacks=0;
    Runtime runtime; // destroyed before resources, after caller has drained GPU.
    // Retain pending sessions until BOTH the producer and D3D11 presenter are
    // finished. A seek or resize can retire another session before that happens.
    inline static std::vector<std::unique_ptr<Impl>> retired;
    inline static std::mutex retiredMutex;
    ~Impl() {if(event) CloseHandle(event);}
    bool Safe() const {
        if(FAILED(device12->GetDeviceRemovedReason())) return true;
        const bool generatedDone=!pending ||
            (pendingValue && fence->GetCompletedValue()>=pendingValue);
        const bool sampledDone=!presentValue ||
            (presentFence && presentFence->GetCompletedValue()>=presentValue);
        return generatedDone && sampledDone;
    }
    bool Wait() {
        Check(fence->SetEventOnCompletion(pendingValue,event));
        if(WaitForSingleObject(event,2000)!=WAIT_OBJECT_0) return false;
        if(FAILED(device12->GetDeviceRemovedReason())) return false;
        pending=false;return true;
    }
    bool Submit() {
        Check(list->Close());ID3D12CommandList* work[]={list.Get()};
        pending=true;pendingValue=0;queue->ExecuteCommandLists(1,work);
        const auto next=++value;Check(queue->Signal(fence.Get(),next));pendingValue=next;return Wait();
    }
    // The allocator, output flags, motion/depth and readback all belong to a
    // bank. Do not reset or overwrite them until that bank's GPU work finishes.
    bool AwaitBank(Bank& bank) {
        if(bank.timeline.ProducerReady(fence->GetCompletedValue())) return true;
        Check(fence->SetEventOnCompletion(bank.timeline.ProducerFenceValue(),event));
        return WaitForSingleObject(event,2000)==WAIT_OBJECT_0 &&
               SUCCEEDED(device12->GetDeviceRemovedReason());
    }
    bool SubmitBank(Bank& bank) {
        Check(bank.list->Close());
        ID3D12CommandList* commands[]={bank.list.Get()};
        // A previous presentation can still be sampling this output on D3D11.
        // Wait on the GPU timeline, not by stalling the source-renderer thread.
        if(bank.timeline.PresenterFenceValue())
            Check(queue->Wait(presentFence.Get(),bank.timeline.PresenterFenceValue()));
        queue->ExecuteCommandLists(1,commands);
        const uint64_t done=++value;
        Check(queue->Signal(fence.Get(),done));
        bank.timeline.ProducerSubmitted(done);
        pending=true;pendingValue=done;
        return asyncProducer || Wait();
    }
    bool CheckBank(Bank& bank) {
        if(!bank.timeline.ProducerFenceValue() ||
           !bank.timeline.ProducerReady(fence->GetCompletedValue())) return false;
        if(bank.checked) return bank.valid;
        bank.checked=true;
        if(!bank.valid || !bank.generatedCount) return bank.valid;
        void* data=nullptr;
        const D3D12_RANGE read{0,bank.generatedCount*4u};
        if(FAILED(bank.readback->Map(0,&read,&data)) || !data) {
            bank.valid=false;
            if(bank.multiplierRetry) runtime.CompleteMultiplierRetry(bank.generatedCount+1,false);
            return false;
        }
        for(unsigned i=0;i<bank.generatedCount;++i)
            if(static_cast<const unsigned*>(data)[i]!=0) bank.valid=false;
        const D3D12_RANGE noWrite{0,0};
        bank.readback->Unmap(0,&noWrite);
        if(bank.multiplierRetry) runtime.CompleteMultiplierRetry(bank.generatedCount+1,bank.valid);
        return bank.valid;
    }
    void MarkConsumed(uint64_t submittedSerial) {
        if(!asyncProducer) return;
        for(unsigned i=0;i<activeBanks;++i) {
            auto& bank=banks[i];
            if(bank.serial!=submittedSerial) continue;
            // D3D11 has enqueued the texture read. The fence value completes
            // after that read, so D3D12 cannot overwrite it prematurely.
            Check(context4->Signal(presentFence11.Get(),++presentValue));
            bank.timeline.PresenterSubmitted(presentValue);
            break;
        }
    }
    void Init(ID3D11Device1* d,ID3D11DeviceContext1* c,const D3D11_TEXTURE2D_DESC& desc,bool nativeSharedOriginals) {
        device11=d;context=c;sourceW=desc.Width;sourceH=desc.Height;
        w=sourceW;h=sourceH;
        const UINT limit=MaxGenerationHeight();
        if(sourceH>limit) {
            h=limit;
            const double scaledWidth=double(sourceW)*double(h)/double(sourceH);
            w=std::max(2u,static_cast<UINT>(std::lround(scaledWidth)) & ~1u);
        }
        format=desc.Format;multiplier=2;
        // Bank allocation is coupled to group resource lifetimes. Unlike the
        // source snapshots, generated resources are reused after N groups.
        // Keep the working v7 default (3), allow 4 for the Live Flow path.
        wchar_t bankSetting[4]{};
        if(GetEnvironmentVariableW(L"MPCVR_MFG_BANKS",bankSetting,_countof(bankSetting)) &&
           bankSetting[1]==L'\0' && bankSetting[0]>=L'2' && bankSetting[0]<=L'4')
            activeBanks=unsigned(bankSetting[0]-L'0');
        Check(device11.As(&device5));Check(context.As(&context4));
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;Check(device11.As(&dxgi));Check(dxgi->GetAdapter(&adapter));
        Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)));
        D3D12_COMMAND_QUEUE_DESC q{};Check(device12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        Check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        Check(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        Check(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        Check(device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&inputFence)));
        Check(device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&presentFence)));
        const auto open11=[&](ID3D12Fence* source,ComPtr<ID3D11Fence>& destination) {
            HANDLE handle=nullptr;
            Check(device12->CreateSharedHandle(source,nullptr,GENERIC_ALL,nullptr,&handle));
            const HRESULT hr=device5->OpenSharedFence(handle,IID_PPV_ARGS(&destination));
            CloseHandle(handle);Check(hr);
        };
        open11(inputFence.Get(),inputFence11);
        open11(presentFence.Get(),presentFence11);
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event) throw std::runtime_error("MFG fence event failed");
        const auto ngxFormat=format==DXGI_FORMAT_R10G10B10A2_UNORM?format:DXGI_FORMAT_R8G8B8A8_UNORM;
        if(!runtime.Open(device12.Get())||!runtime.Create(list.Get(),w,h,ngxFormat)) throw std::runtime_error("NGX open/create failed");
        if(!Submit()) throw std::runtime_error("MFG GPU create timed out");
        // Create one full-resolution snapshot for each outstanding real frame;
        // only NGX-related textures are reduced.  The same snapshot is sampled
        // to downscale the NGX input, avoiding a redundant full-size copy.
        auto nativeDesc=desc;
        nativeDesc.Usage=D3D11_USAGE_DEFAULT;
        nativeDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        nativeDesc.MiscFlags=nativeDesc.CPUAccessFlags=0;
        originalDesc=nativeDesc;
        if(nativeSharedOriginals) {
            // Match the already working Native D3D12 NT-shared BGRA surface
            // contract. Hybrid keeps the original unshared snapshot descriptor.
            originalDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            originalDesc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        }
        for(unsigned bankIndex=0;bankIndex<activeBanks;++bankIndex) {
            auto& bank=banks[bankIndex];
            bank.original=NewOriginal();
            bank.real.Create(device12.Get(),d,w,h,ngxFormat,false);
            bank.depth.Create(device12.Get(),d,w,h,DXGI_FORMAT_R32_FLOAT,false);
            // NVOFA's flow is copied into a bank-owned texture before NGX
            // reads it. Neither the GPU work nor readback is shared between
            // overlapping frame groups.
            bank.motion.Create(device12.Get(),d,w,h,DXGI_FORMAT_R16G16_FLOAT,false);
            Check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&bank.allocator)));
            Check(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               bank.allocator.Get(),nullptr,IID_PPV_ARGS(&bank.list)));
            Check(bank.list->Close());
            bank.readback=Buffer(device12.Get(),MaxGenerated*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            for(unsigned i=0;i<multiplier-1;++i) {
                bank.generated[i].Create(device12.Get(),d,w,h,ngxFormat,true);
                bank.disabled[i]=Buffer(device12.Get(),4,D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
        }
        zero=Buffer(device12.Get(),4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* data=nullptr;D3D12_RANGE empty{0,0};Check(zero->Map(0,&empty,&data));memset(data,0,4);zero->Unmap(0,nullptr);
        const D3D_FEATURE_LEVEL fl[]={D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL chosen{};
        Check(d->CreateDeviceContextState(0,fl,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device1),&chosen,&state));
        ComPtr<ID3DBlob> code,error;
        auto compile=[&](const char* entry,const char* target){code.Reset();error.Reset();Check(D3DCompile(MotionShaders,sizeof(MotionShaders)-1,nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error));};
        compile("VS","vs_5_0");Check(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
        compile("Luma","ps_5_0");Check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&luma));
        compile("Copy","ps_5_0");Check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&copy));
        compile("Vectors","ps_5_0");Check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vectors));
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=32;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(d->CreateBuffer(&cb,nullptr,&constants));
        CDlssOpticalFlow::Options options;options.bidirectional=false;options.cost=false;
        if(!flow.Init(d,c,w,h,options)) throw std::runtime_error("NVIDIA Optical Flow initialization failed");
        allocated=multiplier-1;
    }
    void Reconfigure(unsigned multiple) {
        const unsigned count=multiple-1;
        const auto ngxFormat=format==DXGI_FORMAT_R10G10B10A2_UNORM?format:DXGI_FORMAT_R8G8B8A8_UNORM;
        for(unsigned i=allocated;i<count;++i) {
            for(unsigned bankIndex=0;bankIndex<activeBanks;++bankIndex) {
                banks[bankIndex].generated[i].Create(device12.Get(),device11.Get(),w,h,ngxFormat,true);
                banks[bankIndex].disabled[i]=Buffer(device12.Get(),4,D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
        }
        // MultiFrameCount can be supplied at Evaluate time. Keep OFA history
        // across per-interval x2/x3 changes; only grow output resource capacity.
        // Manual mode changes/discontinuities explicitly ResetHistory upstream.
        allocated=std::max(allocated,count);multiplier=multiple;
    }
    bool CaptureOnly(ID3D11Texture2D* image,int64_t timestamp,Batch& batch,const DepthInput* depth) {
        // No NGX work in Auto x1. The output bank serial still advances so
        // queued generated frames can expire, but real snapshots stay valid.
        auto& bank=banks[unsigned(serial%activeBanks)];
        Capture(bank,image);
        if (depth && depth->captureSource) depth->source.Update(device11.Get(), context.Get(), bank.original->view.Get(),
            depth->picture, depth->modelSize, timestamp);
        bank.serial=++serial;
        batch={serial,0,previous,timestamp,bank.original};
        previous=-1;
        zipHistory.Reset();
        zipDepthActive = false;
        flow.Reset();
        return true;
    }
    bool Generate(ID3D11Texture2D* image,int64_t timestamp,Batch& batch,bool inspectMotion,const DepthInput* depth) {
        const bool reset=previous<0||timestamp<=previous||timestamp-previous>2500000;
        zipDepthActive = false;
        if (reset || !depth) zipHistory.Reset();
        if (reset) {
            flow.Reset();
            motionCheck.Reset();
        }
        const unsigned bankIndex=unsigned(serial%activeBanks);auto& bank=banks[bankIndex];
        if(asyncProducer && !bank.timeline.ProducerReady(fence->GetCompletedValue())) {
            // NGX is more than N groups behind. Preserve this real frame
            // immediately rather than stalling the player's source clock.
            ++busyFallbacks;
            return CaptureOnly(image,timestamp,batch,depth);
        }
        if(!AwaitBank(bank)) throw std::runtime_error("MFG: GPU bank reuse timed out");
        if(bank.multiplierRetry) CheckBank(bank);
        bank.multiplierRetry=false;
        bank.checked=false;bank.valid=false;bank.generatedCount=0;
        ComPtr<ID3DDeviceContextState> saved;context->SwapDeviceContextState(state.Get(),&saved);
        struct Restore {ID3D11DeviceContext1* c;ID3DDeviceContextState* old;~Restore(){c->SwapDeviceContextState(old,nullptr);}} restore{context.Get(),saved.Get()};
        // Keep the original at its full resolution while NGX operates on its
        // downscaled copy. This snapshot is retained by the frame queue.
        Capture(bank,image);
        if (depth && depth->captureSource) depth->source.Update(device11.Get(), context.Get(), bank.original->view.Get(),
            depth->picture, depth->modelSize, timestamp);
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->RSSetState(nullptr);
        context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&viewport);
        // SV_Position-based copy must map the *entire* native source into the
        // lower-resolution NGX target, not crop its top-left corner.
        const UINT copyParams[8]={w,h,sourceW,sourceH,flow.GridSize(),0,0,0};
        context->UpdateSubresource(constants.Get(),0,nullptr,copyParams,0,0);
        ID3D11Buffer* cb=constants.Get();context->PSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* srvs[2]={bank.original->view.Get(),nullptr};context->PSSetShaderResources(0,2,srvs);
        ID3D11RenderTargetView* target=bank.real.rtv.Get();context->OMSetRenderTargets(1,&target,nullptr);
        context->PSSetShader(copy.Get(),nullptr,0);context->Draw(3,0);
        context->OMSetRenderTargets(0,nullptr,nullptr);srvs[0]=bank.real.srv.Get();context->PSSetShaderResources(0,2,srvs);
        target=flow.FrameTarget();context->OMSetRenderTargets(1,&target,nullptr);
        context->PSSetShader(luma.Get(),nullptr,0);context->Draw(3,0);
        target=nullptr;context->OMSetRenderTargets(0,nullptr,nullptr);srvs[0]=nullptr;context->PSSetShaderResources(0,2,srvs);
        const bool hasFlow=flow.Execute();
        if(flow.LastExecuteFailed()) throw std::runtime_error("NVIDIA Optical Flow execution failed");
        if (inspectMotion && hasFlow) {
            motionCheck.Submit(device11.Get(), context.Get(), flow.ForwardFlow(),
                flow.OutputWidth(), flow.OutputHeight(), timestamp);
        }
        const FLOAT flat[4]={.5f,0,0,0},none[4]={0,0,0,0};
        if(hasFlow) {
            // Same constants as the copy pass; the vector shader reads grid.
            srvs[1]=flow.ForwardFlow();context->PSSetShaderResources(0,2,srvs);
            target=bank.motion.rtv.Get();context->OMSetRenderTargets(1,&target,nullptr);
            context->PSSetShader(vectors.Get(),nullptr,0);context->Draw(3,0);
        } else context->ClearRenderTargetView(bank.motion.rtv.Get(),none);
        context->OMSetRenderTargets(0,nullptr,nullptr);srvs[1]=nullptr;context->PSSetShaderResources(0,2,srvs);
        if (!hasFlow) zipHistory.Reset();
        if (depth && hasFlow && !reset) {
            const auto scaleX = [this](LONG x) { return LONG((int64_t(x) * w + sourceW / 2) / sourceW); };
            const auto scaleY = [this](LONG y) { return LONG((int64_t(y) * h + sourceH / 2) / sourceH); };
            const RECT picture{scaleX(depth->picture.left), scaleY(depth->picture.top),
                scaleX(depth->picture.right), scaleY(depth->picture.bottom)};
            zipDepthActive = zipHistory.Draw(device11.Get(), context.Get(), depth->source,
                bank.depth.d11.Get(), bank.depth.rtv.Get(), bank.motion.srv.Get(), picture,
                depth->modelSize, timestamp, float(timestamp - previous) / 10000.f, true);
        }
        if (!zipDepthActive) context->ClearRenderTargetView(bank.depth.rtv.Get(), flat);

        const auto inputReady=++inputValue;
        Check(context4->Signal(inputFence11.Get(),inputReady));context->Flush();
        Check(queue->Wait(inputFence.Get(),inputReady));
        Check(bank.allocator->Reset());Check(bank.list->Reset(bank.allocator.Get(),nullptr));
        ID3D12GraphicsCommandList* work=bank.list.Get();
        std::array<ID3D12Resource*,MaxGenerated> out{},flag{};
        for(auto r:{bank.real.d12.Get(),bank.depth.d12.Get(),bank.motion.d12.Get()})
            Transition(work,r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=0;i<multiplier-1;++i) {
            out[i]=bank.generated[i].d12.Get();flag[i]=bank.disabled[i].Get();
            Transition(work,out[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(work,flag[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
            work->CopyBufferRegion(flag[i],0,zero.Get(),0,4);
            Transition(work,flag[i],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const auto evaluation=runtime.Evaluate(work,bank.real.d12.Get(),bank.depth.d12.Get(),
                                                bank.motion.d12.Get(),out,flag,multiplier,reset||!hasFlow);
        for(unsigned i=0;i<multiplier-1;++i) {
            Transition(work,out[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
            Transition(work,flag[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            work->CopyBufferRegion(bank.readback.Get(),i*4,flag[i],0,4);
            Transition(work,flag[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        for(auto r:{bank.real.d12.Get(),bank.depth.d12.Get(),bank.motion.d12.Get()})
            Transition(work,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(evaluation==Runtime::Evaluation::Failed) throw std::runtime_error("NGX evaluation failed");
        // A refused multiplier may have recorded earlier phases. Submit the
        // bank normally to retire that work, but queue only its source frame.
        const bool accepted=evaluation==Runtime::Evaluation::Accepted;
        bank.generatedCount=accepted ? multiplier-1 : 0;
        bank.valid=accepted&&hasFlow&&!reset;
        bank.multiplierRetry=bank.valid&&multiplier>1u+runtime.Maximum();
        if(!SubmitBank(bank)) throw std::runtime_error("MFG GPU evaluate timed out");
        if(bank.valid && zipDepthActive) depth->source.RecordMfgSubmission();
        // The producer no longer waits for D3D12 completion when asynchronous.
        // Only generated surfaces are gated by the completion fence. Originals
        // remain separate snapshots and may be presented independently.
        bank.serial=++serial;
        batch={serial,bank.valid ? multiplier-1 : 0,previous,timestamp,bank.original};
        previous=accepted ? timestamp : -1;
        if(!accepted) zipDepthActive=false;
        return true;
    }
};
Session::Session()=default;Session::~Session(){Reset();}
bool Session::Warmup(ID3D11Device1* device, ID3D11DeviceContext1* context,
    ID3D11Texture2D* image, unsigned multiplier,
    bool nativeSharedOriginals) {
    if(failed_ || !device || !context || !image || multiplier<2 || multiplier>MaxMultiplier) return false;
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
    // Do not disturb an already initialized/running session during Pause.
    if(impl_ && impl_->sourceW==desc.Width && impl_->sourceH==desc.Height &&
       impl_->format==desc.Format) return true;
    if(impl_) Reset();
    std::unique_ptr<Impl> next;
    try {
        // A retired session may still own GPU resources. Never stall graph
        // preroll waiting for it: the normal video path remains available.
        {
            std::lock_guard guard(Impl::retiredMutex);
            std::erase_if(Impl::retired,[](const auto& old){return old->Safe();});
            if(!Impl::retired.empty()) {
                status_=L"MFG: previous GPU session still retiring";
                return false;
            }
        }
        next=std::make_unique<Impl>();
        next->Init(device,context,desc,nativeSharedOriginals);
        impl_=std::move(next);
        status_=L"MFG: resources prewarmed before playback";
        return true;
    } catch(const std::exception& e) {
        const std::string what=e.what();status_=std::wstring(what.begin(),what.end());
        if(next&&!next->runtime.Status().empty()) status_+=L"; "+next->runtime.Status();
        Reset();failed_=true;
        return false;
    }
}
void Session::ResetHistory() {
    if(failed_){Reset();return;}
    if (impl_) {
        impl_->previous = -1;
        impl_->flow.Reset();
        impl_->motionCheck.Reset();
        impl_->zipHistory.Reset();
        impl_->zipDepthActive = false;
    }
}
void Session::Reset() {
    if(impl_) {
        if(!impl_->Safe()) {
            std::lock_guard guard(Impl::retiredMutex);
            Impl::retired.emplace_back(std::move(impl_));
        } else impl_.reset();
    }
    failed_=false;
}
bool Session::Generate(ID3D11Device1* device,ID3D11DeviceContext1* context,ID3D11Texture2D* image,
    unsigned multiplier,int64_t timestamp,Batch& batch,
    bool nativeSharedOriginals, bool inspectMotion, const DepthInput* depth) {
    if(failed_||!image||multiplier<2||multiplier>MaxMultiplier) return false;
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
    if(impl_&&(impl_->sourceW!=desc.Width||impl_->sourceH!=desc.Height||impl_->format!=desc.Format)) Reset();
    try {
        if(!impl_) {
            {
                std::lock_guard guard(Impl::retiredMutex);
                std::erase_if(Impl::retired,[](const auto& old){return old->Safe();});
                if(!Impl::retired.empty()) {
                    // Seek/resize may retire GPU work which is still in flight.
                    // Let MPC-VR present source frames and retry MFG later;
                    // this is not a permanent runtime failure.
                    status_=L"MFG: finishing the previous GPU/presenter session";
                    return false;
                }
            }
            impl_=std::make_unique<Impl>();impl_->Init(device,context,desc,nativeSharedOriginals);
        }
        const bool continuous = impl_->previous >= 0 && timestamp > impl_->previous &&
            timestamp - impl_->previous <= 2'500'000;
        multiplier = impl_->runtime.SelectMultiplier(multiplier, continuous);
        if(impl_->multiplier!=multiplier)impl_->Reconfigure(multiplier);
        const bool ok=impl_->Generate(image,timestamp,batch,inspectMotion,depth);
        status_=L"MFG x"+std::to_wstring(multiplier)+L"; internal "+
            std::to_wstring(impl_->w)+L"x"+std::to_wstring(impl_->h)+
            L" from "+std::to_wstring(impl_->sourceW)+L"x"+std::to_wstring(impl_->sourceH)+
            L"; generated "+std::to_wstring(batch.generated)+L"; "+impl_->runtime.Status();return ok;
    }catch(const std::exception& e) {
        const std::string what=e.what();status_=std::wstring(what.begin(),what.end());
        if(impl_&&!impl_->runtime.Status().empty()) status_+=L"; "+impl_->runtime.Status();
        Reset();failed_=true;return false;
    }
}
FlowMotion Session::TakeFlowMotion(int64_t pts) {
    return impl_ ? impl_->motionCheck.Take(impl_->context.Get(), pts) : FlowMotion::Unknown;
}

bool Session::CaptureOriginal(ID3D11Texture2D* image,int64_t timestamp,Batch& batch,const DepthInput* depth) {
    if(failed_ || !impl_ || !image) return false;
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
    if(impl_->sourceW!=desc.Width || impl_->sourceH!=desc.Height || impl_->format!=desc.Format)
        return false;
    try {return impl_->CaptureOnly(image,timestamp,batch,depth);}
    catch(const std::exception& e) {
        const std::string what=e.what();status_=std::wstring(what.begin(),what.end());
        failed_=true;return false;
    }
}
bool Session::UsesZipDepth() const {
    return impl_ && impl_->zipDepthActive;
}
SIZE Session::InputSize() const {
    return impl_ ? SIZE{LONG(impl_->sourceW), LONG(impl_->sourceH)} : SIZE{};
}
SIZE Session::GeneratedSize() const {
    return impl_ ? SIZE{LONG(impl_->w), LONG(impl_->h)} : SIZE{};
}
std::wstring Session::WorkDescription() const {
    if(!impl_) return {};
    return std::to_wstring(impl_->w)+L"x"+std::to_wstring(impl_->h)+
        L" (source "+std::to_wstring(impl_->sourceW)+L"x"+std::to_wstring(impl_->sourceH)+L"); banks "+
        std::to_wstring(impl_->activeBanks);
}
bool Session::Ready(uint64_t serial,unsigned index) const {
    if(!impl_ || index>=MaxGenerated) return false;
    for(unsigned i=0;i<impl_->activeBanks;++i) {
        auto& bank=impl_->banks[i];
        if(bank.serial!=serial) continue;
        if(index>=bank.generatedCount || index>=impl_->allocated) return false;
        return impl_->CheckBank(bank);
    }
    return false;
}
FrameSurface Session::Frame(uint64_t serial,unsigned index) const {
    if(!impl_)return {};
    for(unsigned i=0;i<impl_->activeBanks;++i) {
        const auto& bank=impl_->banks[i];
        if(bank.serial!=serial)continue;
        // Original frames are obtained from Batch::original instead; never
        // resolve them through a recyclable bank. This branch is defensive.
        if(index==MaxGenerated)return {};
        // An older bank may contain more generated frames than the current
        // auto-selected multiplier. Address by allocated capacity and serial.
        if(index<impl_->allocated && index<bank.generatedCount) {
            if(!bank.timeline.ProducerReady(impl_->fence->GetCompletedValue()))
                return {nullptr,nullptr,true};
            if(!impl_->CheckBank(const_cast<Impl::Bank&>(bank))) return {};
            return {bank.generated[index].d11.Get(),bank.generated[index].srv.Get(),false};
        }
    }
    return {};
}
void Session::MarkConsumed(uint64_t serial) {
    if(impl_ && !failed_) {
        try {impl_->MarkConsumed(serial);}
        catch(...) {failed_=true;}
    }
}
bool Session::AsyncProducer() const {return impl_ ? impl_->asyncProducer : AsyncProducerEnabled();}
uint64_t Session::BusyFallbacks() const {return impl_ ? impl_->busyFallbacks : 0;}
unsigned Session::BankCount() const {return impl_?impl_->activeBanks:2u;}
unsigned Session::MaxAvailableMultiplier() const {
    return impl_ ? std::min(MaxMultiplier, 1u + impl_->runtime.Maximum()) : MaxMultiplier;
}
}
