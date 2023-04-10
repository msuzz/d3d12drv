/**
\class D3D

Main Direct3D functionality; self-contained, does not call external functions apart from the debug output one.
Does not use Unreal data apart from a couple of PolyFlags. Does not require the renderer interface to deal with Direct3D structures.
Quite a lot of code, but splitting this up does not really seem worth it.

An effort is made to reduce the amount of needed draw() calls. As such, state is only changed when absolutely necessary.

TODO:
Take out D3Dx?
Different shaders/input layouts for different drawXXXX renderer calls for smaller vertex size?

*/
#ifdef _DEBUG
#define _DEBUGDX //debug device
#endif

#include <dxgi1_4.h> // required for IDXGIFactory4
#include <d3d12.h>
#include <DirectXMath.h> // xnamath.h -> DirectXMath.h
#include <DirectXPackedVector.h>
#include <D3dcompiler.h> // D3DX11async.h -> D3dcompiler.h
#include <wrl.h>
#include <hash_map>
#include "include/directx/d3dx12.h"
#include "d3d12drv.h"
#include "polyflags.h" //for polyflags
#include "d3d.h"

// Link necessary d3d12 libraries
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

/**
D3D Objects
*/
static struct
{
	// TODO: Which of these can be local instead?
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIOutput> output;
	ComPtr<ID3D12Device3> device;
	ComPtr<ID3D12Fence> fence;
	ComPtr<IDXGISwapChain> swapChain;
	ComPtr<ID3D12CommandQueue> cmdQueue;
	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	ComPtr<ID3D12RootSignature> rootSig;
	ComPtr<ID3D12PipelineState> pipelineState;
	ComPtr<ID3D12Resource> renderTargetView;
	ComPtr<ID3D12Resource> depthStencilView;
	ID3D11InputLayout* vertexLayout;
	ComPtr<ID3D12Resource> vertexBuf = nullptr;
	ComPtr<ID3D12Resource> vertexBufUploader = nullptr;
	ComPtr<ID3D12Resource> indexBuf;
	ID3DX11Effect* effect;
	ComPtr<ID3D12DescriptorHeap> rtvHeap;
	ComPtr<ID3D12DescriptorHeap> dsvHeap;
} D3DObjects;

/**
Shader side variables
*/
static struct
{
	ID3DX11EffectMatrixVariable* projection; /**< projection matrix */
	ID3DX11EffectScalarVariable* projectionMode; /**< Projection transform mode (near/far) */
	ID3DX11EffectScalarVariable* useTexturePass; /**< Bool whether to use each texture pass (shader side) */
	ID3DX11EffectShaderResourceVariable* shaderTextures; /**< GPU side currently bound textures */
	ID3DX11EffectVectorVariable* flashColor; /**< Flash color */
	ID3DX11EffectScalarVariable* flashEnable; /**< Flash enabled? */
	ID3DX11EffectScalarVariable* time; /**< Time for sin() etc */
	ID3DX11EffectScalarVariable* viewportHeight; /**< Viewport height in pixels */
	ID3DX11EffectScalarVariable* viewportWidth; /**< Viewport width in pixels */
	ID3DX11EffectScalarVariable* brightness; /**< Brightness 0-1 */
	ID3DX11EffectVectorVariable* fogColor; /**< Fog color */
	ID3DX11EffectScalarVariable* fogDist; /**< Fog end distance? */
}  shaderVars;

/**
States (defined in fx file)
*/
static struct
{
	// TODO: Deal with these
	ID3D11DepthStencilState* dstate_Enable;
	ID3D11DepthStencilState* dstate_Disable;
	ID3D11BlendState* bstate_Alpha;
	ID3D11BlendState* bstate_Translucent;
	ID3D11BlendState* bstate_Modulate;
	ID3D11BlendState* bstate_NoBlend;
	ID3D11BlendState* bstate_Masked;
	ID3D11BlendState* bstate_Invis;
} states;

/**
Texture cache variables
*/
static struct
{
	DWORD64 boundTextureID[D3D::DUMMY_NUM_PASSES]; /**< CPU side bound texture IDs for the various passes as defined in the shader */
	BOOL enabled[D3D::DUMMY_NUM_PASSES]; /**< Bool whether to use each texture pass (CPU side, used to set shaderVars.useTexturePass) */
} texturePasses;

/*
The texture cache
*/
stdext::hash_map <unsigned __int64,D3D::CachedTexture> textureCache;

/*
Triangle fans are drawn indexed. Their vertices and draw indexes are stored in mapped buffers.
At the start of a frame or when the buffer is full, it gets emptied. Otherwise, the buffer is reused over multiple draw() calls.
*/
const unsigned int I_BUFFER_SIZE = 20000; //20000 measured to be about max
const unsigned int V_BUFFER_SIZE = I_BUFFER_SIZE; //In worst case, one point for each index
static unsigned int numVerts; //Number of buffered verts
static unsigned int numIndices; //Number of buffered indices
static unsigned int numUndrawnIndices; //Number of buffered indices not yet drawn
//static D3D11_MAPPED_SUBRESOURCE mappedVBuffer; //Memmapped version of vertex buffer
//static D3D11_MAPPED_SUBRESOURCE mappedIBuffer; //Memmapped version of index buffer

/*
Misc
*/
static const DXGI_FORMAT BACKBUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;
static const D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_12_0;
static const float TIME_STEP = (1/60.0f); //Shader time variable increase speed
static D3D::Options options;
int scdBufCount = 1;
int currBackBuf = 0;
UINT rtvDescriptorSize = 0;
UINT dsvDescriptorSize = 0;
UINT cbvSrvDescriptorSize = 0;
UINT64 currentFence = 0;

/**
Create Direct3D device, swapchain, etc. Purely boilerplate stuff.
Initialisation order is based on the book "Introduction to 3D Game Programming with DirectX 12" by Frank D. Luna.

\param hWnd Window to use as a surface.
\param createOptions the D3D::Options which to use.
\param zNear Near Z value.
*/
int D3D::init(HWND hWnd,D3D::Options &createOptions)
{
	HRESULT hr;

	options = createOptions; // Set config options
	CLAMP(options.samples,1,D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT);
	CLAMP(options.aniso,0,16);
	CLAMP(options.VSync,0,1);
	CLAMP(options.LODBias,-10,10);
	UD3D12RenderDevice::debugs("Initializing Direct3D 12.");

	// Enable the debug layer for debug builds
	#ifdef _DEBUGDX
	{
		//flags = D3D11_CREATE_DEVICE_DEBUG; //debug runtime (prints debug messages)
		ComPtr<ID3D12Debug> debugController;
		hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
		if (FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to enable debug layer.");
			return 0;
		}
		debugController->EnableDebugLayer();
	}
	#endif

	// Create the factory - needed for software (WARP) adapter and swapchain
	hr = CreateDXGIFactory1(IID_PPV_ARGS(&D3DObjects.factory));
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating factory.");
		return 0;
	}

	// Create the Direct3D 12 device
	hr = D3D12CreateDevice(
		NULL,
		minFeatureLevel,
		IID_PPV_ARGS(&D3DObjects.device)
	);
	if (FAILED(hr))
	{
		// Fall back to WARP if hardware device creation fails
		UD3D12RenderDevice::debugs("Error creating hardware device. Falling back to WARP (software) adapter.");

		ComPtr<IDXGIAdapter> pWarpAdapter;
		hr = D3DObjects.factory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter));
		if (FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to enumerate WARP adapter.");
		return 0;
	}

		hr = D3D12CreateDevice(
			pWarpAdapter.Get(),
			minFeatureLevel,
			IID_PPV_ARGS(&D3DObjects.device)
		);
		if (FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to create WARP device.");
			return 0;
		}
	}

	// Create fence for GPU/CPU synchronisation
	hr = D3DObjects.device->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&D3DObjects.fence)
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating fence.");
		return 0;
	}

	// Cache descriptor sizes
	rtvDescriptorSize = D3DObjects.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	dsvDescriptorSize = D3DObjects.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	cbvSrvDescriptorSize = D3DObjects.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Check MSAA support and clamp to max level.
	if(!D3D::findAALevel()) 
		return 0;
	SAFE_RELEASE(pDXGIDevice);
	SAFE_RELEASE(pDXGIAdapter);
	SAFE_RELEASE(pIDXGIFactory);

	// Create command queue
	D3D12_COMMAND_QUEUE_DESC cqd;
	cqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	cqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	hr = D3DObjects.device->CreateCommandQueue(
		&cqd,
		IID_PPV_ARGS(&D3DObjects.cmdQueue)
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating command queue.");
		return 0;
	}

	// Create command allocator
	hr = D3DObjects.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(D3DObjects.cmdAlloc.GetAddressOf())
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating command allocator.");
		return 0;
	}

	// Create command list
	hr = D3DObjects.device->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		D3DObjects.cmdAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(D3DObjects.cmdList.GetAddressOf())
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating command list.");
		return 0;
	}

	D3DObjects.cmdList->Close(); // Command list must be closed before resetting

	// Describe and create swap chain
	DXGI_SWAP_CHAIN_DESC scd;
	ZeroMemory(&scd, sizeof(scd));
	scd.BufferCount = scdBufCount;
	scd.BufferDesc.Width = Window::getWidth();
	scd.BufferDesc.Height = Window::getHeight();
	scd.BufferDesc.Format = BACKBUFFER_FORMAT;
	scd.BufferDesc.RefreshRate.Numerator = 60;
	scd.BufferDesc.RefreshRate.Denominator = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = hWnd;
	scd.SampleDesc.Count = options.samples;
	scd.SampleDesc.Quality = 0;
	scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; // If unspecified, will use desktop display mode in fullscreen
	scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scd.Windowed = TRUE;

	hr = D3DObjects.factory->CreateSwapChain(
		D3DObjects.cmdQueue.Get(),
		&scd,
		&D3DObjects.swapChain
	);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating swap chain.");
		return 0;
	}
	D3DObjects.factory->MakeWindowAssociation(hWnd,DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_PRINT_SCREEN |DXGI_MWA_NO_ALT_ENTER); //Stop DXGI from interfering with the game
	D3DObjects.swapChain->GetContainingOutput(&D3DObjects.output);
		
	// Describe and create the RTV/DSV descriptor heaps
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = scdBufCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;

	// Create the RTV descriptor heap
	hr = D3DObjects.device->CreateDescriptorHeap(
		&rtvHeapDesc,
		IID_PPV_ARGS(D3DObjects.rtvHeap.GetAddressOf())
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating RTV descriptor heap.");
		return 0;
	}

	// Describe the DSV descriptor heap
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;

	// Create the DSV descriptor heap
	hr = D3DObjects.device->CreateDescriptorHeap(
		&dsvHeapDesc,
		IID_PPV_ARGS(D3DObjects.dsvHeap.GetAddressOf())
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating DSV descriptor heap.");
		return 0;
	}
		
	//Create the effect we'll be using
	ComPtr<ID3DBlob> shadBlob = nullptr;
	ComPtr<ID3DBlob> shadErrBlob;
	DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
	
	//Set shader macro options
	#define OPTION_TO_STRING(x) char buf##x[10];_itoa_s(options.##x,buf##x,10,10);
	#define OPTIONSTRING_TO_SHADERVAR(x,y) {y,buf##x}
	OPTION_TO_STRING(aniso);
	OPTION_TO_STRING(LODBias);
	OPTION_TO_STRING(zNear);
	OPTION_TO_STRING(samples);
	OPTION_TO_STRING(POM);
	OPTION_TO_STRING(alphaToCoverage);
	
	D3D_SHADER_MACRO shaderMacros[] = {
	D3D_SHADER_MACRO macros[] = {
	OPTIONSTRING_TO_SHADERVAR(aniso,"NUM_ANISO"),
	OPTIONSTRING_TO_SHADERVAR(LODBias,"LODBIAS"),
	OPTIONSTRING_TO_SHADERVAR(zNear,"Z_NEAR"),
	OPTIONSTRING_TO_SHADERVAR(samples,"SAMPLES"),
	OPTIONSTRING_TO_SHADERVAR(POM,"POM_ENABLED"),
	OPTIONSTRING_TO_SHADERVAR(alphaToCoverage,"ALPHA_TO_COVERAGE_ENABLED"),
	NULL};
	
	// Compile shader
	hr = D3DCompileFromFile(
		L"D3D12drv\\unreal.fx",
		shaderMacros,
		D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"NULL",
		"fx_5_1",
		dwShaderFlags,
		0,
		&shadBlob,
		&shadErrBlob
	);

	if(shadErrBlob != nullptr) //Show compile warnings/errors if present
		UD3D12RenderDevice::debugs((char*) shadErrBlob->GetBufferPointer());	

	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error compiling shader file. Please make sure unreal.fx resides in the \"\\system\\D3D12drv\" directory.");		
		return 0;
	}

	// Create effect from shader
	// msuzz: Replace with PSO
	/*
	hr = D3DX11CreateEffectFromMemory(
		effectBlob->GetBufferPointer(),
		effectBlob->GetBufferSize(),
		NULL,
		D3DObjects.device->GetFeatureLevel(),
		D3DObjects.device,
		&D3DObjects.effect
	);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating effect from shader.");		
		return 0;
	}

	SAFE_RELEASE(shadBlob);
	SAFE_RELEASE(effectBlob);
	*/

	//Get shader params
	shaderVars.projection = D3DObjects.effect->GetVariableByName("projection")->AsMatrix();
	shaderVars.projectionMode = D3DObjects.effect->GetVariableByName("projectionMode")->AsScalar(); 	
	shaderVars.flashColor = D3DObjects.effect->GetVariableByName("flashColor")->AsVector();
	shaderVars.flashEnable = D3DObjects.effect->GetVariableByName("flashEnable")->AsScalar();
	shaderVars.useTexturePass = D3DObjects.effect->GetVariableByName("useTexturePass")->AsScalar();
	shaderVars.shaderTextures = D3DObjects.effect->GetVariableByName("textures")->AsShaderResource();
	shaderVars.time = D3DObjects.effect->GetVariableByName("time")->AsScalar();
	shaderVars.viewportHeight = D3DObjects.effect->GetVariableByName("viewportHeight")->AsScalar();
	shaderVars.viewportWidth = D3DObjects.effect->GetVariableByName("viewportWidth")->AsScalar();
	shaderVars.brightness = D3DObjects.effect->GetVariableByName("brightness")->AsScalar();
	shaderVars.fogColor = D3DObjects.effect->GetVariableByName("fogColor")->AsVector();
	shaderVars.fogDist = D3DObjects.effect->GetVariableByName("fogDist")->AsScalar();

	//Get states
	D3DObjects.effect->GetVariableByName("dstate_Enable")->AsDepthStencil()->GetDepthStencilState(0,&states.dstate_Enable);
	D3DObjects.effect->GetVariableByName("dstate_Disable")->AsDepthStencil()->GetDepthStencilState(0,&states.dstate_Disable);
	D3DObjects.effect->GetVariableByName("bstate_Translucent")->AsBlend()->GetBlendState(0,&states.bstate_Translucent);
	D3DObjects.effect->GetVariableByName("bstate_Modulate")->AsBlend()->GetBlendState(0,&states.bstate_Modulate);
	D3DObjects.effect->GetVariableByName("bstate_NoBlend")->AsBlend()->GetBlendState(0,&states.bstate_NoBlend);
	D3DObjects.effect->GetVariableByName("bstate_Masked")->AsBlend()->GetBlendState(0,&states.bstate_Masked);
	D3DObjects.effect->GetVariableByName("bstate_Alpha")->AsBlend()->GetBlendState(0,&states.bstate_Alpha);
	D3DObjects.effect->GetVariableByName("bstate_Invis")->AsBlend()->GetBlendState(0,&states.bstate_Invis);
	
	//Apply shader variable options
	setBrightness(options.brightness);

	//Set the vertex layout
    D3D12_INPUT_ELEMENT_DESC elementDesc[] =
    {
		{ "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",        1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",	      0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     1, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     2, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     3, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     4, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R32_UINT,           0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

	D3D12_INPUT_LAYOUT_DESC ilDesc = {
		elementDesc,
		sizeof(elementDesc)/sizeof(elementDesc[0])
    };
    UINT numElements = sizeof(layoutDesc) / sizeof(layoutDesc[0]);

	// msuzz: I don't think we need these, replaced by pso
	/*
	D3DX11_PASS_DESC passDesc;
	ID3DX11EffectTechnique* t = D3DObjects.effect->GetTechniqueByIndex(0);
	if(!t->IsValid())
	{
		UD3D12RenderDevice::debugs("Failed to find technique 0.");
		return 0;
	}
	ID3DX11EffectPass* p = t->GetPassByIndex(0);
	if(!p->IsValid())
	{
		UD3D12RenderDevice::debugs("Failed to find pass 0.");
		return 0;
	}
	p->GetDesc(&passDesc);
	hr = D3DObjects.device->CreateInputLayout(layoutDesc, numElements, passDesc.pIAInputSignature, passDesc.IAInputSignatureSize, &D3DObjects.vertexLayout);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating input layout.");
		return 0;
	} */
	
	//Set up vertex buffer
	// Create default buffer resource
	hr = D3DObjects.device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(V_BUFFER_SIZE*sizeof(Vertex)), // may be incorrect
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(D3DObjects.vertexBuf.GetAddressOf())
	);

	// Create intermediate upload heap
	hr = D3DObjects.device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(V_BUFFER_SIZE*sizeof(Vertex)), // may be incorrect
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(D3DObjects.vertexBufUploader.GetAddressOf())
	);

	// Describe the data we want to copy into the default buffer
	D3D12_SUBRESOURCE_DATA subResourceData = {};
	subResourceData.pData = initData;
	subResourceData.RowPitch = sizeof(Vertex);
	subResourceData.SlicePitch = subResourceData.RowPitch;
	
	//Create index buffer
	D3D12_RESOURCE_DESC ibDesc;
	ibDesc.Usage = D3D11_USAGE_DYNAMIC;
	ibDesc.ByteWidth = sizeof(int)*I_BUFFER_SIZE;
	ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
	ibDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    ibDesc.MiscFlags        = 0;
	hr = D3DObjects.device->CreateCommittedResource( &ibDesc,NULL, &D3DObjects.indexBuf);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating index buffer.");
		return 0;
	}

	// Root signature descriptor
	// Create a root parameter that expects a descriptor table of 1 constant view buffer, that
	// gets bound to constant buffer register 0 in the HLSL code.
	CD3DX12_ROOT_PARAMETER slotRootParameter[1];
	CD3DX12_DESCRIPTOR_RANGE cbvTable;
	cbvTable.Init(
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
		1,	// number of desriptors in table
		0); // base shader register arguments are bound to for this root parmeter
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, slotRootParameter, 0, nullptr);

	// Serialise root signature
	ComPtr<ID3DBlob> sRootSig = nullptr;
	ComPtr<ID3DBlob> errBlob = nullptr;
	hr = D3D12SerializeRootSignature(
		&rootSigDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		sRootSig.GetAddressOf(),
		errBlob.GetAddressOf());
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error serializing root signature.");
		return 0;
	}

	// Create root signature
	hr = D3DObjects.device->CreateRootSignature(
		0,
		sRootSig->GetBufferPointer(),
		sRootSig->GetBufferSize(),
		IID_PPV_ARGS(&D3DObjects.rootSig));
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error crearing root signature.");
		return 0;
	}

	// Pipeline state object descriptor
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.InputLayout = ilDesc;
	psoDesc.pRootSignature = D3DObjects.rootSig.Get();
	psoDesc.VS = { reinterpret_cast<BYTE*>(shadBlob->GetBufferPointer()), shadBlob->GetBufferSize() };
	psoDesc.PS = { reinterpret_cast<BYTE*>(shadBlob->GetBufferPointer()), shadBlob->GetBufferSize() };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = BACKBUFFER_FORMAT;
	psoDesc.SampleDesc.Count = options.samples;
	psoDesc.SampleDesc.Quality = 0; // fix?
	psoDesc.DSVFormat = DEPTH_STENCIL_FORMAT;

	// Create the pipeline state
	hr = D3DObjects.device->CreateGraphicsPipelineState(
		&psoDesc,
		IID_PPV_ARGS(&D3DObjects.pipelineState)
	);
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating pipeline state object.");
		return 0;
	}

#ifdef _DEBUGDX
	//Disable certain debug output
	ID3D12InfoQueue * pInfoQueue;
	D3DObjects.device->QueryInterface( __uuidof(ID3D12InfoQueue),  (void **)&pInfoQueue );

	// Set up the list of messages to filter
	// msuz: find correct messages that we want here
	D3D12_MESSAGE_ID messageIDs [] = {
		/* D3D11_MESSAGE_ID_DEVICE_DRAW_SHADERRESOURCEVIEW_NOT_SET,
		D3D11_MESSAGE_ID_PSSETSHADERRESOURCES_UNBINDDELETINGOBJECT,
		D3D11_MESSAGE_ID_OMSETRENDERTARGETS_UNBINDDELETINGOBJECT,
		D3D11_MESSAGE_ID_CHECKFORMATSUPPORT_FORMAT_DEPRECATED */
		D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_VERTEX_SHADER_NOT_SET
	};


	//set the DenyList to use the list of messages
	D3D12_INFO_QUEUE_FILTER filter = { 0 };
	filter.DenyList.NumIDs = sizeof(messageIDs);
	filter.DenyList.pIDList = messageIDs;

	//apply the filter to the info queue
	pInfoQueue->AddStorageFilterEntries( &filter ); 
	SAFE_RELEASE(pInfoQueue);

#endif

	// D3D12 initialisation complete
	return 1;
}

/**
Create a render target view from the backbuffer and depth stencil buffer.
*/
int D3D::createRenderTargetViews()
{
	HRESULT hr;

	//Backbuffer
	ID3D11Texture2D* pBuffer;
	hr = D3DObjects.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBuffer);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error getting swap chain buffer.");
		return 0;
	}
	
	hr = D3DObjects.device->CreateRenderTargetView(pBuffer,NULL,&D3DObjects.renderTargetView );
	SAFE_RELEASE(pBuffer);
	hr = D3DObjects.device->CreateRenderTargetView(pBuffer, NULL, &D3DObjects.renderTargetView);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating render target view (back).");
		return 0;
	}

	DXGI_SWAP_CHAIN_DESC swapChainDesc;
	D3DObjects.swapChain->GetDesc(&swapChainDesc);
	

	//Descriptor for depth stencil view
	D3D12_RESOURCE_DESC dsvDesc;
	dsvDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	dsvDesc.Width = swapChainDesc.BufferDesc.Width;
	dsvDesc.Height = swapChainDesc.BufferDesc.Height;
	dsvDesc.MipLevels = 1;
	//descDepth.ArraySize = 1;
	dsvDesc.Format =  DEPTH_STENCIL_FORMAT;
	dsvDesc.SampleDesc.Count = options.samples;
	dsvDesc.SampleDesc.Quality = 0;
	//descDepth.Usage = D3D11_USAGE_DEFAULT;
	//descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	dsvDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	//descDepth.CPUAccessFlags = 0;
	//descDepth.MiscFlags = 0;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = DEPTH_STENCIL_FORMAT;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	
	// Create resource for depth stencil view
	hr = D3DObjects.device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&dsvDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(D3DObjects.depthStencilView.GetAddressOf()));
	if (FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Depth texture resource creation failed.");
		return 0;
	}

	// Create the actual depth stencil view
	// D3D12 CreateDepthStencilView has no return (rather than HRESULT for d3d11)
	D3DObjects.device->CreateDepthStencilView(
		D3DObjects.depthStencilView.Get(),
		nullptr,
		D3DObjects.dsvHeap->GetCPUDescriptorHandleForHeapStart());
	
	/*
	ID3D11Texture2D *depthTexInternal;
	descDepth.Width = scd.BufferDesc.Width;
	descDepth.Height = scd.BufferDesc.Height;
	descDepth.MipLevels = 1;
	descDepth.ArraySize = 1;
	descDepth.Format =  DXGI_FORMAT_D32_FLOAT;
	descDepth.SampleDesc.Count = options.samples;
	descDepth.SampleDesc.Quality = 0;
	descDepth.Usage = D3D11_USAGE_DEFAULT;
	descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	descDepth.CPUAccessFlags = 0;
	descDepth.MiscFlags = 0;
	if(FAILED(D3DObjects.device->CreateTexture2D( &descDepth, NULL,&depthTexInternal )))
	{
		UD3D12RenderDevice::debugs("Depth texture creation failed.");
		return 0;
	}
	*/

	/*
	//Depth Stencil view
	D3D11_DEPTH_STENCIL_VIEW_DESC descDSV;
	descDSV.Format = descDepth.Format;
	descDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
	descDSV.Flags = 0;
	if(FAILED(D3DObjects.device->CreateDepthStencilView(depthTexInternal,&descDSV,&D3DObjects.depthStencilView )))
	{
		UD3D12RenderDevice::debugs("Error creating render target view (depth).");
		return 0;
	}
	SAFE_RELEASE(depthTexInternal);
	*/
	
	return 1;
}


/**
Cleanup
*/
void D3D::uninit()
{
	UD3D12RenderDevice::debugs("Uninit.");
	D3D::flush();
	D3DObjects.swapChain->SetFullscreenState(FALSE,NULL); //Go windowed so swapchain can be released

	if(D3DObjects.deviceContext)
		D3DObjects.deviceContext->ClearState();
	D3DObjects.deviceContext->Flush();

	// TODO: Ensure all new D3DObjects are here
	SAFE_RELEASE(D3DObjects.vertexLayout);
	SAFE_RELEASE(D3DObjects.vertexBuffer);
	SAFE_RELEASE(D3DObjects.indexBuffer);
	SAFE_RELEASE(D3DObjects.effect);
	SAFE_RELEASE(states.dstate_Enable);
	SAFE_RELEASE(states.dstate_Disable);
	SAFE_RELEASE(states.bstate_NoBlend);
	SAFE_RELEASE(states.bstate_Translucent);
	SAFE_RELEASE(states.bstate_Modulate);
	SAFE_RELEASE(states.bstate_Alpha);
	SAFE_RELEASE(D3DObjects.renderTargetView);
	SAFE_RELEASE(D3DObjects.depthStencilView);
	SAFE_RELEASE(D3DObjects.swapChain);
	SAFE_RELEASE(D3DObjects.device);
	SAFE_RELEASE(D3DObjects.deviceContext);
	SAFE_RELEASE(D3DObjects.output);
	SAFE_RELEASE(D3DObjects.factory);
	UD3D12RenderDevice::debugs("Bye.");
}


/**
Set resolution and windowed/fullscreen.

\note DX10 is volatile; the order in which the steps are taken is critical.
*/
int D3D::resize(int X, int Y, bool fullScreen)
{		

	#ifdef _DEBUG
	printf("%d %d %d\n",X,Y,fullScreen);
	#endif
	HRESULT hr;
	DXGI_SWAP_CHAIN_DESC sd;

	switchToPass(-1); //Switch to no pass so stuff will be rebound later.

	//Get swap chain description
	hr = D3DObjects.swapChain->GetDesc(&sd);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Failed to get swap chain description.");
		return 0;
	}
	sd.BufferDesc.Width = X; //Set these so we can use this for getclosestmatchingmode
	sd.BufferDesc.Height = Y;

	SAFE_RELEASE(D3DObjects.renderTargetView); //Release render target view
	SAFE_RELEASE(D3DObjects.depthStencilView);

	//Set fullscreen resolution
	if(fullScreen)
	{
		if(FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to get output adapter.");
			return 0;
		}
		DXGI_MODE_DESC fullscreenMode = sd.BufferDesc;
		//hr = D3DObjects.output->FindClosestMatchingMode(&sd.BufferDesc,&fullscreenMode,D3DObjects.device);
		if(FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to get matching display mode.");
			return 0;
		}
		hr = D3DObjects.swapChain->ResizeTarget(&fullscreenMode);
		if(FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to set full-screen resolution.");
			return 0;
		}
		hr = D3DObjects.swapChain->SetFullscreenState(TRUE,NULL);
		if(FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to switch to full-screen.");
			//return 0;
		}
		//MS recommends doing this
		fullscreenMode.RefreshRate.Denominator=0;
		fullscreenMode.RefreshRate.Numerator=0;
		hr = D3DObjects.swapChain->ResizeTarget(&fullscreenMode);
		if(FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Failed to set full-screen resolution.");
			return 0;
		}
		sd.BufferDesc = fullscreenMode;
	}	

	//This must be done after fullscreen stuff or blitting will be used instead of flipping
	hr = D3DObjects.swapChain->ResizeBuffers(sd.BufferCount,X,Y,sd.BufferDesc.Format,sd.Flags); //Resize backbuffer
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Failed to resize back buffer.");
		return 0;
	}
	if(!createRenderTargetViews()) //Recreate render target view
	{
		return 0;
	}
	
	//Reset viewport, it's sometimes lost.
	D3D11_VIEWPORT vp;
	vp.Width = X;
	vp.Height = Y;
	vp.MinDepth = 0.0;
	vp.MaxDepth = 1.0;
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	D3DObjects.deviceContext->RSSetViewports(1,&vp);
	return 1;
}

/**
Set up things for rendering a new frame. For example, update shader time.
*/
void D3D::newFrame()
{	
	static float time;
	shaderVars.time->SetFloat(time);
	time += TIME_STEP;
}

/**
Clear backbuffer(s)
\param clearColor The color with which the screen is cleared.
*/
void D3D::clear(D3D::Vec4& clearColor)
{
	D3DObjects.deviceContext->ClearRenderTargetView(D3DObjects.renderTargetView,(float*) &clearColor);
}

/**
Clear depth
*/
void D3D::clearDepth()
{
	commit();
	D3DObjects.deviceContext->ClearDepthStencilView(D3DObjects.depthStencilView, D3D11_CLEAR_DEPTH, 1.0, 0);
}

/**
Memory map index and vertex buffer for writing.

\param Clear Sets whether the buffer is restarted from the beginning;
This is done when the buffers are about to overflow, and at the start of a new frame (Microsoft recommendation).
*/
void D3D::map(bool clear)
{	
	HRESULT hr, hr2;
	if(mappedIBuffer.pData!=NULL||mappedVBuffer.pData!=NULL)
	{
		//UD3D12RenderDevice::debugs("map() without render");
		return;
	}

	D3D11_MAP m;
	if(clear)
	{
		numVerts=0;
		numIndices=0;
		numUndrawnIndices=0;
		m = D3D11_MAP_WRITE_DISCARD;
	}
	else
	{
		m = D3D11_MAP_WRITE_NO_OVERWRITE;
	}
	
	hr=D3DObjects.deviceContext->Map(D3DObjects.vertexBuffer,0,m,0,&mappedVBuffer);
	hr2=D3DObjects.deviceContext->Map(D3DObjects.indexBuffer,0,m,0,&mappedIBuffer);
	if(FAILED(hr) || FAILED(hr2))
	{
		UD3D12RenderDevice::debugs("Failed to map index and/or vertex buffer.");
	}
}

/**
Draw current buffer contents.
*/
void D3D::render()
{	
	if(mappedVBuffer.pData==NULL || mappedIBuffer.pData == NULL) //No buffer mapped, do nothing
	{
		return;
	}

	D3DObjects.deviceContext->Unmap(D3DObjects.vertexBuffer,0);
	mappedVBuffer.pData=NULL;
	D3DObjects.deviceContext->Unmap(D3DObjects.indexBuffer,0);
	mappedIBuffer.pData=NULL;
/*
	static unsigned int maxi;
	if(numUndrawnIndices>maxi)
	{
		maxi=numUndrawnIndices;
		printf("%d\n",maxi);
	}
*/
	//This shouldn't happen ever, but if it does we crash (negative amount of indices for draw()), so let's check anyway
	if(numIndices<numUndrawnIndices)
	{
		UD3D12RenderDevice::debugs("Buffer error.");
		numUndrawnIndices=0;
		return;
	}

	D3D::switchToPass(0)->Apply(0,D3DObjects.deviceContext);
	D3DObjects.deviceContext->DrawIndexed(numUndrawnIndices,numIndices-numUndrawnIndices,0);

	numUndrawnIndices=0;
}

/**
Commit buffered polys; i.e. draw and remap. Do this before changing state.
*/
void D3D::commit()
{
	if(numUndrawnIndices>0)
	{
		render();
		map(false);
	}
}

/**
Set up render targets, textures, etc. for the chosen pass.
\param index The number of the pass.
*/
ID3DX11EffectPass *D3D::switchToPass(int index)
{
	static int currIndex=-1;

	ID3DX11EffectPass* ret = NULL;
	if(index!=-1)
			ret = D3DObjects.effect->GetTechniqueByIndex(0)->GetPassByIndex(index);

	if(index!=currIndex)
	{
		UINT stride;
		UINT offset;
		
		switch (index)
		{
			case 0: //Geometry
			{
				stride=sizeof(Vertex);
				offset = 0;
				D3DObjects.deviceContext->IASetInputLayout(D3DObjects.vertexLayout);
				D3DObjects.deviceContext->IASetVertexBuffers( 0, 1, &D3DObjects.vertexBuffer, &stride, &offset );
				D3DObjects.deviceContext->IASetIndexBuffer(D3DObjects.indexBuffer,DXGI_FORMAT_R32_UINT,0);
				D3DObjects.deviceContext->OMSetRenderTargets(1,&D3DObjects.renderTargetView,D3DObjects.depthStencilView);	
				D3DObjects.deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				break;
			}
		}
		currIndex = index;

	}
	return ret;
}

/**
Postprocess and Flip
*/
void D3D::present()
{
	HRESULT hr;
	hr = D3DObjects.swapChain->Present((options.VSync!=0),0);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Present error.");
		return;
	}
}


/**
Generate index data so a triangle fan with 'num' vertices is converted to a triangle list. Should be called BEFORE those vertices are buffered.
\param num Number of vertices in the triangle fan.
*/
void D3D::indexTriangleFan(int num)
{		
	//Make sure there's index and vertex buffer room for a triangle fan; if not, the current buffer content is drawn and discarded
	//Index buffer is checked only, as there's equal or more indices than vertices. There.s 3*(n-1) indices for n vertices.
	int newIndices = (num-2)*3;
	
	if(numIndices+newIndices>I_BUFFER_SIZE)
	{
		D3D::render();
		D3D::map(true);	
	}

	//Generate fan indices
	int* iBufPtr = (int*)mappedIBuffer.pData;
	for(int i=1;i<num-1;i++)
	{
		iBufPtr[numIndices++] = numVerts; //Center point
		iBufPtr[numIndices++] = numVerts+i;
		iBufPtr[numIndices++] = numVerts+i+1;		
	}

	numUndrawnIndices += newIndices;
}

/**
Generate index data for a quad. See indexTriangleFan().
*/
void D3D::indexQuad()
{	
	const int newIndices = 6;
	if(numIndices+newIndices>I_BUFFER_SIZE)
	{
		D3D::render();
		D3D::map(true);	
	}
	int* iBufPtr = (int*) mappedIBuffer.pData;
	iBufPtr[numIndices++] = numVerts;
	iBufPtr[numIndices++] = numVerts+1;
	iBufPtr[numIndices++] = numVerts+2;
	iBufPtr[numIndices++] = numVerts+2;
	iBufPtr[numIndices++] = numVerts+3;
	iBufPtr[numIndices++] = numVerts;

	numUndrawnIndices += newIndices;
}

/**
Set up the viewport. Also sets height and width in shader.
\note Buffered polys must be committed first, otherwise glitches will occur (for example, Deus Ex security cams).
*/
void D3D::setViewPort(int X, int Y, int left, int top)
{
	static int oldX, oldY, oldLeft, oldTop;
	if(X!=oldX || Y!=oldY || left != oldLeft || top != oldTop)
	{
		
		commit();

		D3D12_VIEWPORT vp;
		vp.Width = X;
		vp.Height = Y;
		vp.MinDepth = 0.0;
		vp.MaxDepth = 1.0;
		vp.TopLeftX = left;
		vp.TopLeftY = top;

		D3DObjects.deviceContext->RSSetViewports(1,&vp);
		shaderVars.viewportHeight->SetFloat(Y);
		shaderVars.viewportWidth->SetFloat(X);
	}
	oldLeft = left; oldTop = top; oldX = X; oldY = Y;
}

/**
Returns a pointer to the next vertex in the buffer; this can then be set to buffer a model etc.
\return Vertex pointer.
*/
D3D::Vertex *D3D::getVertex()
{
	//Return a pointer to a vertex which can be filled in.
	return (D3D::Vertex*) &((D3D::Vertex*)mappedVBuffer.pData)[numVerts++];
}

/**
Set projection matrix parameters.
\param aspect The viewport aspect ratio.
\param XoverZ Ratio between frustum X and Z. Projection parameters are for z=1, so x over z gives x coordinate; and x/z*aspect=y/z=y.
*/
void D3D::setProjection(float aspect, float XoverZ)
{
	XMMATRIX m;
	static float oldAspect, oldXoverZ;
	if(aspect!=oldAspect || oldXoverZ != XoverZ)
	{
		commit();
		float xzProper = XoverZ*options.zNear; //Scale so larger near Z does not lead to zoomed in view
		m = XMMatrixPerspectiveOffCenterLH(-xzProper,xzProper,-aspect*xzProper,aspect*xzProper,options.zNear, 32760.0f); //Similar to glFrustum
		shaderVars.projection->SetMatrix(&m.m[0][0]);
		oldAspect = aspect;
		oldXoverZ = XoverZ;
	}
}

/*
Set shader projection mode. Only changes setting if new parameter differs from current state.
\param mode Mode (see D3D::ProjectionMode)
\note It's best to call this at the start of each type of primitive draw call, and not for instance before and after drawing a tile.
The 2nd option results in a switch every time instead of only when starting to draw another primitive type.
*/
void D3D::setProjectionMode(D3D::ProjectionMode mode)
{
	static D3D::ProjectionMode m;
	if(m!=mode)
	{
		commit();
		shaderVars.projectionMode->SetInt(mode);
		m= mode;
	}
}


/** Handle flags that change depth or blend state. See polyflags.h.
Only done if flag is different from current.
If there's any buffered geometry, it will drawn before setting the new flags.
\param flags Unreal polyflags.
\param D3Dflags Custom flags defined in D3D.h.
\note Bottleneck; make sure buffers are only rendered due to flag changes when absolutely necessary	
\note Deus Ex requires other different precedence rules for holoconvos with glasses-wearing characters to look good.
**/
void D3D::setFlags(int flags, int D3DFlags)
{
	const int BLEND_FLAGS = PF_Translucent | PF_Modulated |PF_Invisible |PF_Masked
	#ifdef RUNE
		| PF_AlphaBlend
	#endif
		;
	const int RELEVANT_FLAGS = BLEND_FLAGS|PF_Occlude;
	const int RELEVANT_D3D_FLAGS = 0;
	
	static int currFlags=0;
	static int currD3DFlags;

	
	if(!(flags & (PF_Translucent|PF_Modulated))) //If none of these flags, occlude (opengl renderer)
	{
		flags |= PF_Occlude;
	}

	int changedFlags = currFlags ^ flags;
	int changedD3DFlags = currD3DFlags ^ D3DFlags;
	if (changedFlags&RELEVANT_FLAGS || changedD3DFlags & RELEVANT_D3D_FLAGS) //only blend flag changes are relevant	
	{
		commit();

		//Set blend state		
		if(changedFlags & BLEND_FLAGS) //Only set blend state if it actually changed
		{
			ID3D11BlendState *blendState;
			if(flags&PF_Invisible)
			{
				blendState = states.bstate_Invis;
			}
			#ifdef DEUSEX
			else if(flags&PF_Modulated)
			{				
				blendState = states.bstate_Modulate;
			}
			else if(flags&PF_Translucent)
			{
				blendState = states.bstate_Translucent;
			}		 
			#else
			else if(flags&PF_Translucent)
			{
				blendState = states.bstate_Translucent;
			}
			else if(flags&PF_Modulated)
			{				
				blendState = states.bstate_Modulate;
			}
			#endif
						
			#ifdef RUNE
			else if (flags&PF_AlphaBlend)
			{
				blendState = states.bstate_Alpha;
			}
			#endif
			else if (flags&PF_Masked)
			{
				blendState = states.bstate_Masked;
			}
			else
			{
				blendState = states.bstate_NoBlend;
			}
			D3DObjects.deviceContext->OMSetBlendState(blendState,NULL,0xffffffff);
	
		}
		
		//Set depth state
		if(changedFlags & PF_Occlude)
		{
			ID3D11DepthStencilState *depthState;
			if(flags & PF_Occlude)
				depthState = states.dstate_Enable;
			else
				depthState = states.dstate_Disable;
			D3DObjects.deviceContext->OMSetDepthStencilState(depthState,1);
		}

		currFlags = flags;
		currD3DFlags = D3DFlags;
	}
}

/**
Return D3D12_CPU_DESCRIPTOR_HANDLE with RTV of the current back buffer.
*/
D3D12_CPU_DESCRIPTOR_HANDLE D3D::currentRenderTargetView()
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		D3DObjects.rtvHeap->GetCPUDescriptorHandleForHeapStart(), // handle start
		currBackBuf,	  // index to offset
		rtvDescriptorSize // byte size of descriptor
	);
}

/**
Return D3D12_CPU_DESCRIPTOR_HANDLE with DSV of the current back buffer.
*/
D3D12_CPU_DESCRIPTOR_HANDLE D3D::currentDepthStencilView()
{
	return D3DObjects.dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

/**
Create a texture from a descriptor and data to fill it with.
\param desc Direct3D texture description.
\param data Data to fill the texture with.
*/
ID3D11Texture2D *D3D::createTexture(D3D11_TEXTURE2D_DESC &desc, D3D11_SUBRESOURCE_DATA &data)
{
	//Creates a texture, setting the TextureInfo's data member.
	HRESULT hr;	

	ID3D11Texture2D *texture;
	hr=D3DObjects.device->CreateTexture2D(&desc,&data, &texture);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating texture resource.");
		return NULL;
	}
	return texture;
}

/**
Update a single texture mip using a copy operation.
\param id CacheID to insert texture with.
\param mipNum Mip level to update.
\param data Data to write to the mip.
*/
void D3D::updateMip(DWORD64 id,int mipNum,D3D11_SUBRESOURCE_DATA &data)
{
	ID3D11Resource* resource;

	//If texture is currently bound, draw buffers before updating
	for(int i=0;i<D3D::DUMMY_NUM_PASSES;i++)
	{
		if(texturePasses.boundTextureID[i]==id)
		{
			commit();
			break;
		}
	}

	//Update
	(&textureCache[id])->resourceView->GetResource(&resource);
	D3DObjects.deviceContext->UpdateSubresource(resource,mipNum,NULL,(void*) data.pSysMem,data.SysMemPitch,0);

}

/**
Create a resource view (texture usable by shader) from a filled-in texture and cache it. Caller can then release the texture.
\param id CacheID to insert texture with.
\param metadata Texture metadata.
\param tex A filled Direct3D texture.
*/
void D3D::cacheTexture(unsigned __int64 id,TextureMetaData &metadata,ID3D11Texture2D *tex)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	//Create resource view
	ID3D11ShaderResourceView* r;
	D3D11_SHADER_RESOURCE_VIEW_DESC srDesc;
	srDesc.Format = desc.Format;
	srDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srDesc.Texture2D.MostDetailedMip = 0;
	srDesc.Texture2D.MipLevels = desc.MipLevels;

	hr = D3DObjects.device->CreateShaderResourceView(tex,&srDesc,&r);
	if(FAILED(hr))
	{
		UD3D12RenderDevice::debugs("Error creating texture shader resource view.");
		return;
	}

	//Cache texture
	D3D::CachedTexture c;
	c.metadata = metadata;
	c.resourceView = r;
	textureCache[id]=c;	
}

/**
Returns true if texture is in cache.
\param id CacheID for texture.
*/
bool D3D::textureIsCached(DWORD64 id)
{	
	return textureCache.find(id) != textureCache.end();
}

/**
Returns texture metadata.
\param id CacheID for texture.
*/
D3D::TextureMetaData &D3D::getTextureMetaData(DWORD64 id)
{
	return textureCache[id].metadata;
}


/**
Set the texture for a texture pass (diffuse, lightmap, etc).
Texture is only set if it's not already the current one for that pass.
Cached polygons (using the previous set of textures) are drawn before the switch is made.
\param id CacheID for texture. NULL sets no texture for the pass (by disabling it using a shader constant).
\return texture metadata so renderer can use parameters such as scale/pan; NULL is texture not found
*/
D3D::TextureMetaData *D3D::setTexture(D3D::TexturePass pass,DWORD64 id)
{		
	static D3D::TextureMetaData *metadata[D3D::DUMMY_NUM_PASSES]; //Cache this so it can even be returned when no texture was actually set (because same id as last time)
	if(id!=texturePasses.boundTextureID[pass]) //If different texture than previous one, draw geometry in buffer and switch to new texture
	{			
		texturePasses.boundTextureID[pass]=id;
		
		commit();

		if(id==NULL) //Turn off texture
		{
			texturePasses.enabled[pass]=FALSE;
			metadata[pass]=NULL;	
			shaderVars.useTexturePass->SetBoolArray(texturePasses.enabled,0,D3D::DUMMY_NUM_PASSES);
		}
		else
		{
			//Turn on and switch to new texture			
			D3D::CachedTexture *tex;
			if(!textureIsCached(id)) //Texture not in cache, conversion probably went wrong.
				return NULL;
			tex = &textureCache[id];			
		
			shaderVars.shaderTextures->SetResourceArray(&tex->resourceView,pass,1);	
			if(!texturePasses.enabled[pass]) //Only updating this on change is faster than always doing it
			{				
				texturePasses.enabled[pass]=TRUE;
				shaderVars.useTexturePass->SetBoolArray(texturePasses.enabled,0,D3D::DUMMY_NUM_PASSES); 
			}			
			metadata[pass] = &tex->metadata;
		}
		
	}

	return metadata[pass];
}

/**
Delete a texture (so it can be overwritten with an updated one).
*/
void D3D::deleteTexture(DWORD64 id)
{
	stdext::hash_map<DWORD64,D3D::CachedTexture>::iterator i = textureCache.find(id);
	if(i==textureCache.end())
		return;
	SAFE_RELEASE(i->second.resourceView);
	textureCache.erase(i);
}

/**
Clear texture cache.
*/
void D3D::flush()
{
	for(int i=0;i<D3D::DUMMY_NUM_PASSES;i++)
	{
		setTexture((D3D::TexturePass)i,NULL);
	}

	//Delete textures
	for(stdext::hash_map<DWORD64,D3D::CachedTexture>::iterator i=textureCache.begin();i!=textureCache.end();i++)
	{	
		while(i->second.resourceView)
			SAFE_RELEASE(i->second.resourceView);
	}
	textureCache.clear();
}


/**
Notify the shader a flash effect should be drawn.
*/
void D3D::flash(bool enable, D3D::Vec4 &color)
{	
	shaderVars.flashEnable->SetBool(enable);
	if(enable)
		shaderVars.flashColor->SetFloatVector((float*)&color);
}

/**
Set the shader's fog settings.
*/
void D3D::fog(float dist, D3D::Vec4 *color)
{
	commit(); //Draw previous stuff that required different fog settings.
	shaderVars.fogDist->SetFloat(dist);
	if(dist>0)
	{		
		shaderVars.fogColor->SetFloatVector((float*)color);
	}
}

/**
Create a string of supported display modes.
\return String of modes. Caller must delete[] this.
\note No error checking.
\note Deus Ex and Unreal (non-Gold) only show 16 resolutions, so for it make it the 16 highest ones. Also for Unreal Gold for compatibity with v226.
*/	
TCHAR *D3D::getModes()
{
	TCHAR *out;

	//Get number of modes
	UINT num = 0;	
	D3DObjects.output->GetDisplayModeList(BACKBUFFER_FORMAT, NULL, &num, NULL);
	const int resStringLength = 10;
	out = new TCHAR[num*resStringLength+1];
	out[0]=0;
	
	DXGI_MODE_DESC * descs = new DXGI_MODE_DESC[num];
	D3DObjects.output->GetDisplayModeList(BACKBUFFER_FORMAT, NULL, &num, descs);
	
	//Add each mode once (disregard refresh rates etc)
	#if( DEUSEX || UNREAL || UNREALGOLD)
	const int maxItems = 16;
	int h[maxItems];
	int w[maxItems];
	h[0]=0;
	w[0]=0;
	int slot=maxItems-1;
	//Go through the modes backwards and find up to 16 ones
	for(int i=num;i>0&&slot>0;i--)
	{		
		if(slot==maxItems-1 || w[slot+1]!=descs[i-1].Width || h[slot+1]!=descs[i-1].Height)
		{
			w[slot]=descs[i-1].Width;
			h[slot]=descs[i-1].Height;
			printf("%d\n",w[slot]);
			slot--;
		}
	}
	//Build the string by now going through the saved modes forwards.
	for(int i=slot+1;i<maxItems;i++)	
	{
		TCHAR curr[resStringLength+1];
		swprintf_s(curr,resStringLength+1,L"%dx%d ",w[i],h[i]);
		wcscat_s(out,num*resStringLength+1,curr);	
	}
	#else
	int height = 0;
	int width = 0;
	for(unsigned int i=0;i<num;i++)
	{		
		if(width!=descs[i].Width || height!=descs[i].Height)
		{
			width=descs[i].Width;
			height=descs[i].Height;
			TCHAR curr[resStringLength+1];
			swprintf_s(curr,resStringLength+1,L"%dx%d ",width,height);
			wcscat_s(out,num*resStringLength+1,curr);
		}
	}
	#endif
	
	//Throw away trailing space
	out[wcslen(out)-1]=0;
	delete [] descs;
	return out;
}

/**
Return screen data by copying the back buffer to a staging resource and copying this into an array.
\param buf Array in which the data will be written.
\note No error checking.
*/
void D3D::getScreenshot(D3D::Vec4_byte* buf)
{
	ID3D11Texture2D* backBuffer;
	D3DObjects.swapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBuffer );

	D3D11_TEXTURE2D_DESC desc;
	backBuffer->GetDesc(&desc);
	desc.BindFlags = 0;
	desc.SampleDesc.Count=1;

	//Need to take two steps as backbuffer can be multisampled: copy backbuffer to default and default to staging. Could be skipped for performance level > 10.1
	//Copy backbuffer
	ID3D11Texture2D* tdefault;
	ID3D11Texture2D* tstaging;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	D3DObjects.device->CreateTexture2D(&desc,NULL,&tdefault);
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.Usage = D3D11_USAGE_STAGING;
	D3DObjects.device->CreateTexture2D(&desc,NULL,&tstaging);	
	D3DObjects.deviceContext->ResolveSubresource(tdefault,0,backBuffer,0,BACKBUFFER_FORMAT);
	D3DObjects.deviceContext->CopySubresourceRegion(tstaging,0,0,0,0,tdefault,0,NULL);
	

	//Map copy
	D3D11_MAPPED_SUBRESOURCE tempMapped;
	D3DObjects.deviceContext->Map(tstaging,0,D3D11_MAP_READ,0,&tempMapped);

	//Convert BGRA to RGBA, minding the source stride.
	D3D::Vec4_byte* rowSrc =(D3D::Vec4_byte*) tempMapped.pData;
	D3D::Vec4_byte* rowDst = buf;
	for(unsigned int row=0;row<desc.Height;row++)
	{
		for(unsigned int col=0;col<desc.Width;col++)
		{
			rowDst[col].x = rowSrc[col].z;
			rowDst[col].y = rowSrc[col].y;
			rowDst[col].z = rowSrc[col].x;
			rowDst[col].w = rowSrc[col].w;	
		}
		//Go to next row
		rowSrc+=(tempMapped.RowPitch/sizeof(D3D::Vec4_byte));
		rowDst+=desc.Width;
		
	}

	//Clean up
	D3DObjects.deviceContext->Unmap(tstaging,0);
	SAFE_RELEASE(backBuffer);
	SAFE_RELEASE(tdefault);
	SAFE_RELEASE(tstaging);
	UD3D12RenderDevice::debugs("Done.");
}

/**
Find the maximum level of MSAA supported by the device and clamp the options.MSAA setting to this.
\return 1 if succesful.
*/
int D3D::findAALevel()
{
	HRESULT hr;
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms;
	ms.Format = BACKBUFFER_FORMAT;
	ms.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	ms.NumQualityLevels = 0;
	// Descend through and check each sample count of the ms struct
	for (ms.SampleCount = options.samples; ms.NumQualityLevels == 0 && ms.SampleCount > 0; ms.SampleCount--)
	{
		hr = D3DObjects.device->CheckFeatureSupport(
			D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
			&ms,
			sizeof(ms)
		);
		if (FAILED(hr))
		{
			UD3D12RenderDevice::debugs("Error getting MSAA support level.");
			return 0;
		}

		if (ms.NumQualityLevels != 0) // A quality level of 0 means sample count not supported by hardware
			break;
	}

	// Lower the user-specified MSAA setting if higher than the max supported by hardware
	if(ms.SampleCount != options.samples)
	{
		UD3D12RenderDevice::debugs("Anti aliasing setting decreased; requested setting unsupported.");
		options.samples = ms.SampleCount;
	}

	return 1;
}

/**
Sets the in-shader brightness.
\param brightness Brightness 0-1.
*/
void D3D::setBrightness(float brightness)
{
	if(shaderVars.brightness->IsValid())
		shaderVars.brightness->SetFloat(brightness);
}