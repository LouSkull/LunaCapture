#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>

static_assert(sizeof(NOTIFYICONDATAW) > 0);
static_assert(sizeof(D3D11_TEXTURE2D_DESC) > 0);
static_assert(sizeof(DXGI_OUTDUPL_FRAME_INFO) > 0);

int WindowsHeaderSmoke() {
    IWICImagingFactory* factory = nullptr;
    return factory == nullptr ? 0 : 1;
}
