/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org>
*/

/*
	This example demonstrates how to get a very simple scene running in your HMD using Direct3D 11,
	with as little clutter as possible. Error handling and architecture is largely ignored. You
	probably do not want to base your own project on this code, or at least replace it as you go.

	https://github.com/poppeman/SimpleOVR

	In order to get this compiling, you will need to do some changes:

	Visual Studio 2013:
		* Right click the project in Solution Explorer and click "Properties".
		* Select VC++ Directories.
		* Change the Include Directories and Library Directories to (also) point at the appropriate locations.

		Example (ovr_sdk_win_0.4.3.zip extracted in C:\Src):
			Include Directories: C:\Src\OculusSDK\LibOVR\Include;C:\Src\OculusSDK\LibOVR\Src;$(IncludePath)
			Library Directories: C:\Src\OculusSDK\LibOVR\Lib\Win32\VS2013;$(LibraryPath)

	This demo ONLY handles Direct HMD Access mode.

	Known issues:
		* Running with DWM disabled ("Basic Theme") will eat CPU and possibly result in low FPS, at
		  least with mirroring enabled.
*/

// Prevent windows.h from breaking std::min and std::max.
#define NOMINMAX

// D3D support requires you to define which D3D version you use at compile time.
#define OVR_D3D_VERSION 11
#include <d3d11.h>
#include <d3dcompiler.h>
#include <OVR.h>
#include <OVR_CAPI_D3D.h>
#include <algorithm>
#include <vector>

const LPWSTR ClassName = L"SimpleOVR_D3D11";

/*
	Number of rendered pixels per display pixel. Generally you want this set at 1.0, but you
	can gain some performance by setting it lower in exchange for a more blurry result.
*/
const float PixelsPerDisplayPixel = 1.0f;
const int MultisampleCount = 4; // Set to 1 to disable multisampling

// Commonly used vectors.
const OVR::Vector3f	RightVector(1.0f, 0.0f, 0.0f);
const OVR::Vector3f	UpVector(0.0f, 1.0f, 0.0f);
const OVR::Vector3f	ForwardVector(0.0f, 0.0f, -1.0f);


// Position and angle of the player's body. In a real project these are probably not constant,
// but we're keeping things simple here.
const OVR::Vector3f BodyPosition{ 0.5f, 0.5f, 0 };
const OVR::Anglef BodyYaw{ 0.9f };


ID3D11Buffer* SetupScene(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);
void DestroyScene();

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
		case WM_CLOSE:
		case WM_DESTROY:
			PostQuitMessage(0);
			break;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

/*

	INTERESTING PART BEGINS HERE

*/

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd ) {
	ovrEyeRenderDesc vrEyeRenderDesc[2];
	ovrRecti vrEyeRenderViewport[2];
	ovrHmd vrHmd = nullptr;
	ovrFovPort vrEyeFov[2];
	ovrSizei renderTargetSize;
	ovrD3D11Texture vrEyeTexture[2];
	ovrD3D11Config vrRenderConfiguration;

	ID3D11Texture2D* d3dDepthStencilTexture = nullptr;
	ID3D11DepthStencilView* d3dDepthStencilView = nullptr;
	ID3D11RenderTargetView* d3dBackBufferRenderTargetView = nullptr;

	// Texture used for main rendering. LibOVR will use this as the source when rendering the final
	// distorted view to the HMD.
	ID3D11Texture2D* d3dEyeTexture = nullptr;
	ID3D11RenderTargetView* d3dEyeTextureRenderTargetView = nullptr;
	ID3D11ShaderResourceView* d3dEyeTextureShaderResourceView = nullptr;

	/*
		We need an additional intermediary rendertarget if (and only if) we use multisampling.

		Without multisampling the rendering process is like this:

			Geometry ----> Eye texture ----> Back buffer

		With multisampling we must add one step:
			Geometry ----> Eye texture ----> Intermediary ----> Back buffer

		All this may change in later
	*/
	ID3D11Texture2D* d3dIntermediaryTexture = nullptr;
	ID3D11RenderTargetView* d3dIntermediaryTextureRenderTargetView = nullptr;
	ID3D11ShaderResourceView* d3dIntermediaryTextureShaderResourceView = nullptr;

	/*
		This call prevents the window to get stretched on High-DPI systems. Alternatively you can
		do this by modifying the application manifest file. It is not terribly important though,
		especially if you disable mirroring.

		Further reading:
		http://msdn.microsoft.com/en-us/library/windows/desktop/dn469266%28v=vs.85%29.aspx
	*/
	SetProcessDPIAware();


	/*
		LibOVR Initialization part.
		Beware, LibOVR may be a bit picky about when you perform the various steps. ovr_Initialize
		must be called before you start initializing Direct3D.
	*/
	ovr_Initialize();

	vrHmd = ovrHmd_Create(0);
	if (vrHmd == nullptr) {
		// Forgetting to turn on the HMD is fairly common so we make an exception here and actually
		// add some error handling.
		MessageBox(nullptr, L"Failed initializing HMD, make sure it is connected and turned on.", L"LibOVR error", MB_OK);
		ovr_Shutdown();
		return EXIT_FAILURE;
	}

	// We'll request orientation and position tracking, but not require either. Adjust according to your needs.
	ovrHmd_ConfigureTracking(vrHmd, ovrTrackingCap_Orientation | ovrTrackingCap_Position, 0);

	// Fetch the texture sizes needed for the eye buffers.
	// We'll be using a single texture for both eyes, so we'll figure out how large that texture needs to be.
	auto ovrEyeDimsLeft = ovrHmd_GetFovTextureSize(vrHmd, ovrEye_Left, vrHmd->DefaultEyeFov[0], PixelsPerDisplayPixel);
	auto ovrEyeDimsRight = ovrHmd_GetFovTextureSize(vrHmd, ovrEye_Right, vrHmd->DefaultEyeFov[0], PixelsPerDisplayPixel);

	// We ARE making an assumption here that both eye buffers have the same width, as this is the case for DK2.
	renderTargetSize.w = ovrEyeDimsLeft.w + ovrEyeDimsRight.w;
	renderTargetSize.h = std::max(ovrEyeDimsLeft.h, ovrEyeDimsRight.h);

	// View ports for each eye. We'll be using a single a single render target and allocate half of it to each eye.
	vrEyeRenderViewport[0].Pos = { 0, 0 };
	vrEyeRenderViewport[0].Size = { renderTargetSize.w / 2, renderTargetSize.h };
	vrEyeRenderViewport[1].Pos = { (renderTargetSize.w + 1) / 2, 0 };
	vrEyeRenderViewport[1].Size = vrEyeRenderViewport[0].Size;


	// FOV for each eye.
	vrEyeFov[0] = vrHmd->DefaultEyeFov[0];
	vrEyeFov[1] = vrHmd->DefaultEyeFov[1];


	// Windows-specific initialization part.
	WNDCLASSEX wcx;
	ZeroMemory(&wcx, sizeof(wcx));
	wcx.cbSize = sizeof(wcx);
	wcx.lpszClassName = ClassName;
	wcx.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcx.hInstance = hInstance; // Tip: GetModuleHandle(nullptr) works just as well.
	wcx.lpfnWndProc = WndProc;

	RegisterClassEx(&wcx);

	/*
		The dimensions of the window does not need to match the dimensions of the actual HMD
		output. 
	*/
	auto hwnd = CreateWindowW(
		ClassName,
		L"SimpleOVR - D3D11",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		0, 0,
		1280, 720,
		nullptr,
		nullptr,
		hInstance,
		nullptr);

	/*
		D3D11 initialization.
		This example uses no fancy features, so we only require Direct3D 10.1 capable hardware.
		If you attempt to target anything less you might start getting crashes in LibOVR or various
		D3D-related errors.
	*/	
	D3D_FEATURE_LEVEL requestedLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
	D3D_FEATURE_LEVEL obtainedLevel;
	ID3D11Device* d3dDevice = nullptr;
	ID3D11DeviceContext* d3dContext = nullptr;

	DXGI_SWAP_CHAIN_DESC scd;
	ZeroMemory(&scd, sizeof(scd));
	scd.BufferCount = 1;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	scd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

	scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	scd.OutputWindow = hwnd;
	scd.SampleDesc.Count = MultisampleCount;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	scd.Windowed = false;

	// NOTE: LibOVR 0.4.3 requires that the width and height for the backbuffer is set even if
	// you use windowed mode, despite being optional according to the D3D11 documentation.
	scd.BufferDesc.Width = vrHmd->Resolution.w;
	scd.BufferDesc.Height = vrHmd->Resolution.h;
	scd.BufferDesc.RefreshRate.Numerator = 0;
	scd.BufferDesc.RefreshRate.Denominator = 1;

	UINT createFlags = 0;
#ifdef _DEBUG
	// This flag gives you some quite wonderful debug text. Not wonderful for performance, though!
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	IDXGISwapChain* d3dSwapChain = 0;

	D3D11CreateDeviceAndSwapChain(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		createFlags,
		requestedLevels,
		sizeof(requestedLevels) / sizeof(D3D_FEATURE_LEVEL),
		D3D11_SDK_VERSION,
		&scd,
		&d3dSwapChain,
		&d3dDevice,
		&obtainedLevel,
		&d3dContext);

	// Create a render target view for the backbuffer. This will be used during rendering when we
	// actually render the eye buffers to the HMD.
	ID3D11Texture2D* pBackBuffer = nullptr;
	d3dSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
	d3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &d3dBackBufferRenderTargetView);
	pBackBuffer->Release();

	// We don't get a depth buffer by default, and you'll probably want one of those.
	D3D11_TEXTURE2D_DESC dtd;
	ZeroMemory(&dtd, sizeof(dtd));
	dtd.Width = renderTargetSize.w;
	dtd.Height = renderTargetSize.h;
	dtd.MipLevels = 1;
	dtd.ArraySize = 1;
	dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	dtd.SampleDesc.Count = MultisampleCount;
	d3dDevice->CreateTexture2D(&dtd, nullptr, &d3dDepthStencilTexture);

	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	ZeroMemory(&dsvDesc, sizeof(dsvDesc));
	dsvDesc.Format = dtd.Format;
	dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;

	d3dDevice->CreateDepthStencilView(d3dDepthStencilTexture, &dsvDesc, &d3dDepthStencilView);

	// Allocate a texture that will hold both (undistorted) eye views. Later we'll let LibOVR use this texture
	// to render the final distorted view to the HMD.
	D3D11_TEXTURE2D_DESC texdesc;
	ZeroMemory(&texdesc, sizeof(texdesc));
	texdesc.Width = renderTargetSize.w;
	texdesc.Height = renderTargetSize.h;
	texdesc.MipLevels = 1;
	texdesc.ArraySize = 1;
	texdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texdesc.SampleDesc.Count = MultisampleCount;
	texdesc.Usage = D3D11_USAGE_DEFAULT;
	texdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d3dDevice->CreateTexture2D(&texdesc, nullptr, &d3dEyeTexture);
	d3dDevice->CreateShaderResourceView(d3dEyeTexture, nullptr, &d3dEyeTextureShaderResourceView);
	d3dDevice->CreateRenderTargetView(d3dEyeTexture, nullptr, &d3dEyeTextureRenderTargetView);

	if (MultisampleCount > 1) {
		// This render target is ONLY used for multisampling. More comments up at the variable declarations.
		D3D11_TEXTURE2D_DESC texdesc;
		ZeroMemory(&texdesc, sizeof(texdesc));
		texdesc.Width = renderTargetSize.w;
		texdesc.Height = renderTargetSize.h;
		texdesc.MipLevels = 1;
		texdesc.ArraySize = 1;
		texdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texdesc.SampleDesc.Count = 1; // NOT multisampled. We resolve the multisampled rendertarget to this one.
		texdesc.SampleDesc.Quality = 0;
		texdesc.Usage = D3D11_USAGE_DEFAULT;
		texdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		texdesc.CPUAccessFlags = 0;
		texdesc.MiscFlags = 0;
		d3dDevice->CreateTexture2D(&texdesc, nullptr, &d3dIntermediaryTexture);
		d3dDevice->CreateShaderResourceView(d3dIntermediaryTexture, nullptr, &d3dIntermediaryTextureShaderResourceView);
		d3dDevice->CreateRenderTargetView(d3dIntermediaryTexture, nullptr, &d3dIntermediaryTextureRenderTargetView);
	}

	// We'll let LibOVR take care of the distortion rendering for us, so we'll let it know where it
	// can find the undistorted eye buffers. We are sharing a single texture between both eyes.
	vrEyeTexture[0].D3D11.Header.API = ovrRenderAPI_D3D11;
	vrEyeTexture[0].D3D11.Header.TextureSize = renderTargetSize;
	vrEyeTexture[0].D3D11.Header.RenderViewport = vrEyeRenderViewport[0];

	// If we use multisampling we're actually rendering from the intermediary texture instead
	if (MultisampleCount > 1) {
		vrEyeTexture[0].D3D11.pSRView = d3dIntermediaryTextureShaderResourceView;
		vrEyeTexture[0].D3D11.pTexture = d3dIntermediaryTexture;
	}
	else {
		vrEyeTexture[0].D3D11.pSRView = d3dEyeTextureShaderResourceView;
		vrEyeTexture[0].D3D11.pTexture = d3dEyeTexture;
	}

	// Right eye uses the same texture, but different rendering viewport.
	vrEyeTexture[1] = vrEyeTexture[0];
	vrEyeTexture[1].D3D11.Header.RenderViewport = vrEyeRenderViewport[1];


	vrRenderConfiguration.D3D11.Header.API = ovrRenderAPI_D3D11;
	vrRenderConfiguration.D3D11.Header.RTSize = vrHmd->Resolution;
	vrRenderConfiguration.D3D11.pDevice = d3dDevice;
	vrRenderConfiguration.D3D11.pDeviceContext = d3dContext;
	vrRenderConfiguration.D3D11.pSwapChain = d3dSwapChain;
	vrRenderConfiguration.D3D11.pBackBufferRT = d3dBackBufferRenderTargetView;
	// NOTE: Header.Multisample does not seem to be used as of 0.4.3, so feel free to ignore it for now.
	vrRenderConfiguration.D3D11.Header.Multisample = MultisampleCount;

	ovrHmd_ConfigureRendering(vrHmd, &vrRenderConfiguration.Config, ovrDistortionCap_Chromatic | ovrDistortionCap_TimeWarp | ovrDistortionCap_Overdrive | ovrDistortionCap_Vignette, vrEyeFov, vrEyeRenderDesc);

	// This line can be skipped if the defaults are good enough for you.
	ovrHmd_SetEnabledCaps(vrHmd, ovrHmdCap_LowPersistence | ovrHmdCap_DynamicPrediction | ovrHmdCap_NoMirrorToWindow);

	// This is the magic part that enabled Direct HMD Access mode. Currently (0.4.3) it only works on Windows.
	ovrHmd_AttachToWindow(vrHmd, hwnd, nullptr, nullptr);

	ovrVector3f vrHmdToEyeViewOffset[2] = {
		vrEyeRenderDesc[0].HmdToEyeViewOffset,
		vrEyeRenderDesc[1].HmdToEyeViewOffset
	};

	// Finally we'll create everything we need for a very simple scene. This is probably not very
	// interesting so I have hidden it in a separate function.
	auto d3dConstantBuffer = SetupScene(d3dDevice, d3dContext);

	bool keepRunning = true;
	while (keepRunning) {
		MSG msg;
		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				keepRunning = false;
			}

			// Pressing a key will cause a recenter and attempt to dismiss the health warning.
			// Many other VR applications use F12 for recentering.
			if (msg.message == WM_KEYDOWN) {
				ovrHmd_RecenterPose(vrHmd);
				ovrHmd_DismissHSWDisplay(vrHmd);
			}

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		// Rendering part
		ovrHmd_BeginFrame(vrHmd, 0);

		ovrPosef vrEyeRenderPose[2];
		ovrTrackingState hmdTrackingState;
		ovrHmd_GetEyePoses(vrHmd, 0, vrHmdToEyeViewOffset, vrEyeRenderPose, &hmdTrackingState);

		float f[] = { 0.2f, 0.3f, 0.2f, 1 };
		d3dContext->ClearRenderTargetView(d3dEyeTextureRenderTargetView, f);
		d3dContext->ClearDepthStencilView(d3dDepthStencilView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);

		// We use one single render target for both eyes.
		d3dContext->OMSetRenderTargets(1, &d3dEyeTextureRenderTargetView, d3dDepthStencilView);

		// We'll assume people have at most two eyes.
		for (int i = 0; i < 2; i++) {
			// The HMD might want us to render each eye in a specific order for best result.
			auto eye = vrHmd->EyeRenderOrder[i];

			// Use the viewport for the current eye
			D3D11_VIEWPORT vp;
			vp.Width = static_cast<float>(vrEyeRenderViewport[eye].Size.w);
			vp.Height = static_cast<float>(vrEyeRenderViewport[eye].Size.h);
			vp.TopLeftX = static_cast<float>(vrEyeRenderViewport[eye].Pos.x);
			vp.TopLeftY = static_cast<float>(vrEyeRenderViewport[eye].Pos.y);
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			d3dContext->RSSetViewports(1, &vp);

			// All left now is to render the scene. 

			// Calculate projection and view for the current eye. You'll probably replace all of this in
			// your own project.
			OVR::Posef currentEyePose = vrEyeRenderPose[eye];
			OVR::Matrix4f projection = ovrMatrix4f_Projection(vrEyeRenderDesc[eye].Fov, 0.01f, 10000.0f, true);
			OVR::Quatf quatBodyRotation = OVR::Quatf(UpVector, BodyYaw.Get());
			auto worldPose = OVR::Posef(
				quatBodyRotation * currentEyePose.Rotation, // Final rotation (body AND head)
				BodyPosition + quatBodyRotation.Rotate(currentEyePose.Translation) // Final position (body AND eye)
			);

			auto up = worldPose.Rotation.Rotate(UpVector);
			auto forward = worldPose.Rotation.Rotate(ForwardVector);

			OVR::Matrix4f view = OVR::Matrix4f::LookAtRH(worldPose.Translation, worldPose.Translation + forward, up);

			OVR::Matrix4f mvp = projection * view;

			// Send the View-Projection matrix to the Vertex Shader.
			// The shader only expects the matrix so we're taking the quick and dirty approach.
			ovrMatrix4f transposedMvp = mvp.Transposed();
			D3D11_MAPPED_SUBRESOURCE d3dMappedStatus;
			auto ms = d3dContext->Map(d3dConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &d3dMappedStatus);
			std::memcpy(d3dMappedStatus.pData, &transposedMvp, sizeof(float) * 16);
			d3dContext->Unmap(d3dConstantBuffer, 0);

			d3dContext->Draw(3, 0);
		}

		if (MultisampleCount > 1) {
			d3dContext->ResolveSubresource(d3dIntermediaryTexture, 0, d3dEyeTexture, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
		}

		/*
			Finish the current frame and send it to the HMD. swapChain->Present is called 
			automatically inside this function.
		*/
		ovrHmd_EndFrame(vrHmd, vrEyeRenderPose, &vrEyeTexture[0].Texture);
	}


	/*
		Cleanup part.
	*/
	DestroyScene();
	if (d3dIntermediaryTextureShaderResourceView != nullptr) {
		d3dIntermediaryTextureShaderResourceView->Release();
	}
	if (d3dIntermediaryTextureRenderTargetView != nullptr) {
		d3dIntermediaryTextureRenderTargetView->Release();
	}
	if (d3dIntermediaryTexture != nullptr) {
		d3dIntermediaryTexture->Release();
	}
	d3dDepthStencilView->Release();
	d3dDepthStencilTexture->Release();
	d3dEyeTextureRenderTargetView->Release();
	d3dEyeTextureShaderResourceView->Release();
	d3dEyeTexture->Release();
	d3dBackBufferRenderTargetView->Release();
	d3dSwapChain->Release();
	d3dContext->Release();
	d3dDevice->Release();
	ovrHmd_Destroy(vrHmd);
	ovr_Shutdown();

	return EXIT_SUCCESS;
}

/*

	Below is a bunch of code that prepares the scene. It has very little to do with the actual VR,
	but this example would be fairly boring without anything to look at.

*/

const char* VertexShaderCode = 
	"struct VS_INPUT {"
	"	float3 coord : POSITION;"
	"};"
	"struct PS_INPUT {"
	"	float4 pos : SV_Position;"
	"};"
	"cbuffer Constants {"
	"	float4x4 mvp;"
	"};"
	"PS_INPUT main(VS_INPUT v) {"
	"	PS_INPUT pi;"
	"	pi.pos = mul(mvp, float4(v.coord, 1.0));"
	"	return pi;"
	"}";

const char* PixelShaderCode =
	"struct PS_INPUT {"
	"	float4 pos : SV_Position;"
	"};"

	"float4 main(PS_INPUT pi) :SV_Target{"
	"	return float4(1, 0.8f, 0.8f, 1);"
	"}";

struct Vertex {
	float Position[3];
};

ID3D11InputLayout* d3dInputLayout = nullptr;
ID3D11VertexShader* d3dVertexShader = nullptr;
ID3D11PixelShader* d3dPixelShader = nullptr;
ID3D11Buffer* d3dConstantBuffer = nullptr;
ID3D11Buffer* d3dVertexBuffer = nullptr;

ID3D11Buffer* SetupScene(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext) {
	ID3D10Blob* d3dBlobVertexShader = nullptr;
	ID3D10Blob* d3dBlobPixelShader = nullptr;

	// The vertices of our scene. Perhaps not very exciting.
	std::vector<Vertex> vertices = {
		Vertex{ { -1, -1, -0.5 } },
		Vertex{ { -1, 1, -1.5 } },
		Vertex{ { 1, -1, -0.5 } },
	};


	D3DCompile(VertexShaderCode, strlen(VertexShaderCode), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &d3dBlobVertexShader, nullptr);
	d3dDevice->CreateVertexShader(d3dBlobVertexShader->GetBufferPointer(), d3dBlobVertexShader->GetBufferSize(), nullptr, &d3dVertexShader);
	D3DCompile(PixelShaderCode, strlen(PixelShaderCode), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &d3dBlobPixelShader, nullptr);
	d3dDevice->CreatePixelShader(d3dBlobPixelShader->GetBufferPointer(), d3dBlobPixelShader->GetBufferSize(), nullptr, &d3dPixelShader);

	D3D11_INPUT_ELEMENT_DESC inputElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	d3dDevice->CreateInputLayout(inputElements, 1, d3dBlobVertexShader->GetBufferPointer(), d3dBlobVertexShader->GetBufferSize(), &d3dInputLayout);

	D3D11_BUFFER_DESC vbDesc;
	ZeroMemory(&vbDesc, sizeof(vbDesc));
	vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbDesc.ByteWidth = sizeof(Vertex) * vertices.size();
	vbDesc.Usage = D3D11_USAGE_DEFAULT;

	D3D11_SUBRESOURCE_DATA initialData;
	initialData.pSysMem = vertices.data();
	initialData.SysMemPitch = sizeof(Vertex);

	d3dDevice->CreateBuffer(&vbDesc, &initialData, &d3dVertexBuffer);

	D3D11_BUFFER_DESC cbDesc;
	ZeroMemory(&cbDesc, sizeof(cbDesc));
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.ByteWidth = sizeof(float) * 16;

	d3dDevice->CreateBuffer(&cbDesc, nullptr, &d3dConstantBuffer);

	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	d3dContext->IASetInputLayout(d3dInputLayout);
	d3dContext->IASetVertexBuffers(0, 1, &d3dVertexBuffer, &stride, &offset);
	d3dContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d3dContext->VSSetShader(d3dVertexShader, nullptr, 0);
	d3dContext->PSSetShader(d3dPixelShader, nullptr, 0);
	d3dContext->VSSetConstantBuffers(0, 1, &d3dConstantBuffer);

	d3dBlobPixelShader->Release();
	d3dBlobVertexShader->Release();

	return d3dConstantBuffer;
}

void DestroyScene() {
	d3dConstantBuffer->Release();
	d3dVertexBuffer->Release();
	d3dInputLayout->Release();
	d3dVertexShader->Release();
	d3dPixelShader->Release();
}
