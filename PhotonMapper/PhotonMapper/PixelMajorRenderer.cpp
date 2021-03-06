//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "PixelMajorRenderer.h"
#include "DirectXRaytracingHelper.h"
#include "CompiledShaders\PixelMajorFirstPassShader.hlsl.h"
#include "CompiledShaders\PixelMajorFinalPassShader.hlsl.h"

using namespace std;
using namespace DX;

const wchar_t* PixelMajorRenderer::c_hitGroupName = L"MyHitGroup";
const wchar_t* PixelMajorRenderer::c_raygenShaderName = L"MyRaygenShader";
const wchar_t* PixelMajorRenderer::c_closestHitShaderName = L"MyClosestHitShader";
const wchar_t* PixelMajorRenderer::c_missShaderName = L"MyMissShader";

const LPCWSTR PixelMajorRenderer::c_computeShaderPass0 = L"PixelMajorComputePass0.cso";
const LPCWSTR PixelMajorRenderer::c_computeShaderPass01 = L"PixelMajorComputePass0.cso";
const LPCWSTR PixelMajorRenderer::c_computeShaderPass1 = L"PixelMajorComputePass1.cso";
const LPCWSTR PixelMajorRenderer::c_computeShaderPass2 = L"PixelMajorComputePass2.cso";
const LPCWSTR PixelMajorRenderer::c_computeShaderPass3 = L"PixelMajorComputePass3.cso";

PixelMajorRenderer::PixelMajorRenderer(const DXRPhotonMapper::PMScene& scene, UINT width, UINT height, std::wstring name) :
    PhotonBaseRenderer(scene, width, height, name),
    m_raytracingOutputResourceUAVDescriptorHeapIndex(UINT_MAX),
    m_curRotationAngleRad(0.0f),
    m_calculatePhotonMap(false),
    m_fenceValue(0)
{
    m_forceComputeFallback = false;
    SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
    UpdateForSizeChange(width, height);
}

void PixelMajorRenderer::EnableDirectXRaytracing(IDXGIAdapter1* adapter)
{
    // Fallback Layer uses an experimental feature and needs to be enabled before creating a D3D12 device.
    bool isFallbackSupported = EnableComputeRaytracingFallback(adapter);

    if (!isFallbackSupported)
    {
        OutputDebugString(
            L"Warning: Could not enable Compute Raytracing Fallback (D3D12EnableExperimentalFeatures() failed).\n" \
            L"         Possible reasons: your OS is not in developer mode.\n\n");
    }

    m_isDxrSupported = IsDirectXRaytracingSupported(adapter);

    if (!m_isDxrSupported)
    {
        OutputDebugString(L"Warning: DirectX Raytracing is not supported by your GPU and driver.\n\n");

        ThrowIfFalse(isFallbackSupported,
            L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n"\
            L"Possible reasons: your OS is not in developer mode.\n\n");
        m_raytracingAPI = RaytracingAPI::FallbackLayer;
    }
}

void PixelMajorRenderer::OnInit()
{
    m_deviceResources = std::make_unique<DeviceResources>(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_UNKNOWN,
        FrameCount,
        D3D_FEATURE_LEVEL_11_0,
        // Sample shows handling of use cases with tearing support, which is OS dependent and has been supported since TH2.
        // Since the Fallback Layer requires Fall Creator's update (RS3), we don't need to handle non-tearing cases.
        DeviceResources::c_RequireTearingSupport,
        m_adapterIDoverride
        );
    m_deviceResources->RegisterDeviceNotify(this);
    m_deviceResources->SetWindow(Win32Application::GetHwnd(), m_width, m_height);
    m_deviceResources->InitializeDXGIAdapter();
    EnableDirectXRaytracing(m_deviceResources->GetAdapter());

    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    InitializeScene();

    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();

    m_calculatePhotonMap = true;
}

// Update camera matrices passed into the shader.
void PixelMajorRenderer::UpdateCameraMatrices()
{
    OutputDebugString(L"UpdateCameraMatrices\n");
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    m_sceneCB[frameIndex].cameraPosition = m_eye;
    float fovAngleY = 45.0f;
    XMMATRIX view = XMMatrixLookAtLH(m_eye, m_at, m_up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovAngleY), m_aspectRatio, 1.0f, 125.0f);
    XMMATRIX viewProj = view * proj;

    m_sceneCB[frameIndex].projectionToWorld = XMMatrixInverse(nullptr, viewProj);
    m_sceneCB[frameIndex].viewProj = viewProj;
}

// Initialize scene rendering parameters.
void PixelMajorRenderer::InitializeScene()
{
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    // Setup materials.
    {
        m_cubeCB.albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    // Setup camera.
    {
        // Initialize the view and projection inverse matrices.
        //m_eye = { 0.0f, 2.0f, -5.0f, 1.0f };
        m_eye = { 0.0f, 3.0f, -15.0f, 1.0f };
        m_at = { 0.0f, 0.0f, 0.0f, 1.0f };
        XMVECTOR right = { 1.0f, 0.0f, 0.0f, 0.0f };

        XMVECTOR direction = XMVector4Normalize(m_at - m_eye);
        m_up = XMVector3Normalize(XMVector3Cross(direction, right));
        //m_up = { 0.0f, 1.0f, 0.0f, 0.0f };

        // Rotate camera around Y axis.
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(-0.0f)); // Originally 45.0f
        m_eye = XMVector3Transform(m_eye, rotate);
        m_up = XMVector3Transform(m_up, rotate);

        UpdateCameraMatrices();
    }

    // Setup lights.
    {
        // Initialize the lighting parameters.
        XMFLOAT4 lightPosition;
        XMFLOAT4 lightAmbientColor;
        XMFLOAT4 lightDiffuseColor;

        //lightPosition = XMFLOAT4(0.0f, 1.8f, -3.0f, 0.0f);
        lightPosition = XMFLOAT4(3.0f, 0.0f, -3.0f, 0.0f);
        m_sceneCB[frameIndex].lightPosition = XMLoadFloat4(&lightPosition);

        lightAmbientColor = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
        m_sceneCB[frameIndex].lightAmbientColor = XMLoadFloat4(&lightAmbientColor);

        //lightDiffuseColor = XMFLOAT4(0.5f, 0.0f, 0.0f, 1.0f);
        lightDiffuseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        m_sceneCB[frameIndex].lightDiffuseColor = XMLoadFloat4(&lightDiffuseColor);
    }

    // Apply the initial values to all frames' buffer instances.
    for (auto& sceneCB : m_sceneCB)
    {
        sceneCB = m_sceneCB[frameIndex];
    }
}

// Create constant buffers.
void PixelMajorRenderer::CreateConstantBuffers()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto frameCount = m_deviceResources->GetBackBufferCount();

    // Create the constant buffer memory and map the CPU and GPU addresses
    const D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Allocate one constant buffer per frame, since it gets updated every frame.
    size_t cbSize = frameCount * sizeof(AlignedSceneConstantBuffer);
    const D3D12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &constantBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_perFrameConstants)));

    // Map the constant buffer and cache its heap pointers.
    // We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
    CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
    ThrowIfFailed(m_perFrameConstants->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedConstantData)));
}

// Create constant buffers.
void PixelMajorRenderer::CreateComputeConstantBuffer()
{
    auto device = m_deviceResources->GetD3DDevice();

    // Create the constant buffer memory and map the CPU and GPU addresses
    const D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    size_t cbSize = sizeof(AlignedComputeConstantBuffer);
    const D3D12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &constantBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_computeConstantRes)));

    // Map the constant buffer and cache its heap pointers.
    // We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
    CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
    ThrowIfFailed(m_computeConstantRes->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedComputeConstantData)));
}

// Create resources that depend on the device.
void PixelMajorRenderer::CreateDeviceDependentResources()
{
    // Initialize raytracing pipeline.

    // Create raytracing interfaces: raytracing device and commandlist.
    CreateRaytracingInterfaces();

    // Create root signatures for the shaders.

    CreateFirstPassRootSignatures();
    CreateSecondPassRootSignatures();

    CreateComputeFirstPassRootSignature();

    // Create a raytracing pipeline state object which defines the binding of shaders, state and resources to be used during raytracing.
    // Temporary Testing: Should be put back later on.
    CreateFirstPassPhotonPipelineStateObject();
    CreateSecondPassPhotonPipelineStateObject();

    CreateComputePipelineStateObject(c_computeShaderPass0, m_computeInitializePSO);
    CreateComputePipelineStateObject(c_computeShaderPass01, m_computeInitializePSO2);
    CreateComputePipelineStateObject(c_computeShaderPass1, m_computeFirstPassPSO);
    CreateComputePipelineStateObject(c_computeShaderPass2, m_computeSecondPassPSO);
    CreateComputePipelineStateObject(c_computeShaderPass3, m_computeThirdPassPSO);

    // Create a heap for descriptors.
    CreateDescriptorHeap();

    // Build geometry to be used in the sample.
    BuildGeometryBuffers();

    // Build supporting contant buffer views
    BuildGeometrySceneBufferDesc();
    BuildMaterialBuffer();
    BuildLightBuffer();

    // Build raytracing acceleration structures from the generated geometry.
    BuildGeometryAccelerationStructures();

    // Create constant buffers for the geometry and the scene.
    CreateConstantBuffers();

    // Create constant buffers for the compute pipeline
    CreateComputeConstantBuffer();

    // Build shader tables, which define shaders and their local root arguments.
    BuildFirstPassShaderTables();
    BuildSecondPassShaderTables();

    // Create a fence for tracking GPU execution progress.
    auto device = m_deviceResources->GetD3DDevice();
    /*
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ThrowIfFailed(m_fallbackDevice->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(m_dxrDevice->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    }
    */
    ThrowIfFailed(device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue++;

    m_fenceEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    if (!m_fenceEvent.IsValid())
    {
        ThrowIfFailed(E_FAIL, L"CreateEvent failed.\n");
    }
}

void PixelMajorRenderer::SerializeAndCreateRaytracingRootSignature(D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rootSig)
{
    auto device = m_deviceResources->GetD3DDevice();
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;

    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ThrowIfFailed(m_fallbackDevice->D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
        ThrowIfFailed(m_fallbackDevice->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*rootSig))));
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
        ThrowIfFailed(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*rootSig))));
    }
}

void PixelMajorRenderer::CreateFirstPassRootSignatures()
{
    auto device = m_deviceResources->GetD3DDevice();

    // Global Root Signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    {
        const UINT numPrimitives = UINT(m_scene.m_primitives.size());
        const UINT numMaterials = UINT(m_scene.m_materials.size());
        const UINT numLights = UINT(m_scene.m_lights.size());

        CD3DX12_DESCRIPTOR_RANGE ranges[6]; // Perfomance TIP: Order from most frequent to least frequent.
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, NumRenderTargets + NumGBuffers + NumPhotonCountBuffer + NumPhotonScanBuffer + NumPhotonTempIndexBuffer, 0);  // 1 output texture + a couple of GBuffers
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numPrimitives, 0, 1);  // index buffer array
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numPrimitives, 0, 2);  // vertex buffer array
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numPrimitives, 0, 1);  // CBV - scene buffer array
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numMaterials, 0, 2);  // CBV - material buffer array
        ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numLights, 0, 3);  // CBV - light buffer array

        CD3DX12_ROOT_PARAMETER rootParameters[GlobalRootSignatureParamsWithPrimitives::Count];
        rootParameters[GlobalRootSignatureParamsWithPrimitives::OutputViewSlot].InitAsDescriptorTable(1, &ranges[0]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::AccelerationStructureSlot].InitAsShaderResourceView(0);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::IndexBuffersSlot].InitAsDescriptorTable(1, &ranges[1]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::VertexBuffersSlot].InitAsDescriptorTable(1, &ranges[2]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::GeomIndexSlot].InitAsDescriptorTable(1, &ranges[3]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::MaterialSlot].InitAsDescriptorTable(1, &ranges[4]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::LightSlot].InitAsDescriptorTable(1, &ranges[5]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::SceneConstantSlot].InitAsConstantBufferView(0);

        CD3DX12_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        SerializeAndCreateRaytracingRootSignature(globalRootSignatureDesc, &m_firstPassGlobalRootSignature);
    }

    // Local Root Signature
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    {
        CD3DX12_ROOT_PARAMETER rootParameters[LocalRootSignatureParams::Count];
        rootParameters[LocalRootSignatureParams::CubeConstantSlot].InitAsConstants(SizeOfInUint32(m_cubeCB), 1);
        CD3DX12_ROOT_SIGNATURE_DESC localRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        localRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        SerializeAndCreateRaytracingRootSignature(localRootSignatureDesc, &m_firstPassLocalRootSignature);
    }
}

void PixelMajorRenderer::CreateSecondPassRootSignatures()
{
    auto device = m_deviceResources->GetD3DDevice();

    // Global Root Signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    {
        const UINT numPrimitives = UINT(m_scene.m_primitives.size());
        const UINT numMaterials = UINT(m_scene.m_materials.size());
        const UINT numLights = UINT(m_scene.m_lights.size());

        CD3DX12_DESCRIPTOR_RANGE ranges[6]; // Perfomance TIP: Order from most frequent to least frequent.
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, NumRenderTargets + NumGBuffers + NumPhotonCountBuffer + NumPhotonScanBuffer + NumPhotonTempIndexBuffer, 0);  // 1 output texture + a couple of GBuffers
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numPrimitives, 0, 1);  // index buffer array
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numPrimitives, 0, 2);  // vertex buffer array
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numPrimitives, 0, 1);  // CBV - scene buffer array
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numMaterials, 0, 2);  // CBV - material buffer array
        ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numLights, 0, 3);  // CBV - light buffer array

        CD3DX12_ROOT_PARAMETER rootParameters[GlobalRootSignatureParamsWithPrimitives::Count];
        rootParameters[GlobalRootSignatureParamsWithPrimitives::OutputViewSlot].InitAsDescriptorTable(1, &ranges[0]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::AccelerationStructureSlot].InitAsShaderResourceView(0);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::IndexBuffersSlot].InitAsDescriptorTable(1, &ranges[1]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::VertexBuffersSlot].InitAsDescriptorTable(1, &ranges[2]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::GeomIndexSlot].InitAsDescriptorTable(1, &ranges[3]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::MaterialSlot].InitAsDescriptorTable(1, &ranges[4]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::LightSlot].InitAsDescriptorTable(1, &ranges[5]);
        rootParameters[GlobalRootSignatureParamsWithPrimitives::SceneConstantSlot].InitAsConstantBufferView(0);

        CD3DX12_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        SerializeAndCreateRaytracingRootSignature(globalRootSignatureDesc, &m_secondPassGlobalRootSignature);
    }

    // Local Root Signature
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    {
        CD3DX12_ROOT_PARAMETER rootParameters[LocalRootSignatureParams::Count];
        rootParameters[LocalRootSignatureParams::CubeConstantSlot].InitAsConstants(SizeOfInUint32(m_cubeCB), 1);
        CD3DX12_ROOT_SIGNATURE_DESC localRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        localRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        SerializeAndCreateRaytracingRootSignature(localRootSignatureDesc, &m_secondPassLocalRootSignature);
    }
}

void PixelMajorRenderer::CreateComputeFirstPassRootSignature()
{
    auto device = m_deviceResources->GetD3DDevice();
    CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, NumRenderTargets + NumGBuffers + NumPhotonCountBuffer + NumPhotonScanBuffer + NumPhotonTempIndexBuffer, 0);

    CD3DX12_ROOT_PARAMETER1 rootParameters[ComputeRootSignatureParams::Count];
    rootParameters[ComputeRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &ranges[0]);
    rootParameters[ComputeRootSignatureParams::ParamConstantBuffer].InitAsConstantBufferView(0); // TODO: Not yet ready

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
    computeRootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)));
}

// Create raytracing device and command list.
void PixelMajorRenderer::CreateRaytracingInterfaces()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();

    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        CreateRaytracingFallbackDeviceFlags createDeviceFlags = m_forceComputeFallback ?
            CreateRaytracingFallbackDeviceFlags::ForceComputeFallback :
            CreateRaytracingFallbackDeviceFlags::None;
        ThrowIfFailed(D3D12CreateRaytracingFallbackDevice(device, createDeviceFlags, 0, IID_PPV_ARGS(&m_fallbackDevice)));
        m_fallbackDevice->QueryRaytracingCommandList(commandList, IID_PPV_ARGS(&m_fallbackCommandList));
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&m_dxrDevice)), L"Couldn't get DirectX Raytracing interface for the device.\n");
        ThrowIfFailed(commandList->QueryInterface(IID_PPV_ARGS(&m_dxrCommandList)), L"Couldn't get DirectX Raytracing interface for the command list.\n");
    }
}

// Local root signature and shader association
// This is a root signature that enables a shader to have unique arguments that come from shader tables.
void PixelMajorRenderer::CreateLocalRootSignatureSubobjects(CD3D12_STATE_OBJECT_DESC* raytracingPipeline, ComPtr<ID3D12RootSignature>* rootSig)
{
    // Ray gen and miss shaders in this sample are not using a local root signature and thus one is not associated with them.

    // Local root signature to be used in a hit group.
    auto localRootSignature = raytracingPipeline->CreateSubobject<CD3D12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
    localRootSignature->SetRootSignature(rootSig->Get());
    // Define explicit shader association for the local root signature. 
    {
        auto rootSignatureAssociation = raytracingPipeline->CreateSubobject<CD3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
        rootSignatureAssociation->SetSubobjectToAssociate(*localRootSignature);
        rootSignatureAssociation->AddExport(c_hitGroupName);
    }
}

// Create a raytracing pipeline state object (RTPSO).
// An RTPSO represents a full set of shaders reachable by a DispatchRays() call,
// with all configuration options resolved, such as local signatures and other state.
void PixelMajorRenderer::CreateFirstPassPhotonPipelineStateObject()
{
    // Create 7 subobjects that combine into a RTPSO:
    // Subobjects need to be associated with DXIL exports (i.e. shaders) either by way of default or explicit associations.
    // Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
    // This simple sample utilizes default shader association except for local root signature subobject
    // which has an explicit association specified purely for demonstration purposes.
    // 1 - DXIL library
    // 1 - Triangle hit group
    // 1 - Shader config
    // 2 - Local root signature and association
    // 1 - Global root signature
    // 1 - Pipeline config
    CD3D12_STATE_OBJECT_DESC raytracingPipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };


    // DXIL library
    // This contains the shaders and their entrypoints for the state object.
    // Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
    auto lib = raytracingPipeline.CreateSubobject<CD3D12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE libdxil = CD3DX12_SHADER_BYTECODE((void *)g_pPixelMajorFirstPassShader, ARRAYSIZE(g_pPixelMajorFirstPassShader));
    lib->SetDXILLibrary(&libdxil);
    // Define which shader exports to surface from the library.
    // If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
    // In this sample, this could be ommited for convenience since the sample uses all shaders in the library. 
    {
        lib->DefineExport(c_raygenShaderName);
        lib->DefineExport(c_closestHitShaderName);
        lib->DefineExport(c_missShaderName);
    }

    // Triangle hit group
    // A hit group specifies closest hit, any hit and intersection shaders to be executed when a ray intersects the geometry's triangle/AABB.
    // In this sample, we only use triangle geometry with a closest hit shader, so others are not set.
    auto hitGroup = raytracingPipeline.CreateSubobject<CD3D12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(c_closestHitShaderName);
    hitGroup->SetHitGroupExport(c_hitGroupName);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // Shader config
    // Defines the maximum sizes in bytes for the ray payload and attribute structure.
    auto shaderConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize = 3 * sizeof(XMFLOAT4) + 2 * sizeof(XMFLOAT3);    // float4 pixelColor
    UINT attributeSize = sizeof(XMFLOAT2);  // float2 barycentrics
    shaderConfig->Config(payloadSize, attributeSize);

    // Local root signature and shader association
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    CreateLocalRootSignatureSubobjects(&raytracingPipeline, &m_firstPassLocalRootSignature);

    // Global root signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    auto globalRootSignature = raytracingPipeline.CreateSubobject<CD3D12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_firstPassGlobalRootSignature.Get());

    // Pipeline config
    // Defines the maximum TraceRay() recursion depth.
    auto pipelineConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    // PERFOMANCE TIP: Set max recursion depth as low as needed 
    // as drivers may apply optimization strategies for low recursion depths.
    UINT maxRecursionDepth = MAX_RAY_RECURSION_DEPTH; // ~ primary rays only. // TODO
    pipelineConfig->Config(maxRecursionDepth);

#if _DEBUG
    PrintStateObjectDesc(raytracingPipeline);
#endif

    // Create the state object.
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ThrowIfFailed(m_fallbackDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_fallbackFirstPassStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(m_dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrFirstPassStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }
}

// Creates the PSO for Ray tracing the photons
void PixelMajorRenderer::CreateSecondPassPhotonPipelineStateObject()
{
    CD3D12_STATE_OBJECT_DESC raytracingPipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    // DXIL library
    // This contains the shaders and their entrypoints for the state object.
    // Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
    auto lib = raytracingPipeline.CreateSubobject<CD3D12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE libdxil = CD3DX12_SHADER_BYTECODE((void *)g_pPixelMajorFinalPassShader, ARRAYSIZE(g_pPixelMajorFinalPassShader));
    lib->SetDXILLibrary(&libdxil);
    // Define which shader exports to surface from the library.
    // If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
    // In this sample, this could be ommited for convenience since the sample uses all shaders in the library. 
    {
        lib->DefineExport(c_raygenShaderName);
        lib->DefineExport(c_closestHitShaderName);
        lib->DefineExport(c_missShaderName);
    }

    // Triangle hit group
    // A hit group specifies closest hit, any hit and intersection shaders to be executed when a ray intersects the geometry's triangle/AABB.
    // In this sample, we only use triangle geometry with a closest hit shader, so others are not set.
    auto hitGroup = raytracingPipeline.CreateSubobject<CD3D12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(c_closestHitShaderName);
    hitGroup->SetHitGroupExport(c_hitGroupName);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // Shader config
    // Defines the maximum sizes in bytes for the ray payload and attribute structure.
    auto shaderConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize = 3 * sizeof(XMFLOAT4) + 2 * sizeof(XMFLOAT3);    // float4 pixelColor
    UINT attributeSize = sizeof(XMFLOAT2);  // float2 barycentrics
    shaderConfig->Config(payloadSize, attributeSize);

    // Local root signature and shader association
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    CreateLocalRootSignatureSubobjects(&raytracingPipeline, &m_secondPassLocalRootSignature);

    // Global root signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    auto globalRootSignature = raytracingPipeline.CreateSubobject<CD3D12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_secondPassGlobalRootSignature.Get());

    // Pipeline config
    // Defines the maximum TraceRay() recursion depth.
    auto pipelineConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    // PERFOMANCE TIP: Set max recursion depth as low as needed 
    // as drivers may apply optimization strategies for low recursion depths.
    UINT maxRecursionDepth = 1; // ~ primary rays only. // TODO
    pipelineConfig->Config(maxRecursionDepth);

#if _DEBUG
    PrintStateObjectDesc(raytracingPipeline);
#endif

    // Create the state object.
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ThrowIfFailed(m_fallbackDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_fallbackSecondPassStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(m_dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrSecondPassStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }
}

void PixelMajorRenderer::CreateComputePipelineStateObject(const LPCWSTR& compiledShaderName, ComPtr<ID3D12PipelineState>& computePipeline)
{
    auto device = m_deviceResources->GetD3DDevice();

    UINT fileSize = 0;
    UINT8* shader;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(compiledShaderName).c_str(), &shader, &fileSize));

    // Describe and create the compute pipeline state object (PSO).
    D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
    computePsoDesc.pRootSignature = m_computeRootSignature.Get();
    computePsoDesc.CS = CD3DX12_SHADER_BYTECODE((void *)shader, fileSize);

    ThrowIfFailed(device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&computePipeline)));
}

// Create 2D output texture for raytracing.
void PixelMajorRenderer::CreateRaytracingOutputResource()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto backbufferFormat = m_deviceResources->GetBackBufferFormat();

    // Create the output resource. The dimensions and format should match the swap-chain.
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(backbufferFormat, m_width, m_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_raytracingOutput)));
    NAME_D3D12_OBJECT(m_raytracingOutput);

    D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
    m_raytracingOutputResourceUAVDescriptorHeapIndex = AllocateDescriptor(&uavDescriptorHandle, m_raytracingOutputResourceUAVDescriptorHeapIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_raytracingOutput.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
    m_raytracingOutputResourceUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), m_raytracingOutputResourceUAVDescriptorHeapIndex, m_descriptorSize);
}

void PixelMajorRenderer::CreateGBuffers()
{
    // There are 3 G Buffers defined here. All of them are unordered access views
    // 1. Photon Position
    // 2. Photon Color
    // 3. Photon Normal (local space?)

    // TODO Might change later.
    int width = sqrt(NumPhotons);
    //int log = ilog2ceil(width); // For exclusive scan, number of elements need to be power of 2
    //width = pow(2, log);
    int height = width;

    if (NumPhotons % width != 0)
    {
        height++;
    }

    m_gBufferWidth = min(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, width);
    m_gBufferHeight = min(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, height);
    m_gBufferDepth = MAX_RAY_RECURSION_DEPTH;

    auto device = m_deviceResources->GetD3DDevice();
    auto backbufferFormat = m_deviceResources->GetBackBufferFormat();

    // Create the output resource. The dimensions and format should match the swap-chain.
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, m_gBufferWidth, m_gBufferHeight, m_gBufferDepth, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    m_gBuffers.clear();

    for (int i = 0; i < NumGBuffers; ++i)
    {
        GBuffer gBuffer = {};

        ThrowIfFailed(device->CreateCommittedResource(
            &defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&gBuffer.textureResource)));
        NAME_D3D12_OBJECT(gBuffer.textureResource);

        gBuffer.uavDescriptorHeapIndex = UINT_MAX;

        D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
        gBuffer.uavDescriptorHeapIndex = AllocateDescriptor(&uavDescriptorHandle, gBuffer.uavDescriptorHeapIndex);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
        UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; // TODO is this right?
        UAVDesc.Texture2DArray.ArraySize = m_gBufferDepth;
        device->CreateUnorderedAccessView(gBuffer.textureResource.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
        gBuffer.uavGPUDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), gBuffer.uavDescriptorHeapIndex, m_descriptorSize);

        m_gBuffers.push_back(gBuffer);
    }
}

void PixelMajorRenderer::CreatePhotonCountBuffer(GBuffer& gBuffer)
{
    // Create a buffer for hash grid construction
    // A texture that stores the number of photons in each cell
    // Divide the entire scene space into cells

    auto device = m_deviceResources->GetD3DDevice();

    // Create the output resource. The dimensions and format should match the swap-chain.
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, NUM_CELLS_IN_X, NUM_CELLS_IN_Y, NUM_CELLS_IN_Z, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    gBuffer = {};

    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&gBuffer.textureResource)));

    gBuffer.uavDescriptorHeapIndex = UINT_MAX;

    D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
    gBuffer.uavDescriptorHeapIndex = AllocateDescriptor(&uavDescriptorHandle, gBuffer.uavDescriptorHeapIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    UAVDesc.Texture2DArray.ArraySize = NUM_CELLS_IN_Z;
    device->CreateUnorderedAccessView(gBuffer.textureResource.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
    gBuffer.uavGPUDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), gBuffer.uavDescriptorHeapIndex, m_descriptorSize);
}

void PixelMajorRenderer::CreateDescriptorHeap()
{
    auto device = m_deviceResources->GetD3DDevice();

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
    // Allocate a heap for descriptors:
    // n - vertex SRVs
    // n - index SRVs
    // n - constant buffer views for scene buffer info
    // n - constant buffer views for materials
    // n - constant buffer views for lights
    // n - bottom level acceleration structure fallback wrapped pointer UAVs
    // n - top level acceleration structure fallback wrapped pointer UAVs
    descriptorHeapDesc.NumDescriptors = NumRenderTargets + NumGBuffers + NumPhotonCountBuffer + NumPhotonScanBuffer + NumPhotonTempIndexBuffer + GetNumDescriptorsForScene();
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descriptorHeapDesc.NodeMask = 0;
    device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
    NAME_D3D12_OBJECT(m_descriptorHeap);

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// Build shader tables.
// This encapsulates all shader records - shaders and the arguments for their local root signatures.
void PixelMajorRenderer::BuildFirstPassShaderTables()
{
    auto device = m_deviceResources->GetD3DDevice();

    void* rayGenShaderIdentifier;
    void* missShaderIdentifier;
    void* hitGroupShaderIdentifier;

    m_firstPassShaderTableRes = {};

    auto GetShaderIdentifiers = [&](auto* stateObjectProperties)
    {
        rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_raygenShaderName);
        missShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_missShaderName);
        hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_hitGroupName);
    };

    // Get shader identifiers.
    UINT shaderIdentifierSize;
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        GetShaderIdentifiers(m_fallbackFirstPassStateObject.Get());
        shaderIdentifierSize = m_fallbackDevice->GetShaderIdentifierSize();
    }
    else // DirectX Raytracing
    {
        ComPtr<ID3D12StateObjectPropertiesPrototype> stateObjectProperties;
        ThrowIfFailed(m_dxrFirstPassStateObject.As(&stateObjectProperties));
        GetShaderIdentifiers(stateObjectProperties.Get());
        shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    }

    // Ray gen shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable rayGenShaderTable(device, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
        rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
        m_firstPassShaderTableRes.m_rayGenShaderTable = rayGenShaderTable.GetResource();
    }

    // Miss shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable missShaderTable(device, numShaderRecords, shaderRecordSize, L"MissShaderTable");
        missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize));
        m_firstPassShaderTableRes.m_missShaderTable = missShaderTable.GetResource();
    }

    // Hit group shader table
    {
        struct RootArguments {
            CubeConstantBuffer cb;
        } rootArguments;
        rootArguments.cb = m_cubeCB;

        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize + sizeof(rootArguments);
        ShaderTable hitGroupShaderTable(device, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
        hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(rootArguments)));
        m_firstPassShaderTableRes.m_hitGroupShaderTable = hitGroupShaderTable.GetResource();
    }
}


// Build shader tables.
// This encapsulates all shader records - shaders and the arguments for their local root signatures.
void PixelMajorRenderer::BuildSecondPassShaderTables()
{
    auto device = m_deviceResources->GetD3DDevice();

    void* rayGenShaderIdentifier;
    void* missShaderIdentifier;
    void* hitGroupShaderIdentifier;

    m_secondPassShaderTableRes = {};

    auto GetShaderIdentifiers = [&](auto* stateObjectProperties)
    {
        rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_raygenShaderName);
        missShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_missShaderName);
        hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_hitGroupName);
    };

    // Get shader identifiers.
    UINT shaderIdentifierSize;
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        GetShaderIdentifiers(m_fallbackSecondPassStateObject.Get());
        shaderIdentifierSize = m_fallbackDevice->GetShaderIdentifierSize();
    }
    else // DirectX Raytracing
    {
        ComPtr<ID3D12StateObjectPropertiesPrototype> stateObjectProperties;
        ThrowIfFailed(m_dxrFirstPassStateObject.As(&stateObjectProperties));
        GetShaderIdentifiers(stateObjectProperties.Get());
        shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    }

    // Ray gen shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable rayGenShaderTable(device, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
        rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
        m_secondPassShaderTableRes.m_rayGenShaderTable = rayGenShaderTable.GetResource();
    }

    // Miss shader table
    {
        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize;
        ShaderTable missShaderTable(device, numShaderRecords, shaderRecordSize, L"MissShaderTable");
        missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize));
        m_secondPassShaderTableRes.m_missShaderTable = missShaderTable.GetResource();
    }

    // Hit group shader table
    {
        struct RootArguments {
            CubeConstantBuffer cb;
        } rootArguments;
        rootArguments.cb = m_cubeCB;

        UINT numShaderRecords = 1;
        UINT shaderRecordSize = shaderIdentifierSize + sizeof(rootArguments);
        ShaderTable hitGroupShaderTable(device, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
        hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(rootArguments)));
        m_secondPassShaderTableRes.m_hitGroupShaderTable = hitGroupShaderTable.GetResource();
    }
}

void PixelMajorRenderer::SelectRaytracingAPI(RaytracingAPI type)
{
    if (type == RaytracingAPI::FallbackLayer)
    {
        m_raytracingAPI = type;
    }
    else // DirectX Raytracing
    {
        if (m_isDxrSupported)
        {
            m_raytracingAPI = type;
        }
        else
        {
            OutputDebugString(L"Invalid selection - DXR is not available.\n");
        }
    }
}

void PixelMajorRenderer::OnKeyDown(UINT8 key)
{
    // Store previous values.
    RaytracingAPI previousRaytracingAPI = m_raytracingAPI;
    bool previousForceComputeFallback = m_forceComputeFallback;

    switch (key)
    {
    case VK_NUMPAD1:
    case '1': // Fallback Layer
        m_forceComputeFallback = false;
        SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
        break;
    case VK_NUMPAD2:
    case '2': // Fallback Layer + force compute path
        m_forceComputeFallback = true;
        SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
        break;
    case VK_NUMPAD3:
    case '3': // DirectX Raytracing
        SelectRaytracingAPI(RaytracingAPI::DirectXRaytracing);
        break;

        // Camera Movements
    case 'W':
    {
		XMVECTOR tempAt = 0.1f * XMVector3Normalize(m_at - m_eye);
        XMMATRIX translate = XMMatrixTranslation(XMVectorGetX(tempAt), XMVectorGetY(tempAt), XMVectorGetZ(tempAt));
        m_eye = XMVector3Transform(m_eye, translate);
        //m_up = XMVector3Transform(m_up, translate);
        m_at = XMVector3Transform(m_at, translate);
        //UpdateCameraMatrices();
		cameraNeedsUpdate = true;
    }
    break;

    case 'S':
    {
		XMVECTOR tempAt = -0.1f * XMVector3Normalize(m_at - m_eye);
		XMMATRIX translate = XMMatrixTranslation(XMVectorGetX(tempAt), XMVectorGetY(tempAt), XMVectorGetZ(tempAt));
        m_eye = XMVector3Transform(m_eye, translate);
        //m_up = XMVector3Transform(m_up, translate);
        m_at = XMVector3Transform(m_at, translate);
        //UpdateCameraMatrices();
		cameraNeedsUpdate = true;
    }
    break;

    case 'Q':
    {
		XMVECTOR tempUp = -0.1f * m_up;
        XMMATRIX translate = XMMatrixTranslation(XMVectorGetX(tempUp), XMVectorGetY(tempUp), XMVectorGetZ(tempUp));
        m_eye = XMVector3Transform(m_eye, translate);
        //m_up = XMVector3Transform(m_up, translate);
        m_at = XMVector3Transform(m_at, translate);
        //UpdateCameraMatrices();
		cameraNeedsUpdate = true;
    }
    break;

    case 'E':
    {
		XMVECTOR tempUp = 0.1f * m_up;
		XMMATRIX translate = XMMatrixTranslation(XMVectorGetX(tempUp), XMVectorGetY(tempUp), XMVectorGetZ(tempUp));
        m_eye = XMVector3Transform(m_eye, translate);
        //m_up = XMVector3Transform(m_up, translate);
        m_at = XMVector3Transform(m_at, translate);
        //UpdateCameraMatrices();
		cameraNeedsUpdate = true;
    }
    break;

	case 'A':
	{
		XMVECTOR tempAt = XMVector3Normalize(m_at - m_eye);
		XMVECTOR tempRight = 0.1f * XMVector3Normalize(XMVector3Cross(tempAt, m_up));
		XMMATRIX translate = XMMatrixTranslation(XMVectorGetX(tempRight), XMVectorGetY(tempRight), XMVectorGetZ(tempRight));
		m_eye = XMVector3Transform(m_eye, translate);
		//m_up = XMVector3Transform(m_up, translate);
		m_at = XMVector3Transform(m_at, translate);
		cameraNeedsUpdate = true;
	}
	break;

	case 'D':
	{
		XMVECTOR tempAt = XMVector3Normalize(m_at - m_eye);
		XMVECTOR tempRight = -0.1f * XMVector3Normalize(XMVector3Cross(tempAt, m_up));
		XMMATRIX translate = XMMatrixTranslation(XMVectorGetX(tempRight), XMVectorGetY(tempRight), XMVectorGetZ(tempRight));
		m_eye = XMVector3Transform(m_eye, translate);
		//m_up = XMVector3Transform(m_up, translate);
		m_at = XMVector3Transform(m_at, translate);
		cameraNeedsUpdate = true;
	}
	break;

    case '%': // Left arrow
    {
        //XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(3));
		XMMATRIX rotate = XMMatrixRotationAxis(m_up, XMConvertToRadians(3));

        /*glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), deg, up);
        ref = ref - eye;
        ref = glm::vec3(rotation * glm::vec4(ref, 1));
        ref = ref + eye;
        RecomputeAttributes();*/

        XMVECTOR tempAt = m_at - m_eye;
        tempAt = XMVector3Transform(tempAt, rotate);
        m_at = tempAt + m_eye;
		m_up = XMVector3Transform(m_up, rotate);

        //UpdateCameraMatrices();
		cameraNeedsUpdate = true;
    }
    break;

    case '\'': // Right arrow
    {
        //XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(-3));
		XMMATRIX rotate = XMMatrixRotationAxis(m_up, XMConvertToRadians(-3));

        /*glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), deg, up);
        ref = ref - eye;
        ref = glm::vec3(rotation * glm::vec4(ref, 1));
        ref = ref + eye;
        RecomputeAttributes();*/

        XMVECTOR tempAt = m_at - m_eye;
		tempAt = XMVector3Transform(tempAt, rotate);
        m_at = tempAt + m_eye;
		m_up = XMVector3Transform(m_up, rotate);

        //UpdateCameraMatrices();
		cameraNeedsUpdate = true;
    }
    break;

	case '&': // Up arrow
	{
		//XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(-3));
		XMVECTOR tempAt = XMVector3Normalize(m_at - m_eye);
		XMVECTOR tempRight = XMVector3Normalize(XMVector3Cross(tempAt, m_up));
		XMMATRIX rotate = XMMatrixRotationAxis(tempRight, XMConvertToRadians(-3));

		/*glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), deg, up);
		ref = ref - eye;
		ref = glm::vec3(rotation * glm::vec4(ref, 1));
		ref = ref + eye;
		RecomputeAttributes();*/

		tempAt = m_at - m_eye;
		tempAt = XMVector3Transform(tempAt, rotate);
		m_at = tempAt + m_eye;
		m_up = XMVector3Transform(m_up, rotate);

		//UpdateCameraMatrices();
		cameraNeedsUpdate = true;
	}
	break;

	case '(': // Down arrow
	{
		//XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(-3));
		XMVECTOR tempAt = XMVector3Normalize(m_at - m_eye);
		XMVECTOR tempRight = XMVector3Normalize(XMVector3Cross(tempAt, m_up));
		XMMATRIX rotate = XMMatrixRotationAxis(tempRight, XMConvertToRadians(3));

		/*glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), deg, up);
		ref = ref - eye;
		ref = glm::vec3(rotation * glm::vec4(ref, 1));
		ref = ref + eye;
		RecomputeAttributes();*/

		tempAt = m_at - m_eye;
		tempAt = XMVector3Transform(tempAt, rotate);
		m_at = tempAt + m_eye;
		m_up = XMVector3Transform(m_up, rotate);

		//UpdateCameraMatrices();
		cameraNeedsUpdate = true;
	}
	break;

	case 'M': // Rotate about look?
	{
		//XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(-3));
		XMVECTOR tempAt = XMVector3Normalize(m_at - m_eye);
		XMMATRIX rotate = XMMatrixRotationAxis(tempAt, XMConvertToRadians(3));

		/*glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), deg, up);
		ref = ref - eye;
		ref = glm::vec3(rotation * glm::vec4(ref, 1));
		ref = ref + eye;
		RecomputeAttributes();*/

		m_up = XMVector3Normalize(XMVector3Transform(m_up, rotate));

		//UpdateCameraMatrices();
		cameraNeedsUpdate = true;
	}
	break;

	case 'N': // Rotate about look?
	{
		//XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(-3));
		XMVECTOR tempAt = XMVector3Normalize(m_at - m_eye);
		XMMATRIX rotate = XMMatrixRotationAxis(tempAt, XMConvertToRadians(-3));

		/*glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), deg, up);
		ref = ref - eye;
		ref = glm::vec3(rotation * glm::vec4(ref, 1));
		ref = ref + eye;
		RecomputeAttributes();*/

		m_up = XMVector3Normalize(XMVector3Transform(m_up, rotate));

		//UpdateCameraMatrices();
		cameraNeedsUpdate = true;
	}
	break;

    default:
        break;
    }

    if (m_raytracingAPI != previousRaytracingAPI ||
        m_forceComputeFallback != previousForceComputeFallback)
    {
        // Raytracing API selection changed, recreate everything.
        RecreateD3D();
    }
}

// Update frame-based values.
void PixelMajorRenderer::OnUpdate()
{
    m_timer.Tick();
    CalculateFrameStats();
    float elapsedTime = static_cast<float>(m_timer.GetElapsedSeconds());
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();
    auto prevFrameIndex = m_deviceResources->GetPreviousFrameIndex();

	if (cameraNeedsUpdate)
	{
		UpdateCameraMatrices();
	}

    /*
    // Rotate the camera around Y axis.
    {
        float secondsToRotateAround = 24.0f;
        float angleToRotateBy = 360.0f * (elapsedTime / secondsToRotateAround);
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(angleToRotateBy));
        m_eye = XMVector3Transform(m_eye, rotate);
        m_up = XMVector3Transform(m_up, rotate);
        m_at = XMVector3Transform(m_at, rotate);
        UpdateCameraMatrices();
    }

    // Rotate the second light around Y axis.
    {
        float secondsToRotateAround = 8.0f;
        float angleToRotateBy = 0.f;//-360.0f * (elapsedTime / secondsToRotateAround);
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(angleToRotateBy));
        const XMVECTOR& prevLightPosition = m_sceneCB[prevFrameIndex].lightPosition;
        m_sceneCB[frameIndex].lightPosition = XMVector3Transform(prevLightPosition, rotate);
    }
    */
}


// Parse supplied command line args.
void PixelMajorRenderer::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
    PhotonBaseRenderer::ParseCommandLineArgs(argv, argc);

    if (argc > 1)
    {
        if (_wcsnicmp(argv[1], L"-FL", wcslen(argv[1])) == 0)
        {
            m_forceComputeFallback = true;
            m_raytracingAPI = RaytracingAPI::FallbackLayer;
        }
        else if (_wcsnicmp(argv[1], L"-DXR", wcslen(argv[1])) == 0)
        {
            m_raytracingAPI = RaytracingAPI::DirectXRaytracing;
        }
    }
}

void PixelMajorRenderer::DoFirstPassPhotonMapping()
{
    auto commandList = m_deviceResources->GetCommandList();
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    auto DispatchRays = [&](auto* commandList, auto* stateObject, auto* dispatchDesc)
    {
        // Since each shader table has only one shader record, the stride is same as the size.
        dispatchDesc->HitGroupTable.StartAddress = m_firstPassShaderTableRes.m_hitGroupShaderTable->GetGPUVirtualAddress();
        dispatchDesc->HitGroupTable.SizeInBytes = m_firstPassShaderTableRes.m_hitGroupShaderTable->GetDesc().Width;
        dispatchDesc->HitGroupTable.StrideInBytes = dispatchDesc->HitGroupTable.SizeInBytes;
        dispatchDesc->MissShaderTable.StartAddress = m_firstPassShaderTableRes.m_missShaderTable->GetGPUVirtualAddress();
        dispatchDesc->MissShaderTable.SizeInBytes = m_firstPassShaderTableRes.m_missShaderTable->GetDesc().Width;
        dispatchDesc->MissShaderTable.StrideInBytes = dispatchDesc->MissShaderTable.SizeInBytes;
        dispatchDesc->RayGenerationShaderRecord.StartAddress = m_firstPassShaderTableRes.m_rayGenShaderTable->GetGPUVirtualAddress();
        dispatchDesc->RayGenerationShaderRecord.SizeInBytes = m_firstPassShaderTableRes.m_rayGenShaderTable->GetDesc().Width;
        dispatchDesc->Width = m_gBufferWidth;
        dispatchDesc->Height = m_gBufferHeight;
        dispatchDesc->Depth = 1;
        commandList->SetPipelineState1(stateObject);
        commandList->DispatchRays(dispatchDesc);
    };

    auto SetCommonPipelineState = [&](auto* descriptorSetCommandList)
    {
        descriptorSetCommandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());
        // Set index and successive vertex buffer decriptor tables
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::IndexBuffersSlot, m_geometryBuffers[0].indexBuffer.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::VertexBuffersSlot, m_geometryBuffers[0].vertexBuffer.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::GeomIndexSlot, m_sceneBufferDescriptors[0].sceneBufferDescRes.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::MaterialSlot, m_materialDescriptors[0].materialDescRes.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::LightSlot, m_lightDescriptors[0].lightDescRes.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::OutputViewSlot, m_raytracingOutputResourceUAVGpuDescriptor);
    };

    commandList->SetComputeRootSignature(m_firstPassGlobalRootSignature.Get());

    // Copy the updated scene constant buffer to GPU.
    memcpy(&m_mappedConstantData[frameIndex].constants, &m_sceneCB[frameIndex], sizeof(m_sceneCB[frameIndex]));
    auto cbGpuAddress = m_perFrameConstants->GetGPUVirtualAddress() + frameIndex * sizeof(m_mappedConstantData[0]);
    commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParamsWithPrimitives::SceneConstantSlot, cbGpuAddress);

    // Bind the heaps, acceleration structure and dispatch rays.
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        SetCommonPipelineState(m_fallbackCommandList.Get());
        m_fallbackCommandList->SetTopLevelAccelerationStructure(GlobalRootSignatureParamsWithPrimitives::AccelerationStructureSlot, m_fallBackPrimitiveTLAS);
        DispatchRays(m_fallbackCommandList.Get(), m_fallbackFirstPassStateObject.Get(), &dispatchDesc);
    }
    else // DirectX Raytracing
    {
        SetCommonPipelineState(commandList);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParamsWithPrimitives::AccelerationStructureSlot, m_topLevelAccelerationStructure->GetGPUVirtualAddress());
        DispatchRays(m_dxrCommandList.Get(), m_dxrFirstPassStateObject.Get(), &dispatchDesc);
    }
}

void PixelMajorRenderer::DoSecondPassPhotonMapping()
{
    auto commandList = m_deviceResources->GetCommandList();
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    auto DispatchRays = [&](auto* commandList, auto* stateObject, auto* dispatchDesc)
    {
        // Since each shader table has only one shader record, the stride is same as the size.
        dispatchDesc->HitGroupTable.StartAddress = m_secondPassShaderTableRes.m_hitGroupShaderTable->GetGPUVirtualAddress();
        dispatchDesc->HitGroupTable.SizeInBytes = m_secondPassShaderTableRes.m_hitGroupShaderTable->GetDesc().Width;
        dispatchDesc->HitGroupTable.StrideInBytes = dispatchDesc->HitGroupTable.SizeInBytes;
        dispatchDesc->MissShaderTable.StartAddress = m_secondPassShaderTableRes.m_missShaderTable->GetGPUVirtualAddress();
        dispatchDesc->MissShaderTable.SizeInBytes = m_secondPassShaderTableRes.m_missShaderTable->GetDesc().Width;
        dispatchDesc->MissShaderTable.StrideInBytes = dispatchDesc->MissShaderTable.SizeInBytes;
        dispatchDesc->RayGenerationShaderRecord.StartAddress = m_secondPassShaderTableRes.m_rayGenShaderTable->GetGPUVirtualAddress();
        dispatchDesc->RayGenerationShaderRecord.SizeInBytes = m_secondPassShaderTableRes.m_rayGenShaderTable->GetDesc().Width;
        dispatchDesc->Width = m_width;
        dispatchDesc->Height = m_height;
        dispatchDesc->Depth = 1;
        commandList->SetPipelineState1(stateObject);
        commandList->DispatchRays(dispatchDesc);
    };

    auto SetCommonPipelineState = [&](auto* descriptorSetCommandList)
    {
        descriptorSetCommandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());
        // Set index and successive vertex buffer decriptor tables
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::IndexBuffersSlot, m_geometryBuffers[0].indexBuffer.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::VertexBuffersSlot, m_geometryBuffers[0].vertexBuffer.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::GeomIndexSlot, m_sceneBufferDescriptors[0].sceneBufferDescRes.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::MaterialSlot, m_materialDescriptors[0].materialDescRes.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::LightSlot, m_lightDescriptors[0].lightDescRes.gpuDescriptorHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParamsWithPrimitives::OutputViewSlot, m_raytracingOutputResourceUAVGpuDescriptor);
    };

    commandList->SetComputeRootSignature(m_secondPassGlobalRootSignature.Get());

    // Copy the updated scene constant buffer to GPU.
    memcpy(&m_mappedConstantData[frameIndex].constants, &m_sceneCB[frameIndex], sizeof(m_sceneCB[frameIndex]));
    auto cbGpuAddress = m_perFrameConstants->GetGPUVirtualAddress() + frameIndex * sizeof(m_mappedConstantData[0]);
    commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParamsWithPrimitives::SceneConstantSlot, cbGpuAddress);

    // Bind the heaps, acceleration structure and dispatch rays.
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        SetCommonPipelineState(m_fallbackCommandList.Get());
        m_fallbackCommandList->SetTopLevelAccelerationStructure(GlobalRootSignatureParamsWithPrimitives::AccelerationStructureSlot, m_fallBackPrimitiveTLAS);
        DispatchRays(m_fallbackCommandList.Get(), m_fallbackSecondPassStateObject.Get(), &dispatchDesc);
    }
    else // DirectX Raytracing
    {
        SetCommonPipelineState(commandList);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParamsWithPrimitives::AccelerationStructureSlot, m_topLevelAccelerationStructure->GetGPUVirtualAddress());
        DispatchRays(m_dxrCommandList.Get(), m_dxrSecondPassStateObject.Get(), &dispatchDesc);
    }
}

void PixelMajorRenderer::DoComputePass(ComPtr<ID3D12PipelineState>& computePSO, UINT xThreads, UINT yThreads, UINT zThreads)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    commandList->SetPipelineState(computePSO.Get());
    commandList->SetComputeRootSignature(m_computeRootSignature.Get());

    commandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());
    commandList->SetComputeRootDescriptorTable(ComputeRootSignatureParams::OutputViewSlot, m_raytracingOutputResourceUAVGpuDescriptor);

    // Copy the updated scene constant buffer to GPU.
    memcpy(&m_mappedComputeConstantData[0].constants, &m_computeConstantBuffer, sizeof(m_computeConstantBuffer));
    auto cbGpuAddress = m_computeConstantRes->GetGPUVirtualAddress();
    commandList->SetComputeRootConstantBufferView(ComputeRootSignatureParams::ParamConstantBuffer, cbGpuAddress);

    commandList->Dispatch(xThreads, yThreads, zThreads);
}

// Update the application state with the new resolution.
void PixelMajorRenderer::UpdateForSizeChange(UINT width, UINT height)
{
    PhotonBaseRenderer::UpdateForSizeChange(width, height);
}

// Copy the raytracing output to the backbuffer.
void PixelMajorRenderer::CopyRaytracingOutputToBackbuffer()
{
    auto commandList = m_deviceResources->GetCommandList();
    auto renderTarget = m_deviceResources->GetRenderTarget();

    D3D12_RESOURCE_BARRIER preCopyBarriers[2];
    preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

    commandList->CopyResource(renderTarget, m_raytracingOutput.Get());

    D3D12_RESOURCE_BARRIER postCopyBarriers[2];
    postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);
}

// Copy data from one UAV to another.
void PixelMajorRenderer::CopyUAVData(GBuffer& source, GBuffer& destination)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto renderTarget = m_deviceResources->GetRenderTarget();

    D3D12_RESOURCE_BARRIER preCopyBarriers[2];
    preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(destination.textureResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(source.textureResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

    commandList->CopyResource(destination.textureResource.Get(), source.textureResource.Get());

    D3D12_RESOURCE_BARRIER postCopyBarriers[2];
    postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(destination.textureResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(source.textureResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);
}

void PixelMajorRenderer::CopyGBUfferToBackBuffer(UINT gbufferIndex)
{
    if (m_gBuffers.size() > gbufferIndex)
    {
        auto commandList = m_deviceResources->GetCommandList();
        auto renderTarget = m_deviceResources->GetRenderTarget();

        // Change State to Copy from Source
        D3D12_RESOURCE_BARRIER preCopyBarriers[2];
        preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[gbufferIndex].textureResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

        // Perform the Copy
        commandList->CopyResource(renderTarget, m_gBuffers[gbufferIndex].textureResource.Get());

        // Change State back to Unordered access view
        D3D12_RESOURCE_BARRIER postCopyBarriers[2];
        postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[gbufferIndex].textureResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);
    }
}

// Create resources that are dependent on the size of the main window.
void PixelMajorRenderer::CreateWindowSizeDependentResources()
{
    CreateRaytracingOutputResource();
    CreatePhotonCountBuffer(m_photonCountBuffer);
    CreatePhotonCountBuffer(m_photonScanBuffer);
    CreatePhotonCountBuffer(m_photonTempIndexBuffer);
    CreateGBuffers(); // TODO is this right?
    UpdateCameraMatrices();
}

// Release resources that are dependent on the size of the main window.
void PixelMajorRenderer::ReleaseWindowSizeDependentResources()
{
    m_raytracingOutput.Reset();

    for (GBuffer gBuffer : m_gBuffers)
    {
        gBuffer.textureResource.Reset();
    }
}

// Release all resources that depend on the device.
void PixelMajorRenderer::ReleaseDeviceDependentResources()
{
    m_fallbackDevice.Reset();
    m_fallbackCommandList.Reset();
    m_fallbackFirstPassStateObject.Reset();
    m_firstPassGlobalRootSignature.Reset();
    m_firstPassLocalRootSignature.Reset();

    m_computeRootSignature.Reset();

    m_dxrDevice.Reset();
    m_dxrCommandList.Reset();
    m_dxrFirstPassStateObject.Reset();

    m_computeFirstPassPSO.Reset();
    m_computeSecondPassPSO.Reset();
    m_computeThirdPassPSO.Reset();

    m_descriptorHeap.Reset();
    m_descriptorsAllocated = 0;
    m_raytracingOutputResourceUAVDescriptorHeapIndex = UINT_MAX;

    for (GBuffer gBuffer : m_gBuffers)
    {
        gBuffer.uavDescriptorHeapIndex = UINT_MAX;
    }

    m_photonCountBuffer.uavDescriptorHeapIndex = UINT_MAX;
    m_photonScanBuffer.uavDescriptorHeapIndex = UINT_MAX;
    m_photonTempIndexBuffer.uavDescriptorHeapIndex = UINT_MAX;

    m_perFrameConstants.Reset();

    m_computeConstantRes.Reset();

    m_firstPassShaderTableRes.m_rayGenShaderTable.Reset();
    m_firstPassShaderTableRes.m_missShaderTable.Reset();
    m_firstPassShaderTableRes.m_hitGroupShaderTable.Reset();

    m_secondPassShaderTableRes.m_rayGenShaderTable.Reset();
    m_secondPassShaderTableRes.m_missShaderTable.Reset();
    m_secondPassShaderTableRes.m_hitGroupShaderTable.Reset();

    m_topLevelAccelerationStructure.Reset();

    m_fence.Reset();

}

void PixelMajorRenderer::RecreateD3D()
{
    // Give GPU a chance to finish its execution in progress.
    try
    {
        m_deviceResources->WaitForGpu();
    }
    catch (HrException&)
    {
        // Do nothing, currently attached adapter is unresponsive.
    }
    m_deviceResources->HandleDeviceLost();
}

// Render the scene.
void PixelMajorRenderer::OnRender()
{
    if (!m_deviceResources->IsWindowVisible())
    {
        return;
    }

    m_deviceResources->Prepare();

    auto commandList = m_deviceResources->GetCommandList();
    auto commandAllocator = m_deviceResources->GetCommandAllocator();
    auto commandQueue = m_deviceResources->GetCommandQueue();


    if (m_calculatePhotonMap)
    {
        // Clear the photon Count - Find a better way to do this, instead of launching a new compute shader
        DoComputePass(m_computeInitializePSO, NUM_CELLS_IN_X, NUM_CELLS_IN_Y, NUM_CELLS_IN_Z);

        m_deviceResources->ExecuteCommandList();
        m_deviceResources->WaitForGpu();
        commandList->Reset(commandAllocator, nullptr);

        DoComputePass(m_computeInitializePSO2, m_gBufferWidth, m_gBufferHeight, m_gBufferDepth);

        m_deviceResources->ExecuteCommandList();
        m_deviceResources->WaitForGpu();
        commandList->Reset(commandAllocator, nullptr);

        // Performs:
        // 1. Photon Generation
        // 2. Photon Traversal
        // 3. Calculates the number of photons in each cell.
        DoFirstPassPhotonMapping();

        m_calculatePhotonMap = false;

        // Try to keep this number to be a power of 2.
        const int numItems = NUM_CELLS_IN_X * NUM_CELLS_IN_Y * NUM_CELLS_IN_Z;
        const int log_n = ilog2ceil(numItems);

        // Make a copy of the count data into the scan buffer
        CopyUAVData(m_photonCountBuffer, m_photonScanBuffer);

        // Exclusive scan up-sweep
        int power_2 = 1;
        for (int d = 0; d < log_n; ++d)
        {
            power_2 = (1 << d);
            m_computeConstantBuffer.param1 = power_2;
            m_computeConstantBuffer.param2 = numItems;

            DoComputePass(m_computeFirstPassPSO, numItems, 1, 1);

            m_deviceResources->ExecuteCommandList();
            ScanWaitForGPU(commandQueue);
            //m_deviceResources->WaitForGpu();
            commandList->Reset(commandAllocator, nullptr);
        }
        
        // 3b. DownSweep
        for (int d = log_n - 1; d >= 0; --d)
        {
            power_2 = (1 << d);
            m_computeConstantBuffer.param1 = power_2;
            m_computeConstantBuffer.param2 = numItems;

            DoComputePass(m_computeSecondPassPSO, numItems, 1, 1);

            m_deviceResources->ExecuteCommandList();
            ScanWaitForGPU(commandQueue);
            commandList->Reset(commandAllocator, nullptr);
        }

        // Copy the count data to a dynamic index 
        CopyUAVData(m_photonScanBuffer, m_photonTempIndexBuffer);

        m_deviceResources->ExecuteCommandList();
        m_deviceResources->WaitForGpu();
        commandList->Reset(commandAllocator, nullptr);

        // Sort the photons
        m_computeConstantBuffer.param1 = m_gBufferWidth;
        m_computeConstantBuffer.param2 = m_gBufferDepth;
        DoComputePass(m_computeThirdPassPSO, m_gBufferWidth, m_gBufferHeight, m_gBufferDepth); //TODO change width..

        m_deviceResources->ExecuteCommandList();
        m_deviceResources->WaitForGpu();
        commandList->Reset(commandAllocator, nullptr);

    }

    DoSecondPassPhotonMapping();
    CopyRaytracingOutputToBackbuffer();
    //CopyGBUfferToBackBuffer(0U);

    m_deviceResources->Present(D3D12_RESOURCE_STATE_PRESENT);
}

void PixelMajorRenderer::OnDestroy()
{
    // Let GPU finish before releasing D3D resources.
    m_deviceResources->WaitForGpu();
    OnDeviceLost();
}

// Release all device dependent resouces when a device is lost.
void PixelMajorRenderer::OnDeviceLost()
{
    ReleaseWindowSizeDependentResources();
    ReleaseDeviceDependentResources();
}

// Create all device dependent resources when a device is restored.
void PixelMajorRenderer::OnDeviceRestored()
{
    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();
}

// Compute the average frames per second and million rays per second.
void PixelMajorRenderer::CalculateFrameStats()
{
    static int frameCnt = 0;
    static double elapsedTime = 0.0f;
    double totalTime = m_timer.GetTotalSeconds();
    frameCnt++;

    // Compute averages over one second period.
    if ((totalTime - elapsedTime) >= 1.0f)
    {
        float diff = static_cast<float>(totalTime - elapsedTime);
        float fps = static_cast<float>(frameCnt) / diff; // Normalize to an exact second.

        frameCnt = 0;
        elapsedTime = totalTime;

        float MRaysPerSecond = (m_width * m_height * fps) / static_cast<float>(1e6);

        wstringstream windowText;

        if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
        {
            if (m_fallbackDevice->UsingRaytracingDriver())
            {
                windowText << L"(FL-DXR)";
            }
            else
            {
                windowText << L"(FL)";
            }
        }
        else
        {
            windowText << L"(DXR)";
        }
        windowText << setprecision(2) << fixed
            << L"    fps: " << fps << L"     ~Million Primary Rays/s: " << MRaysPerSecond
            << L"    GPU[" << m_deviceResources->GetAdapterID() << L"]: " << m_deviceResources->GetAdapterDescription()
            << L"    # Photons " << NumPhotons;
        SetCustomWindowText(windowText.str().c_str());
    }
}

// Handle OnSizeChanged message event.
void PixelMajorRenderer::OnSizeChanged(UINT width, UINT height, bool minimized)
{
    if (!m_deviceResources->WindowSizeChanged(width, height, minimized))
    {
        return;
    }

    UpdateForSizeChange(width, height);

    ReleaseWindowSizeDependentResources();
    CreateWindowSizeDependentResources();
}

void PixelMajorRenderer::ScanWaitForGPU(Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueueNo)
{
    auto commandQueue = m_deviceResources->GetCommandQueue();

    // Schedule a Signal command in the GPU queue.
    if (commandQueue && m_fence && m_fenceEvent.IsValid())
    {
        UINT64 fenceValue = m_fenceValue;
        m_fenceValue++;
        if (SUCCEEDED(commandQueue->Signal(m_fence.Get(), fenceValue)))
        {
            // Wait until the Signal has been processed.
            if (SUCCEEDED(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent.Get())))
            {
                WaitForSingleObjectEx(m_fenceEvent.Get(), INFINITE, FALSE);

                // Increment the fence value for the current frame.
            }
            //m_fenceValue++;
        }
    }
}
