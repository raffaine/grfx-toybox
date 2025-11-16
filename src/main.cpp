#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <string>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <cassert>
using Microsoft::WRL::ComPtr;

static const UINT kWidth = 1280;
static const UINT kHeight = 720;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

struct VertexIn { float pos[3]; float nrm[3]; unsigned int boneIdx[4]; float boneWgt[4]; };
struct MeshletData { UINT vCount, pCount; UINT vOffset, pOffset; UINT boneBase; };
struct GlobalsCB { float mvp[16]; float time; float pad[3]; };

static void MakeIdentity(float m[16]) { memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1.0f; }
static void MakeOrtho(float m[16], float w, float h) { MakeIdentity(m); m[0]=2.0f/w; m[5]=-2.0f/h; m[10]=1.0f; m[12]=-1.0f; m[13]=1.0f; }

ComPtr<IDxcBlob> CompileHLSL(ComPtr<IDxcCompiler3> compiler, ComPtr<IDxcUtils> utils,
    const std::wstring& file, const wchar_t* entry, const wchar_t* target)
{
    ComPtr<IDxcIncludeHandler> includeHandler; utils->CreateDefaultIncludeHandler(&includeHandler);
    DxcBuffer buffer{};
    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(file.c_str(), nullptr, &source))) throw std::runtime_error("Failed to load shader file");
    buffer.Ptr = source->GetBufferPointer(); buffer.Size = source->GetBufferSize(); buffer.Encoding = DXC_CP_ACP;
    const wchar_t* args[] = { L"-E", entry, L"-T", target, L"-Zi", L"-Qembed_debug", L"-O3", L"-enable-16bit-types" };
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result))))
        throw std::runtime_error("DXC compile failed");
    ComPtr<IDxcBlobUtf8> errors; result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) OutputDebugStringA(errors->GetStringPointer());
    HRESULT status; result->GetStatus(&status); if (FAILED(status)) throw std::runtime_error("Shader compile error");
    ComPtr<IDxcBlob> blob; result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr); return blob;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // Window
    WNDCLASSEX wc { .cbSize = sizeof(WNDCLASSEX), .lpfnWndProc = WndProc, .hInstance = hInstance, .lpszClassName = "DX12MSDemoWnd" };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, "DX12 Mesh Shader Skinning Minimal", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // DXGI / Device
    ComPtr<IDXGIFactory7> factory; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<ID3D12Device> device0; D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device0));
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
    if (FAILED(device0->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7))) ||
        opts7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
    { MessageBox(hwnd, "Mesh Shaders not supported.", "Error", MB_OK); return 0; }
    ComPtr<ID3D12Device2> device; device0.As(&device);

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    DXGI_SWAP_CHAIN_DESC1 sc{}; sc.Width = kWidth; sc.Height = kHeight; sc.BufferCount = 2; sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc = {1,0}; sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1; factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sc, nullptr, nullptr, &sc1);
    ComPtr<IDXGISwapChain3> swap; sc1.As(&swap); UINT frameIndex = swap->GetCurrentBackBufferIndex();

    // RTVs
    ComPtr<ID3D12DescriptorHeap> rtvHeap; D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.NumDescriptors=2; rtvDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)); auto rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    std::vector<ComPtr<ID3D12Resource>> backbuf(2);
    for (UINT i=0;i<2;i++) { swap->GetBuffer(i, IID_PPV_ARGS(&backbuf[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i)*SIZE_T(rtvInc);
        device->CreateRenderTargetView(backbuf[i].Get(), nullptr, h); }

    // Command list + fence
    ComPtr<ID3D12CommandAllocator> alloc; device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList6> list; device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    ComPtr<ID3D12Fence> fence; UINT64 fenceValue=0; device->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    auto WaitGPU = [&](){ fenceValue++; queue->Signal(fence.Get(), fenceValue);
        if (fence->GetCompletedValue() < fenceValue){ fence->SetEventOnCompletion(fenceValue, fenceEvent); WaitForSingleObject(fenceEvent, INFINITE);} };

    // Root signature: CBV b0 + SRVs t0..t3 as root descriptors
    D3D12_ROOT_PARAMETER1 params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; params[0].Descriptor = {0,0};
    for(int i=0;i<4;i++){ params[1+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[1+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL; params[1+i].Descriptor={(UINT)i,0}; }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{}; rsd.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;
    D3D12_ROOT_SIGNATURE_DESC1 d{}; d.NumParameters=5; d.pParameters=params; d.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT; rsd.Desc_1_1=d;
    ComPtr<ID3DBlob> rsBlob, rsErr; D3D12SerializeVersionedRootSignature(&rsd,&rsBlob,&rsErr);
    ComPtr<ID3D12RootSignature> rootSig; device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));

    // DXC load
    HMODULE dxcm = LoadLibrary("dxcompiler.dll");
    if(!dxcm){ MessageBox(hwnd,"dxcompiler.dll not found.","DXC missing",MB_OK); return 0; }
    auto DxcCreateInstance = reinterpret_cast<HRESULT (WINAPI *)(REFCLSID, REFIID, LPVOID*)>(GetProcAddress(dxcm,"DxcCreateInstance"));
    if(!DxcCreateInstance){ MessageBox(hwnd,"Failed to get DxcCreateInstance","DXC error",MB_OK); return 0; }
    ComPtr<IDxcUtils> dxcUtils; ComPtr<IDxcCompiler3> dxc; DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils)); DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
    auto asBlob = CompileHLSL(dxc,dxcUtils,L"shaders/as.hlsl",L"ASMain",L"as_6_5");
    auto msBlob = CompileHLSL(dxc,dxcUtils,L"shaders/ms.hlsl",L"MSMain",L"ms_6_5");
    auto psBlob = CompileHLSL(dxc,dxcUtils,L"shaders/ps.hlsl",L"PSMain",L"ps_6_6");

    // PSO via pipeline state stream
    D3D12_RT_FORMAT_ARRAY rtvFormats{}; rtvFormats.NumRenderTargets=1; rtvFormats.RTFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; ID3D12RootSignature* p; } sRoot{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rootSig.Get() };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_SHADER_BYTECODE BC; } sAS{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, { asBlob->GetBufferPointer(), asBlob->GetBufferSize() } };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_SHADER_BYTECODE BC; } sMS{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, { msBlob->GetBufferPointer(), msBlob->GetBufferSize() } };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_SHADER_BYTECODE BC; } sPS{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, { psBlob->GetBufferPointer(), psBlob->GetBufferSize() } };
    D3D12_RASTERIZER_DESC rast{ D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_RASTERIZER_DESC Desc; } sRast{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, rast };
    D3D12_BLEND_DESC blend{ FALSE, FALSE, { { FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                            D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL } } };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_BLEND_DESC Desc; } sBlend{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend };
    D3D12_DEPTH_STENCIL_DESC dss{}; dss.DepthEnable = FALSE; dss.StencilEnable = FALSE;
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_DEPTH_STENCIL_DESC Desc; } sDS{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, dss };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_RT_FORMAT_ARRAY Fmts; } sRTVs{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, rtvFormats };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; DXGI_SAMPLE_DESC SD; } sSamp{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, {1,0} };
    struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type; D3D12_PRIMITIVE_TOPOLOGY_TYPE PT; } sPT{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    const void* stream[] = { &sRoot, &sAS, &sMS, &sPS, &sRast, &sBlend, &sDS, &sRTVs, &sSamp, &sPT };
    D3D12_PIPELINE_STATE_STREAM_DESC pssDesc{}; pssDesc.SizeInBytes = sizeof(stream); pssDesc.pPipelineStateSubobjectStream = stream;
    ComPtr<ID3D12PipelineState> pso; if (FAILED(device->CreatePipelineState(&pssDesc, IID_PPV_ARGS(&pso))))
    { MessageBox(hwnd,"Failed to create PSO.","PSO error",MB_OK); return 0; }

    // Buffers: quad meshlet (4 verts, 2 tris), 2 bones
    VertexIn verts[4] = {
        {{-0.5f,  0.5f, 0.0f},{0,0,1},{0,0,0,0},{1,0,0,0}},
        {{ 0.5f,  0.5f, 0.0f},{0,0,1},{0,0,0,0},{1,0,0,0}},
        {{-0.5f, -0.5f, 0.0f},{0,0,1},{1,0,0,0},{1,0,0,0}},
        {{ 0.5f, -0.5f, 0.0f},{0,0,1},{1,0,0,0},{1,0,0,0}},
    };
    struct UINT3 { unsigned int x,y,z; };
    UINT3 triangles[2] = { {0,2,1}, {1,2,3} };
    MeshletData meshlet{4,2,0,0,0};

    auto CreateUpload = [&](size_t size, ComPtr<ID3D12Resource>& res){
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size; desc.Height = 1;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_UNKNOWN; desc.SampleDesc={1,0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res));
    };

    ComPtr<ID3D12Resource> vb, ib, mb, bones, cb;
    CreateUpload(sizeof(verts), vb); CreateUpload(sizeof(triangles), ib); CreateUpload(sizeof(meshlet), mb);
    CreateUpload(sizeof(float)*16*2, bones); CreateUpload(sizeof(GlobalsCB), cb);

    void* p; vb->Map(0,nullptr,&p); memcpy(p, verts, sizeof(verts)); vb->Unmap(0,nullptr);
    ib->Map(0,nullptr,&p); memcpy(p, triangles, sizeof(triangles)); ib->Unmap(0,nullptr);
    mb->Map(0,nullptr,&p); memcpy(p, &meshlet, sizeof(meshlet)); mb->Unmap(0,nullptr);

    // Main loop
    auto start = std::chrono::high_resolution_clock::now();
    bool running = true;
    while (running)
    {
        MSG msg{}; while (PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){ if (msg.message==WM_QUIT) running=false; TranslateMessage(&msg); DispatchMessage(&msg); }
        if (!running) break;

        // Update bones
        auto now = std::chrono::high_resolution_clock::now();
        float t = std::chrono::duration<float>(now - start).count();
        float angle = 0.5f * sinf(t*1.5f);

        float bonesCPU[32]; MakeIdentity(&bonesCPU[0]);
        float c = cosf(angle), s = sinf(angle); float b1[16]; MakeIdentity(b1);
        b1[5]=c; b1[6]=s; b1[9]=-s; b1[10]=c; b1[13]=-0.25f; // rotate around X, translate downward
        memcpy(&bonesCPU[16], b1, sizeof(b1));
        bones->Map(0,nullptr,&p); memcpy(p, bonesCPU, sizeof(bonesCPU)); bones->Unmap(0,nullptr);

        GlobalsCB g{}; MakeOrtho(g.mvp, (float)kWidth, (float)kHeight); g.time = t;
        cb->Map(0,nullptr,&p); memcpy(p, &g, sizeof(g)); cb->Unmap(0,nullptr);

        alloc->Reset(); list->Reset(alloc.Get(), pso.Get());

        // Transition to RT
        D3D12_RESOURCE_BARRIER toRT{}; toRT.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; toRT.Transition.pResource=backbuf[frameIndex].Get();
        toRT.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; toRT.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;
        toRT.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; list->ResourceBarrier(1,&toRT);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += SIZE_T(frameIndex)*SIZE_T(rtvInc);
        FLOAT clear[4] = {0.1f,0.1f,0.12f,1.0f}; list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        list->SetGraphicsRootSignature(rootSig.Get());
        list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(1, vb->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(2, ib->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(3, mb->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(4, bones->GetGPUVirtualAddress());

        D3D12_VIEWPORT vp{0,0,(float)kWidth,(float)kHeight,0,1}; D3D12_RECT sc{0,0,(LONG)kWidth,(LONG)kHeight};
        list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&sc);

        list->DispatchMesh(1,1,1);

        D3D12_RESOURCE_BARRIER toPresent{}; toPresent.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; toPresent.Transition.pResource=backbuf[frameIndex].Get();
        toPresent.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; toPresent.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT; list->ResourceBarrier(1,&toPresent);

        list->Close(); ID3D12CommandList* lists[] = { list.Get() }; queue->ExecuteCommandLists(1, lists);
        swap->Present(1,0); WaitGPU(); frameIndex = swap->GetCurrentBackBufferIndex();
    }

    WaitGPU(); CloseHandle(fenceEvent); return 0;
}
