#include "wglContext.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

static constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001;
static constexpr int WGL_ACCELERATION_ARB = 0x2003;
static constexpr int WGL_SUPPORT_OPENGL_ARB = 0x2010;
static constexpr int WGL_DOUBLE_BUFFER_ARB = 0x2011;
static constexpr int WGL_PIXEL_TYPE_ARB = 0x2013;
static constexpr int WGL_COLOR_BITS_ARB = 0x2014;
static constexpr int WGL_ALPHA_BITS_ARB = 0x201B;
static constexpr int WGL_DEPTH_BITS_ARB = 0x2022;
static constexpr int WGL_STENCIL_BITS_ARB = 0x2023;
static constexpr int WGL_FULL_ACCELERATION_ARB = 0x2027;
static constexpr int WGL_TYPE_RGBA_ARB = 0x202B;

static constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
static constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
static constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
static constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
static constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;

using PFNWGLCHOOSEPIXELFORMATARBPROC = BOOL(WINAPI*)(
    HDC hdc,
    const int* piAttribIList,
    const FLOAT* pfAttribFList,
    UINT nMaxFormats,
    int* piFormats,
    UINT* nNumFormats);

using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI*)(
    HDC hDC,
    HGLRC hShareContext,
    const int* attribList);

static constexpr wchar_t kWindowClassName[] = L"UsdScreenshotHiddenWglWindow";

static bool EnsureWindowClassRegistered()
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    registered = true;
    return true;
}

static HWND CreateHiddenWindow()
{
    return CreateWindowExW(
        0,
        kWindowClassName,
        L"usdScreenshot",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
}

static PIXELFORMATDESCRIPTOR MakeDefaultPixelFormatDescriptor()
{
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    return pfd;
}

static void SetError(std::string* outError, const char* msg)
{
    if (outError)
        *outError = msg;
}

OffscreenWglContext::~OffscreenWglContext()
{
    Shutdown();
}

bool OffscreenWglContext::Initialize(std::string* errorMessage)
{
    Shutdown();

    if (!EnsureWindowClassRegistered()) {
        SetError(errorMessage, "Failed to register hidden WGL window class.");
        return false;
    }

    // Create a bootstrap window and legacy GL context to query WGL ARB extensions.
    HWND dummyHwnd = CreateHiddenWindow();
    if (!dummyHwnd) {
        SetError(errorMessage, "Failed to create bootstrap hidden window.");
        return false;
    }

    HDC dummyHdc = GetDC(dummyHwnd);
    const PIXELFORMATDESCRIPTOR defaultPfd = MakeDefaultPixelFormatDescriptor();
    const int dummyPixelFormat = ChoosePixelFormat(dummyHdc, &defaultPfd);
    if (dummyPixelFormat == 0 || !SetPixelFormat(dummyHdc, dummyPixelFormat, &defaultPfd)) {
        ReleaseDC(dummyHwnd, dummyHdc);
        DestroyWindow(dummyHwnd);
        SetError(errorMessage, "Failed to set bootstrap WGL pixel format.");
        return false;
    }

    HGLRC dummyContext = wglCreateContext(dummyHdc);
    if (!dummyContext || !wglMakeCurrent(dummyHdc, dummyContext)) {
        if (dummyContext)
            wglDeleteContext(dummyContext);
        ReleaseDC(dummyHwnd, dummyHdc);
        DestroyWindow(dummyHwnd);
        SetError(errorMessage, "Failed to create bootstrap WGL context.");
        return false;
    }

    auto wglChoosePixelFormatARB = reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(
        wglGetProcAddress("wglChoosePixelFormatARB"));
    auto wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        wglGetProcAddress("wglCreateContextAttribsARB"));

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummyContext);
    ReleaseDC(dummyHwnd, dummyHdc);
    DestroyWindow(dummyHwnd);

    // Create the actual offscreen window and OpenGL 4.5+ context.
    HWND hwnd = CreateHiddenWindow();
    if (!hwnd) {
        SetError(errorMessage, "Failed to create offscreen WGL window.");
        return false;
    }

    HDC hdc = GetDC(hwnd);
    bool pixelFormatSet = false;
    if (wglChoosePixelFormatARB) {
        const int pixelAttribs[] = {
            WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
            WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
            WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
            WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB, 32,
            WGL_ALPHA_BITS_ARB, 8,
            WGL_DEPTH_BITS_ARB, 24,
            WGL_STENCIL_BITS_ARB, 8,
            0
        };
        int pixelFormat = 0;
        UINT numFormats = 0;
        if (wglChoosePixelFormatARB(hdc, pixelAttribs, nullptr, 1, &pixelFormat, &numFormats) &&
            numFormats > 0) {
            PIXELFORMATDESCRIPTOR chosenPfd{};
            DescribePixelFormat(hdc, pixelFormat, sizeof(chosenPfd), &chosenPfd);
            pixelFormatSet = SetPixelFormat(hdc, pixelFormat, &chosenPfd) != FALSE;
        }
    }

    if (!pixelFormatSet) {
        const int fallbackFormat = ChoosePixelFormat(hdc, &defaultPfd);
        if (fallbackFormat != 0)
            pixelFormatSet = SetPixelFormat(hdc, fallbackFormat, &defaultPfd) != FALSE;
    }

    if (!pixelFormatSet) {
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        SetError(errorMessage, "Failed to configure pixel format on offscreen WGL window.");
        return false;
    }

    HGLRC hglrc = nullptr;
    if (wglCreateContextAttribsARB) {
        struct VersionAttempt
        {
            int major;
            int minor;
            int profileMask;
        };
        constexpr VersionAttempt attempts[] = {
            { 4, 6, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB },
            { 4, 5, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB },
            { 4, 6, WGL_CONTEXT_CORE_PROFILE_BIT_ARB },
            { 4, 5, WGL_CONTEXT_CORE_PROFILE_BIT_ARB },
        };

        for (const auto& attempt : attempts) {
            const int contextAttribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, attempt.major,
                WGL_CONTEXT_MINOR_VERSION_ARB, attempt.minor,
                WGL_CONTEXT_PROFILE_MASK_ARB, attempt.profileMask,
                0
            };
            hglrc = wglCreateContextAttribsARB(hdc, nullptr, contextAttribs);
            if (hglrc)
                break;
        }
    }

    if (!hglrc)
        hglrc = wglCreateContext(hdc);

    if (!hglrc || !wglMakeCurrent(hdc, hglrc)) {
        if (hglrc)
            wglDeleteContext(hglrc);
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        SetError(errorMessage, "Failed to create or activate offscreen OpenGL context.");
        return false;
    }

    m_hwnd = hwnd;
    m_hdc = hdc;
    m_hglrc = hglrc;
    return true;
}

void OffscreenWglContext::Shutdown()
{
    if (m_hglrc) {
        if (wglGetCurrentContext() == static_cast<HGLRC>(m_hglrc))
            wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(static_cast<HGLRC>(m_hglrc));
        m_hglrc = nullptr;
    }
    if (m_hdc && m_hwnd) {
        ReleaseDC(static_cast<HWND>(m_hwnd), static_cast<HDC>(m_hdc));
        m_hdc = nullptr;
    }
    if (m_hwnd) {
        DestroyWindow(static_cast<HWND>(m_hwnd));
        m_hwnd = nullptr;
    }
}

bool OffscreenWglContext::MakeCurrent() const
{
    if (!m_hdc || !m_hglrc)
        return false;
    return wglMakeCurrent(static_cast<HDC>(m_hdc), static_cast<HGLRC>(m_hglrc)) != FALSE;
}
