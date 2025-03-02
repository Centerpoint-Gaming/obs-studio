/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "d3d11-subsystem.hpp"
#include <unordered_map>

// Window class name for the display window
#define DISPLAY_WINDOW_CLASS L"OBSDuplicatorWindow"

// Window procedure for the display window
static LRESULT CALLBACK DisplayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_CLOSE:
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DESTROY:
		return 0;
	case WM_SIZE:
		// Resize handling is done when presenting the frame
		return 0;
	}
	return DefWindowProc(hwnd, message, wParam, lParam);
}

// Register the window class
static bool RegisterDisplayWindowClass()
{
	static bool registered = false;
	if (registered)
		return true;

	WNDCLASSW wc = {};
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = DisplayWindowProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = DISPLAY_WINDOW_CLASS;

	if (!RegisterClassW(&wc)) {
		blog(LOG_ERROR, "Failed to register display window class");
		return false;
	}

	registered = true;
	return true;
}

static inline bool get_monitor(gs_device_t *device, int monitor_idx, IDXGIOutput **dxgiOutput)
{
	HRESULT hr;

	hr = device->adapter->EnumOutputs(monitor_idx, dxgiOutput);
	if (FAILED(hr)) {
		if (hr == DXGI_ERROR_NOT_FOUND)
			return false;

		throw HRError("Failed to get output", hr);
	}

	return true;
}

void gs_duplicator::Start()
{
	ComPtr<IDXGIOutput5> output5;
	ComPtr<IDXGIOutput1> output1;
	ComPtr<IDXGIOutput> output;
	HRESULT hr;

	if (!get_monitor(device, idx, output.Assign()))
		throw "Invalid monitor index";

	hr = output->QueryInterface(IID_PPV_ARGS(output5.Assign()));
	hdr = false;
	sdr_white_nits = 80.f;
	if (SUCCEEDED(hr)) {
		constexpr DXGI_FORMAT supportedFormats[]{
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_B8G8R8A8_UNORM,
		};
		hr = output5->DuplicateOutput1(device->device, 0, _countof(supportedFormats), supportedFormats,
					       duplicator.Assign());
		if (FAILED(hr))
			throw HRError("Failed to DuplicateOutput1", hr);
		DXGI_OUTPUT_DESC desc;
		if (SUCCEEDED(output->GetDesc(&desc))) {
			gs_monitor_color_info info = device->GetMonitorColorInfo(desc.Monitor);
			hdr = info.hdr;
			sdr_white_nits = (float)info.sdr_white_nits;
		}
	} else {
		hr = output->QueryInterface(IID_PPV_ARGS(output1.Assign()));
		if (FAILED(hr))
			throw HRError("Failed to query IDXGIOutput1", hr);

		hr = output1->DuplicateOutput(device->device, duplicator.Assign());
		if (FAILED(hr))
			throw HRError("Failed to DuplicateOutput", hr);
	}

	// Create the display window
	CreateDisplayWindow();
}

void gs_duplicator::CreateDisplayWindow()
{
	if (!RegisterDisplayWindowClass())
		return;

	// Get monitor info to determine window size and position
	DXGI_OUTPUT_DESC outputDesc;
	ComPtr<IDXGIOutput> output;
	
	if (!get_monitor(this->device, this->idx, output.Assign()))
		return;

	if (FAILED(output->GetDesc(&outputDesc)))
		return;

	RECT monitorRect = outputDesc.DesktopCoordinates;
	int width = monitorRect.right - monitorRect.left;
	int height = monitorRect.bottom - monitorRect.top;

	// Create the window
	this->displayWindow = CreateWindowW(
		DISPLAY_WINDOW_CLASS,
		L"Screen Display",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		width / 2, height / 2, // Start with half the monitor size
		NULL, NULL, GetModuleHandle(NULL), NULL);

	if (!this->displayWindow) {
		blog(LOG_ERROR, "Failed to create display window");
		return;
	}

	// Create swap chain for the window
	struct gs_init_data initData = {};
	initData.window.hwnd = this->displayWindow;
	initData.cx = width / 2;
	initData.cy = height / 2;
	initData.format = GS_BGRA;
	initData.zsformat = GS_ZS_NONE;
	initData.num_backbuffers = 1;
	
	try {
		this->displaySwapChain = new gs_swap_chain(this->device, &initData);
		ShowWindow(this->displayWindow, SW_SHOW);
	} catch (const HRError &error) {
		blog(LOG_ERROR, "Failed to create swap chain: %s (%08lX)", error.str, error.hr);
		DestroyWindow(this->displayWindow);
		this->displayWindow = NULL;
	}
}

gs_duplicator::gs_duplicator(gs_device_t *device_, int monitor_idx)
	: gs_obj(device_, gs_type::gs_duplicator),
	  texture(nullptr),
	  idx(monitor_idx),
	  refs(1),
	  updated(false),
	  displayWindow(NULL),
	  displaySwapChain(nullptr)
{
	Start();
}

gs_duplicator::~gs_duplicator()
{
	delete texture;
	
	if (displaySwapChain) {
		delete displaySwapChain;
		displaySwapChain = nullptr;
	}
	
	if (displayWindow) {
		DestroyWindow(displayWindow);
		displayWindow = NULL;
	}
}

// Function to display the captured frame in the window
void gs_duplicator::PresentFrame()
{
	// Validate that we have everything we need
	if (!this->displayWindow || !this->displaySwapChain || !this->texture || !this->texture->texture) {
		return;
	}
		
	// Check if the window is visible
	if (!IsWindowVisible(this->displayWindow)) {
		return;
	}
		
	// Check if window needs resizing
	RECT clientRect;
	GetClientRect(this->displayWindow, &clientRect);
	uint32_t windowWidth = clientRect.right - clientRect.left;
	uint32_t windowHeight = clientRect.bottom - clientRect.top;
	
	if (windowWidth == 0 || windowHeight == 0) {
		return; // Skip rendering to 0-sized windows
	}
	
	// Resize the swap chain if necessary
	if (windowWidth != this->displaySwapChain->target.width || 
	    windowHeight != this->displaySwapChain->target.height) {
		try {
			this->displaySwapChain->Resize(windowWidth, windowHeight, GS_BGRA);
		} catch (const HRError &error) {
			blog(LOG_ERROR, "Failed to resize swap chain: %s (%08lX)", error.str, error.hr);
			return;
		}
	}
		
	// Store the current swap chain
	gs_swap_chain *prevSwapChain = this->device->curSwapChain;
	
	// Load our swap chain
	this->device->curSwapChain = this->displaySwapChain;
	
	// Set the viewport
	gs_rect viewport = {};
	viewport.x = viewport.y = 0;
	viewport.cx = this->displaySwapChain->target.width;
	viewport.cy = this->displaySwapChain->target.height;
	this->device->viewport = viewport;

	// Get the render target view and check if it exists
	ID3D11RenderTargetView *rtv = this->displaySwapChain->target.renderTarget[0].Get();
	if (!rtv) {
		blog(LOG_ERROR, "Missing render target view");
		this->device->curSwapChain = prevSwapChain;
		return;
	}
	
	// Clear the render target
	float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	this->device->context->ClearRenderTargetView(rtv, color);
	
	// Set render target
	this->device->context->OMSetRenderTargets(1, &rtv, nullptr);
	
	// Copy the texture to the back buffer using D3D11 context functions
	D3D11_BOX srcBox = {};
	srcBox.left = 0;
	srcBox.top = 0;
	srcBox.front = 0;
	srcBox.right = this->texture->width;
	srcBox.bottom = this->texture->height;
	srcBox.back = 1;
	
	// Use CopySubresourceRegion to copy the texture to the swap chain's back buffer
	this->device->context->CopySubresourceRegion(
		this->displaySwapChain->target.texture,
		0, 0, 0, 0,
		this->texture->texture,
		0, &srcBox);
	
	// Present the frame
	UINT interval = this->displaySwapChain->hWaitable ? 1 : 0;
	this->displaySwapChain->swap->Present(interval, 0);
	
	// Restore the original swap chain
	this->device->curSwapChain = prevSwapChain;
}

extern "C" {

EXPORT bool device_get_duplicator_monitor_info(gs_device_t *device, int monitor_idx, struct gs_monitor_info *info)
{
	DXGI_OUTPUT_DESC desc;
	HRESULT hr;

	try {
		ComPtr<IDXGIOutput> output;

		if (!get_monitor(device, monitor_idx, output.Assign()))
			return false;

		hr = output->GetDesc(&desc);
		if (FAILED(hr))
			throw HRError("GetDesc failed", hr);

	} catch (const HRError &error) {
		blog(LOG_ERROR,
		     "device_get_duplicator_monitor_info: "
		     "%s (%08lX)",
		     error.str, error.hr);
		return false;
	}

	switch (desc.Rotation) {
	case DXGI_MODE_ROTATION_UNSPECIFIED:
	case DXGI_MODE_ROTATION_IDENTITY:
		info->rotation_degrees = 0;
		break;

	case DXGI_MODE_ROTATION_ROTATE90:
		info->rotation_degrees = 90;
		break;

	case DXGI_MODE_ROTATION_ROTATE180:
		info->rotation_degrees = 180;
		break;

	case DXGI_MODE_ROTATION_ROTATE270:
		info->rotation_degrees = 270;
		break;
	}

	info->x = desc.DesktopCoordinates.left;
	info->y = desc.DesktopCoordinates.top;
	info->cx = desc.DesktopCoordinates.right - info->x;
	info->cy = desc.DesktopCoordinates.bottom - info->y;

	return true;
}

EXPORT int device_duplicator_get_monitor_index(gs_device_t *device, void *monitor)
{
	const HMONITOR handle = (HMONITOR)monitor;

	int index = -1;

	UINT output = 0;
	while (index == -1) {
		IDXGIOutput *pOutput;
		const HRESULT hr = device->adapter->EnumOutputs(output, &pOutput);
		if (hr == DXGI_ERROR_NOT_FOUND)
			break;

		if (SUCCEEDED(hr)) {
			DXGI_OUTPUT_DESC desc;
			if (SUCCEEDED(pOutput->GetDesc(&desc))) {
				if (desc.Monitor == handle)
					index = output;
			} else {
				blog(LOG_ERROR,
				     "device_duplicator_get_monitor_index: "
				     "Failed to get desc (%08lX)",
				     hr);
			}

			pOutput->Release();
		} else if (hr == DXGI_ERROR_NOT_FOUND) {
			blog(LOG_ERROR,
			     "device_duplicator_get_monitor_index: "
			     "Failed to get output (%08lX)",
			     hr);
		}

		++output;
	}

	return index;
}

static std::unordered_map<int, gs_duplicator *> instances;

void reset_duplicators(void)
{
	for (std::pair<const int, gs_duplicator *> &pair : instances) {
		pair.second->updated = false;
	}
}

EXPORT gs_duplicator_t *device_duplicator_create(gs_device_t *device, int monitor_idx)
{
	gs_duplicator *duplicator = nullptr;

	const auto it = instances.find(monitor_idx);
	if (it != instances.end()) {
		duplicator = it->second;
		duplicator->refs++;
		return duplicator;
	}

	try {
		duplicator = new gs_duplicator(device, monitor_idx);
		instances[monitor_idx] = duplicator;

	} catch (const char *error) {
		blog(LOG_DEBUG, "device_duplicator_create: %s", error);
		return nullptr;

	} catch (const HRError &error) {
		blog(LOG_DEBUG, "device_duplicator_create: %s (%08lX)", error.str, error.hr);
		return nullptr;
	}

	return duplicator;
}

EXPORT void gs_duplicator_destroy(gs_duplicator_t *duplicator)
{
	if (--duplicator->refs == 0) {
		instances.erase(duplicator->idx);
		delete duplicator;
	}
}

static inline void copy_texture(gs_duplicator_t *d, ID3D11Texture2D *tex)
{
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	const gs_color_format format = ConvertDXGITextureFormat(desc.Format);
	const gs_color_format general_format = gs_generalize_format(format);

	if (!d->texture || (d->texture->width != desc.Width) || (d->texture->height != desc.Height) ||
	    (d->texture->format != general_format)) {

		delete d->texture;
		d->texture = (gs_texture_2d *)gs_texture_create(desc.Width, desc.Height, general_format, 1, nullptr, 0);
		d->color_space =
			d->hdr ? GS_CS_709_SCRGB
			       : ((desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? GS_CS_SRGB_16F : GS_CS_SRGB);
	}

	if (d->texture)
		d->device->context->CopyResource(d->texture->texture, tex);
}

EXPORT bool gs_duplicator_update_frame(gs_duplicator_t *d)
{
	DXGI_OUTDUPL_FRAME_INFO info;
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<IDXGIResource> res;
	HRESULT hr;

	if (!d->duplicator) {
		return false;
	}
	
	// Check if window still exists first
	if (d->displayWindow && d->displaySwapChain && d->texture && d->updated) {
		// Continue presenting existing frame if no new one
		d->PresentFrame();
		return true;
	}

	hr = d->duplicator->AcquireNextFrame(0, &info, res.Assign());
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		return false;

	} else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		return true;

	} else if (FAILED(hr)) {
		blog(LOG_ERROR,
		     "gs_duplicator_update_frame: Failed to update "
		     "frame (%08lX)",
		     hr);
		return true;
	}

	hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)tex.Assign());
	if (FAILED(hr)) {
		blog(LOG_ERROR,
		     "gs_duplicator_update_frame: Failed to query "
		     "ID3D11Texture2D (%08lX)",
		     hr);
		d->duplicator->ReleaseFrame();
		return true;
	}

	copy_texture(d, tex);
	d->duplicator->ReleaseFrame();
	d->updated = true;
	
	// Display the captured frame in the window if window exists
	if (d->displayWindow && d->displaySwapChain) {
		d->PresentFrame();
	}
	
	return true;
}

EXPORT gs_texture_t *gs_duplicator_get_texture(gs_duplicator_t *duplicator)
{
	return duplicator->texture;
}

EXPORT enum gs_color_space gs_duplicator_get_color_space(gs_duplicator_t *duplicator)
{
	return duplicator->color_space;
}

EXPORT float gs_duplicator_get_sdr_white_level(gs_duplicator_t *duplicator)
{
	return duplicator->sdr_white_nits;
}

EXPORT void gs_duplicator_show_window(gs_duplicator_t *duplicator, bool show)
{
	if (!duplicator || !duplicator->displayWindow)
		return;
		
	ShowWindow(duplicator->displayWindow, show ? SW_SHOW : SW_HIDE);
}
}
