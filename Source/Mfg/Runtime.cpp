// SPDX-License-Identifier: GPL-3.0-or-later
// SDK call contract follows NVIDIA's DLSS-G helpers. No NR/SR backend.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Runtime.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params_dlssg.h>
using MfgBridgeFn = NVSDK_NGX_Result(__cdecl*)(void*,void*,const void*,const void*,void*);
static thread_local MfgBridgeFn mfgBridge=nullptr;
static thread_local void* mfgTarget=nullptr;
static thread_local bool mfgEvaluateException=false;
static NVSDK_NGX_Result MfgEvaluateBridge(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params,PFN_NVSDK_NGX_ProgressCallback) {
    __try { return mfgBridge(mfgTarget,list,handle,params,nullptr); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        mfgEvaluateException=true;
        return NVSDK_NGX_Result_Fail;
    }
}
#define NVSDK_NGX_D3D12_EvaluateFeature_C MfgEvaluateBridge
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#undef NVSDK_NGX_D3D12_EvaluateFeature_C
#include <bcrypt.h>
#include <shlobj.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <atomic>
#include "../DLSS/NgxRuntime.h"
#include "AdaCompatibility.h"
#include "DlssCaptureCamera.h"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace mfg {
namespace {
std::atomic<bool> runtimeOwned{false};
constexpr ULONGLONG MultiplierRetryDelayMs = 2000;
using Result = NVSDK_NGX_Result;
using Init = Result(__cdecl*)(unsigned long long,const wchar_t*,ID3D12Device*,unsigned,const void*);
using Populate = Result(__cdecl*)(NVSDK_NGX_Parameter*);
using CreateFn = Result(__cdecl*)(ID3D12GraphicsCommandList*,unsigned,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using EvalFn = Result(__cdecl*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,void*);
using ReleaseFn = Result(__cdecl*)(NVSDK_NGX_Handle*);
using ShutdownFn = Result(__cdecl*)(ID3D12Device*);
using Alloc = Result(__cdecl*)(NVSDK_NGX_Parameter**);
using Destroy = Result(__cdecl*)(NVSDK_NGX_Parameter*);
using ShimInit = Result(__cdecl*)(void*,unsigned long long,const wchar_t*,void*,unsigned,const void*);
using ShimPopulate = Result(__cdecl*)(void*,void*);
using ShimCreate = Result(__cdecl*)(void*,void*,unsigned,void*,void**);
using ShimEval = Result(__cdecl*)(void*,void*,const void*,const void*,void*);
using ShimRelease = Result(__cdecl*)(void*,void*);
Result SafeRelease(ShimRelease shim,void* target,void* object) {
    __try {return shim(target,object);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return NVSDK_NGX_Result_Fail;}
}
// The SEH boundary contains no C++ objects with destructors.
Result SafeInit(ShimInit f,Init target,const wchar_t* cache,ID3D12Device* dev,NVSDK_NGX_Parameter* p) {
    __try { return f(reinterpret_cast<void*>(target),0x1000000ull,cache,dev,NVSDK_NGX_Version_API,p); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
Result SafeCreate(ShimCreate f,CreateFn target,ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h) {
    __try { return f(reinterpret_cast<void*>(target),list,NVSDK_NGX_Feature_FrameGeneration,p,reinterpret_cast<void**>(h)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
Result SafeEval(ShimEval f,EvalFn target,ID3D12GraphicsCommandList* list,NVSDK_NGX_Handle* h,NVSDK_NGX_Parameter* p) {
    __try { return f(reinterpret_cast<void*>(target),list,h,p,nullptr); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
std::wstring Error(const wchar_t* step,Result r) {
    wchar_t s[128]{}; swprintf_s(s,L"%s: NGX 0x%08X; normal playback retained",step,unsigned(r)); return s;
}
bool Hash(HANDLE file,const char* expected) {
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    bool ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok) ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
    BYTE block[65536],digest[32]{}; DWORD n=0;
    while(ok) {
        if(!ReadFile(file,block,sizeof(block),&n,nullptr)) {ok=false;break;}
        if(!n) break;
        ok=BCryptHashData(hash,block,n,0)>=0;
    }
    if(ok) ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash) BCryptDestroyHash(hash); if(alg) BCryptCloseAlgorithmProvider(alg,0);
    char hex[65]{}; for(unsigned i=0;i<32;++i) sprintf_s(hex+i*2,3,"%02X",digest[i]);
    return ok && _stricmp(hex,expected)==0;
}
unsigned ComputeCapability(ID3D12Device* device,HMODULE cuda) {
    if(!cuda) return 0;
    auto init=reinterpret_cast<int(__stdcall*)(unsigned)>(GetProcAddress(cuda,"cuInit"));
    auto count=reinterpret_cast<int(__stdcall*)(int*)>(GetProcAddress(cuda,"cuDeviceGetCount"));
    auto get=reinterpret_cast<int(__stdcall*)(int*,int)>(GetProcAddress(cuda,"cuDeviceGet"));
    auto luid=reinterpret_cast<int(__stdcall*)(char*,unsigned*,int)>(GetProcAddress(cuda,"cuDeviceGetLuid"));
    auto attr=reinterpret_cast<int(__stdcall*)(int*,int,int)>(GetProcAddress(cuda,"cuDeviceGetAttribute"));
    int n=0; const LUID selected=device->GetAdapterLuid();
    if(!init||!count||!get||!luid||!attr||init(0)||count(&n)) return 0;
    for(int i=0;i<n;++i) {
        int dev=0,major=0,minor=0; unsigned mask=0; LUID id{};
        if(!get(&dev,i)&&!luid(reinterpret_cast<char*>(&id),&mask,dev)&&!memcmp(&id,&selected,sizeof(id))&&
            !attr(&major,75,dev)&&!attr(&minor,76,dev)) return unsigned(major*10+minor);
    }
    return 0;
}
}
struct Runtime::Impl {
    bool exclusive=!runtimeOwned.exchange(true);
    HMODULE module=nullptr,core=nullptr,shim=nullptr,cuda=nullptr;
    HANDLE file=INVALID_HANDLE_VALUE;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    NVSDK_NGX_Parameter* params=nullptr;
    NVSDK_NGX_Handle* feature=nullptr;
    CreateFn create=nullptr; EvalFn evaluate=nullptr; ReleaseFn release=nullptr; ShutdownFn shutdown=nullptr;
    Destroy destroy=nullptr;
    ShimInit si=nullptr; ShimPopulate sp=nullptr; ShimCreate sc=nullptr; ShimEval se=nullptr; ShimRelease sr=nullptr,ss=nullptr;
    unsigned maximum=1,width=0,height=0; bool initialized=false,hdr=false;
    unsigned populateMaximum=0, createMaximum=0;
    bool ada=false, adaAdapted=false, adaImageReused=false;
    unsigned refusedCount=0;
    bool retrySubmitted=false;
    ULONGLONG retryAt=0;
    std::wstring rejectedMultiplier;
    unsigned long long frame=0;
    unsigned Available() const {
        return refusedCount ? refusedCount-1 : maximum;
    }
    std::wstring CapabilityStatus() const {
        const auto describe = [](unsigned generated) -> std::wstring {
            return generated ? L"x" + std::to_wstring(generated + 1) : L"unreported";
        };
        return L"310.9.1; cap " + describe(Available()) +
            (adaImageReused ? L" (Ada adapted, reused DLL image)" :
                adaAdapted ? L" (Ada adapted)" : ada ? L" (Ada adaptation unavailable)" : L"") +
            L"; Populate " + describe(populateMaximum) + L"; Create " + describe(createMaximum) +
            rejectedMultiplier +
            (refusedCount ? (retryAt ? L"; retry pending" :
                retrySubmitted ? L"; retry submitted" : L"; lower cap for this session") : L"");
    }
    ~Impl() {
        if(feature&&release&&sr) SafeRelease(sr,reinterpret_cast<void*>(release),feature);
        if(initialized&&shutdown&&ss) SafeRelease(ss,reinterpret_cast<void*>(shutdown),device.Get());
        if(params&&destroy) destroy(params);
        if(module) FreeLibrary(module); if(core) FreeLibrary(core); if(shim) FreeLibrary(shim);
        if(cuda) FreeLibrary(cuda); if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
        if(exclusive) runtimeOwned=false;
    }
};
Runtime::Runtime()=default;
Runtime::~Runtime()=default;
void Runtime::Close() { impl_.reset(); }
unsigned Runtime::Maximum() const { return impl_?impl_->Available():0; }
unsigned Runtime::SelectMultiplier(unsigned requested, bool allowRetry) const {
    if(!impl_) return 2;
    const auto& p=*impl_;
    const unsigned available=1u+p.Available();
    if(allowRetry && requested>available && p.retryAt && GetTickCount64()>=p.retryAt)
        return available+1;
    return std::min(requested,available);
}
bool Runtime::Open(ID3D12Device* device) {
    Close(); status_=L"MFG unavailable";
    if(!device) return false;
    auto p=std::make_unique<Impl>(); p->device=device;
    if(!p->exclusive) {status_=L"MFG: another renderer owns the private runtime; normal playback retained";return false;}
    const auto directory=NgxRuntime::EditionDir();
    const auto dll=directory+L"\\nvngx_dlssg.dll";
    p->file=CreateFileW(dll.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(p->file==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        status_=L"MFG: cannot open edition\\nvngx_dlssg.dll (Win32 "+std::to_wstring(error)+L")";
        return false;
    }
    // Exact NVIDIA-signed Manager native runtime, independently inventoried.
    if(!Hash(p->file,"FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82")) {
        status_=L"MFG: edition\\nvngx_dlssg.dll hash mismatch"; return false;
    }
    p->module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    p->shim=LoadLibraryExW((directory+L"\\mpcvrmfg_nvngx.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    p->core=NgxRuntime::Load();
    if(!p->module||!p->shim||!p->core) {status_=L"MFG: runtime, private bridge or driver NGX core could not load";return false;}
    const auto bridgeAbi=reinterpret_cast<unsigned(__cdecl*)()>(GetProcAddress(p->shim,"mfg_bridge_abi"));
    if(!bridgeAbi||bridgeAbi()!=0x00010000){status_=L"MFG: private bridge ABI mismatch; worker DLLs cannot be substituted";return false;}
    auto init=reinterpret_cast<Init>(GetProcAddress(p->module,"NVSDK_NGX_D3D12_Init_Ext"));
    auto populate=reinterpret_cast<Populate>(GetProcAddress(p->module,"NVSDK_NGX_D3D12_PopulateParameters_Impl"));
    auto allocate=reinterpret_cast<Alloc>(GetProcAddress(p->core,"NVSDK_NGX_D3D12_AllocateParameters"));
    p->destroy=reinterpret_cast<Destroy>(GetProcAddress(p->core,"NVSDK_NGX_D3D12_DestroyParameters"));
    p->create=reinterpret_cast<CreateFn>(GetProcAddress(p->module,"NVSDK_NGX_D3D12_CreateFeature"));
    p->evaluate=reinterpret_cast<EvalFn>(GetProcAddress(p->module,"NVSDK_NGX_D3D12_EvaluateFeature"));
    p->release=reinterpret_cast<ReleaseFn>(GetProcAddress(p->module,"NVSDK_NGX_D3D12_ReleaseFeature"));
    p->shutdown=reinterpret_cast<ShutdownFn>(GetProcAddress(p->module,"NVSDK_NGX_D3D12_Shutdown1"));
    p->si=reinterpret_cast<ShimInit>(GetProcAddress(p->shim,"shim_init"));
    p->sp=reinterpret_cast<ShimPopulate>(GetProcAddress(p->shim,"shim_populate"));
    p->sc=reinterpret_cast<ShimCreate>(GetProcAddress(p->shim,"shim_create"));
    p->se=reinterpret_cast<ShimEval>(GetProcAddress(p->shim,"shim_eval"));
    p->sr=reinterpret_cast<ShimRelease>(GetProcAddress(p->shim,"shim_release"));
    p->ss=reinterpret_cast<ShimRelease>(GetProcAddress(p->shim,"shim_shutdown1"));
    if(!init||!populate||!allocate||!p->destroy||!p->create||!p->evaluate||!p->release||!p->shutdown||
        !p->si||!p->sp||!p->sc||!p->se||!p->sr||!p->ss) {status_=L"MFG: incomplete NGX ABI";return false;}
    p->cuda=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const unsigned sm=ComputeCapability(device,p->cuda);
    if(sm==89) {
        p->ada=true;
        auto base=reinterpret_cast<BYTE*>(p->module);
        auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        const auto mapping=CreateFileMappingW(p->file,nullptr,PAGE_READONLY|SEC_IMAGE_NO_EXECUTE,0,0,nullptr);
        if(mapping) {
            const std::unique_ptr<void,decltype(&UnmapViewOfFile)> original(
                MapViewOfFile(mapping,FILE_MAP_READ,0,0,0),&UnmapViewOfFile);
            CloseHandle(mapping);
            if(original) {
                const auto adapted=lf_ada::Apply(base,static_cast<BYTE*>(original.get()),nt->OptionalHeader.SizeOfImage);
                p->adaAdapted=adapted.applied;
                p->adaImageReused=adapted.reused;
            }
        }
        // This is still only a potential ceiling; Create/Evaluate must succeed.
        p->maximum=p->adaAdapted?MaxGenerated:1;
    } else if(sm>=120) p->maximum=MaxGenerated;
    else {status_=L"MFG: selected adapter capability is not recognized; no compatibility patch applied";return false;}
    auto r=allocate(&p->params);
    if(NVSDK_NGX_FAILED(r)||!p->params) {status_=Error(L"AllocateParameters",r);return false;}
    wchar_t localAppData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        status_=L"MFG: local application data unavailable"; return false;
    }
    const auto cache=std::filesystem::path(localAppData) / L"MPC-VR-Edition" / L"mfg";
    std::error_code ec;
    std::filesystem::create_directories(cache,ec);
    if(ec) {status_=L"MFG: private runtime cache is not writable";return false;}
    r=SafeInit(p->si,init,cache.c_str(),device,p->params);
    if(NVSDK_NGX_FAILED(r)) {status_=Error(L"Init_Ext",r);return false;}
    p->initialized=true;
    p->sp(reinterpret_cast<void*>(populate),p->params);
    unsigned advertised=0;
    if(p->params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,&advertised)==NVSDK_NGX_Result_Success)
        p->populateMaximum=std::min(advertised,MaxGenerated);
    // The private Live Flow Ada path establishes its own ceiling after
    // adapting this exact runtime. The public capability value can still
    // describe stock Ada. Check actual Evaluate results on that path.
    if(!p->adaAdapted && p->populateMaximum) p->maximum=std::min(p->maximum,p->populateMaximum);
    status_=p->CapabilityStatus()+L"; initialized";
    impl_=std::move(p); return true;
}
bool Runtime::Create(ID3D12GraphicsCommandList* list,unsigned width,unsigned height,DXGI_FORMAT format) {
    if(!impl_||!list||!width||!height) return false;
    auto& p=*impl_; if(p.feature) return false;
    auto s=p.params; s->Reset();
    s->Set("CreationNodeMask",1u); s->Set("VisibilityNodeMask",1u);
    s->Set("Width",width); s->Set("Height",height);
    s->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,unsigned(format));
    s->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth,width);
    s->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight,height);
    s->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution,0u);
    const auto r=SafeCreate(p.sc,p.create,list,s,&p.feature);
    if(NVSDK_NGX_FAILED(r)||!p.feature) {status_=Error(L"CreateFeature",r);return false;}
    p.width=width;p.height=height;p.hdr=format==DXGI_FORMAT_R10G10B10A2_UNORM;
    unsigned maximum=0;
    if(s->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,&maximum)==NVSDK_NGX_Result_Success)
        p.createMaximum=std::min(maximum,MaxGenerated);
    if(!p.adaAdapted && p.createMaximum) p.maximum=std::min(p.maximum,p.createMaximum);
    status_=p.CapabilityStatus()+L"; feature created";return true;
}
Runtime::Evaluation Runtime::Evaluate(ID3D12GraphicsCommandList* list,ID3D12Resource* color,ID3D12Resource* depth,
    ID3D12Resource* motion,const std::array<ID3D12Resource*,MaxGenerated>& outputs,
    const std::array<ID3D12Resource*,MaxGenerated>& disabled,unsigned multiplier,bool reset) {
    if(!impl_||!list||!color||!depth||!motion||multiplier<2||multiplier>MaxMultiplier) return Evaluation::Failed;
    auto& p=*impl_;const unsigned count=multiplier-1;
    if(!p.feature || multiplier> SelectMultiplier(multiplier, true)) {
        status_=L"MFG: requested multiplier exceeds initialized runtime capacity";
        return Evaluation::Failed;
    }
    auto s=p.params;
    s->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,++p.frame);
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    options.multiFrameCount=count; options.reset=reset; options.colorBuffersHDR=p.hdr;
    options.mvecScale[0]=options.mvecScale[1]=1.f;
    options.cameraUp[1]=options.cameraRight[0]=options.cameraFwd[2]=1.f;
    options.cameraNear=framegen::DlssCaptureCamera::Near; options.cameraFar=framegen::DlssCaptureCamera::Far;
    options.cameraFOV=1.04719755f; options.cameraAspectRatio=float(p.width)/p.height;
    options.depthInverted=options.cameraMotionIncluded=options.orthoProjection=true;
    options.motionVectorsInvalidValue=(std::numeric_limits<float>::max)();
    options.mvecsSubrectSize=options.depthSubrectSize={p.width,p.height};
    options.backbufferSubrectSize=options.outputInterpSubrectSize={p.width,p.height};
    const framegen::DlssCaptureCamera camera(options.cameraAspectRatio);
    memcpy(options.cameraViewToClip,camera.viewToClip.data(),sizeof(options.cameraViewToClip));
    memcpy(options.clipToCameraView,camera.clipToView.data(),sizeof(options.clipToCameraView));
    for(unsigned i=0;i<4;++i) options.clipToLensClip[i][i]=options.clipToPrevClip[i][i]=options.prevClipToClip[i][i]=1;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params evaluate{};
    evaluate.pBackbuffer=color;evaluate.pDepth=depth;evaluate.pMVecs=motion;
    mfgBridge=p.se;mfgTarget=reinterpret_cast<void*>(p.evaluate);
    mfgEvaluateException=false;
    for(unsigned i=0;i<count;++i) {
        if(!outputs[i]||!disabled[i]) {status_=L"MFG: incomplete output resources";return Evaluation::Failed;}
        options.multiFrameIndex=i+1;
        evaluate.pOutputInterpFrame=outputs[i];evaluate.pOutputDisableInterpolation=disabled[i];
        const auto r=NGX_D3D12_EVALUATE_DLSSG(list,p.feature,s,&evaluate,&options);
        if(NVSDK_NGX_FAILED(r)) {
            if(p.adaAdapted && count>1 && !mfgEvaluateException &&
               SUCCEEDED(p.device->GetDeviceRemovedReason())) {
                // One rejected batch may be transient. Retry this level once
                // after lower-multiplier playback has had time to recover.
                p.retryAt=p.refusedCount ? 0 : GetTickCount64()+MultiplierRetryDelayMs;
                p.refusedCount=count;
                p.retrySubmitted=false;
                p.rejectedMultiplier=L"; x"+std::to_wstring(multiplier)+L" refused ("+Error(L"EvaluateFeature",r)+L")";
                status_=p.CapabilityStatus();
                return Evaluation::LowerMultiplier;
            }
            status_=Error(L"EvaluateFeature",r);
            return Evaluation::Failed;
        }
    }
    // Keep the lower cap until the retry's GPU output flags are checked.
    if(p.refusedCount && count==p.refusedCount && !reset) {
        p.retryAt=0;
        p.retrySubmitted=true;
        status_=p.CapabilityStatus();
    }
    return Evaluation::Accepted;
}
void Runtime::CompleteMultiplierRetry(unsigned multiplier, bool valid) {
    if(impl_ && impl_->retrySubmitted && multiplier==impl_->refusedCount+1) {
        auto& p=*impl_;
        p.retrySubmitted=false;
        if(valid) p.refusedCount=0;
        p.rejectedMultiplier+=L"; x"+std::to_wstring(multiplier)+
            (valid ? L" retry verified" : L" retry output unavailable");
        status_=p.CapabilityStatus();
    }
}
}
