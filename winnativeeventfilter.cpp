/*
 * MIT License
 *
 * Copyright (C) 2020 by wangwenx190 (Yuhang Zhao)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "winnativeeventfilter.h"

#include <QDebug>
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
#include <QGuiApplication>
#else
#include <QCoreApplication>
#endif
#include <QLibrary>
#include <QOperatingSystemVersion>
#include <QTimer>
#include <cmath>
#include <windowsx.h>

#ifndef ABM_GETSTATE
// Only available since Windows XP
#define ABM_GETSTATE 0x00000004
#endif

#ifndef ABM_GETTASKBARPOS
// Only available since Windows XP
#define ABM_GETTASKBARPOS 0x00000005
#endif

#ifndef ABS_AUTOHIDE
// Only available since Windows XP
#define ABS_AUTOHIDE 0x0000001
#endif

#ifndef ABE_LEFT
// Only available since Windows XP
#define ABE_LEFT 0
#endif

#ifndef ABE_TOP
// Only available since Windows XP
#define ABE_TOP 1
#endif

#ifndef ABE_RIGHT
// Only available since Windows XP
#define ABE_RIGHT 2
#endif

#ifndef ABE_BOTTOM
// Only available since Windows XP
#define ABE_BOTTOM 3
#endif

#ifndef USER_DEFAULT_SCREEN_DPI
// Only available since Windows Vista
#define USER_DEFAULT_SCREEN_DPI 96
#endif

#ifndef SM_CXPADDEDBORDER
// Only available since Windows Vista
#define SM_CXPADDEDBORDER 92
#endif

#ifndef WM_NCUAHDRAWCAPTION
// Not documented, only available since Windows Vista
#define WM_NCUAHDRAWCAPTION 0x00AE
#endif

#ifndef WM_NCUAHDRAWFRAME
// Not documented, only available since Windows Vista
#define WM_NCUAHDRAWFRAME 0x00AF
#endif

#ifndef WM_DWMCOMPOSITIONCHANGED
// Only available since Windows Vista
#define WM_DWMCOMPOSITIONCHANGED 0x031E
#endif

#ifndef WM_DPICHANGED
// Only available since Windows 7
#define WM_DPICHANGED 0x02E0
#endif

#ifndef WNEF_GENERATE_WINAPI
#define WNEF_GENERATE_WINAPI(funcName, resultType, ...) \
    using _WNEF_WINAPI_##funcName = resultType (WINAPI *) (__VA_ARGS__); \
    _WNEF_WINAPI_##funcName m_lp##funcName = nullptr;
#endif

#ifndef WNEF_RESOLVE_WINAPI
#define WNEF_RESOLVE_WINAPI(libName, funcName)                                 \
    if (!m_lp##funcName) {                                                       \
    QLibrary library(QString::fromUtf8(#libName));                         \
    m_lp##funcName =                                                         \
    reinterpret_cast<_WNEF_WINAPI_##funcName>(library.resolve(#funcName));        \
    Q_ASSERT_X(m_lp##funcName, __FUNCTION__, qUtf8Printable(library.errorString()));\
    }
#endif

namespace {

using MONITOR_DPI_TYPE = enum _MONITOR_DPI_TYPE {
    MDT_EFFECTIVE_DPI = 0,
    MDT_ANGULAR_DPI = 1,
    MDT_RAW_DPI = 2,
    MDT_DEFAULT = MDT_EFFECTIVE_DPI
};

using DWMNCRENDERINGPOLICY = enum _DWMNCRENDERINGPOLICY { DWMNCRP_ENABLED = 2 };

using DWMWINDOWATTRIBUTE = enum _DWMWINDOWATTRIBUTE {
    DWMWA_NCRENDERING_POLICY = 2
};

using MARGINS = struct _MARGINS {
    int cxLeftWidth;
    int cxRightWidth;
    int cyTopHeight;
    int cyBottomHeight;
};

using APPBARDATA = struct _APPBARDATA {
    DWORD cbSize;
    HWND hWnd;
    UINT uCallbackMessage;
    UINT uEdge;
    RECT rc;
    LPARAM lParam;
};

WNEF_GENERATE_WINAPI(GetSystemDpiForProcess, UINT, HANDLE)
WNEF_GENERATE_WINAPI(GetDpiForWindow, UINT, HWND)
WNEF_GENERATE_WINAPI(GetDpiForSystem, UINT)
WNEF_GENERATE_WINAPI(GetSystemMetricsForDpi, int, int, UINT)
WNEF_GENERATE_WINAPI(GetDpiForMonitor, HRESULT, HMONITOR, MONITOR_DPI_TYPE, UINT *,
                     UINT *)
WNEF_GENERATE_WINAPI(DwmExtendFrameIntoClientArea, HRESULT, HWND, const MARGINS *)
WNEF_GENERATE_WINAPI(DwmIsCompositionEnabled, HRESULT, BOOL *)
WNEF_GENERATE_WINAPI(DwmSetWindowAttribute, HRESULT, HWND, DWORD, LPCVOID, DWORD)
WNEF_GENERATE_WINAPI(SHAppBarMessage, UINT_PTR, DWORD, APPBARDATA *)
WNEF_GENERATE_WINAPI(GetDeviceCaps, int, HDC, int)

const UINT m_defaultDotsPerInch = USER_DEFAULT_SCREEN_DPI;

const qreal m_defaultDevicePixelRatio = 1.0;

int m_borderWidth = -1, m_borderHeight = -1, m_titlebarHeight = -1;

QScopedPointer<WinNativeEventFilter> m_instance;

QVector<HWND> m_framelessWindows;

} // namespace

WinNativeEventFilter::WinNativeEventFilter() { initWin32Api(); }

WinNativeEventFilter::~WinNativeEventFilter() = default;

void WinNativeEventFilter::install() {
    if (m_instance.isNull()) {
        m_instance.reset(new WinNativeEventFilter);
        qApp->installNativeEventFilter(m_instance.data());
    }
}

void WinNativeEventFilter::uninstall() {
    if (!m_instance.isNull()) {
        qApp->removeNativeEventFilter(m_instance.data());
        m_instance.reset();
    }
    if (!m_framelessWindows.isEmpty()) {
        m_framelessWindows.clear();
    }
}

QVector<HWND> WinNativeEventFilter::framelessWindows() {
    return m_framelessWindows;
}

void WinNativeEventFilter::setFramelessWindows(QVector<HWND> windows) {
    if (!windows.isEmpty() && (windows != m_framelessWindows)) {
        m_framelessWindows = windows;
        install();
    }
}

void WinNativeEventFilter::addFramelessWindow(HWND window,
                                              const WINDOWDATA *data) {
    if (window && !m_framelessWindows.contains(window)) {
        m_framelessWindows.append(window);
        if (data) {
            createUserData(window, data);
        }
        install();
    }
}

void WinNativeEventFilter::removeFramelessWindow(HWND window) {
    if (window && m_framelessWindows.contains(window)) {
        m_framelessWindows.removeAll(window);
    }
}

void WinNativeEventFilter::clearFramelessWindows() {
    if (!m_framelessWindows.isEmpty()) {
        m_framelessWindows.clear();
    }
}

int WinNativeEventFilter::borderWidth(HWND handle) {
    if (handle) {
        createUserData(handle);
        const auto userData = reinterpret_cast<WINDOW *>(
            GetWindowLongPtrW(handle, GWLP_USERDATA));
        const int bw = userData->windowData.borderWidth;
        if (bw > 0) {
            return std::round(bw * getDevicePixelRatioForWindow(handle));
        }
    }
    if (m_borderWidth > 0) {
        return m_borderWidth;
    }
    return getSystemMetricsForWindow(handle, SM_CXFRAME) +
        getSystemMetricsForWindow(handle, SM_CXPADDEDBORDER);
}

int WinNativeEventFilter::borderHeight(HWND handle) {
    if (handle) {
        createUserData(handle);
        const auto userData = reinterpret_cast<WINDOW *>(
            GetWindowLongPtrW(handle, GWLP_USERDATA));
        const int bh = userData->windowData.borderHeight;
        if (bh > 0) {
            return std::round(bh * getDevicePixelRatioForWindow(handle));
        }
    }
    if (m_borderHeight > 0) {
        return m_borderHeight;
    }
    return getSystemMetricsForWindow(handle, SM_CYFRAME) +
        getSystemMetricsForWindow(handle, SM_CXPADDEDBORDER);
}

int WinNativeEventFilter::titlebarHeight(HWND handle) {
    if (handle) {
        createUserData(handle);
        const auto userData = reinterpret_cast<WINDOW *>(
            GetWindowLongPtrW(handle, GWLP_USERDATA));
        const int tbh = userData->windowData.titlebarHeight;
        if (tbh > 0) {
            return std::round(tbh * getDevicePixelRatioForWindow(handle));
        }
    }
    if (m_titlebarHeight > 0) {
        return m_titlebarHeight;
    }
    return borderHeight(handle) +
        getSystemMetricsForWindow(handle, SM_CYCAPTION);
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
bool WinNativeEventFilter::nativeEventFilter(const QByteArray &eventType,
                                             void *message, qintptr *result)
#else
bool WinNativeEventFilter::nativeEventFilter(const QByteArray &eventType,
                                             void *message, long *result)
#endif
{
    Q_UNUSED(eventType)
    const auto msg = static_cast<LPMSG>(message);
    if (!msg->hwnd) {
        // Why sometimes the window handle is null? Is it designed to be?
        // Anyway, we should skip it in this case.
        return false;
    }
    if (m_framelessWindows.isEmpty()) {
        // Only top level windows can be frameless.
        // Try to avoid this case because it will result in strange behavior, use addFramelessWindow if possible.
        const HWND parent = GetAncestor(msg->hwnd, GA_PARENT);
        if (parent && (parent != GetDesktopWindow())) {
            return false;
        }
    } else if (!m_framelessWindows.contains(msg->hwnd)) {
        return false;
    }
    createUserData(msg->hwnd);
    const auto data =
        reinterpret_cast<WINDOW *>(GetWindowLongPtrW(msg->hwnd, GWLP_USERDATA));
    if (!data->initialized) {
        data->initialized = TRUE;
        // The following two lines are necessary to remove the three system buttons (minimize, maximize and close),
        // but they will make the Acrylic (available since Win10 1709) unusable.
        SetWindowLongPtrW(msg->hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW | WS_EX_LAYERED);
        SetLayeredWindowAttributes(msg->hwnd, RGB(255, 0, 255), 0, LWA_COLORKEY);
        // Make sure our window have it's frame shadow.
        // The frame shadow is drawn by Desktop Window Manager (DWM), don't draw it yourself.
        // The frame shadow will get lost if DWM composition is disabled, it's designed to be,
        // don't force the window to draw a frame shadow in that case.
        // According to MSDN, DWM composition is always enabled and can't be disabled since Win8.
        handleDwmCompositionChanged(data);
        // For debug purposes.
        qDebug().noquote() << "Window handle:" << msg->hwnd;
        qDebug().noquote() << "Window DPI:" << getDotsPerInchForWindow(msg->hwnd)
                           << "Window DPR:"
                           << getDevicePixelRatioForWindow(msg->hwnd);
        qDebug().noquote() << "Window border width:" << borderWidth(msg->hwnd)
                           << "Window border height:" << borderHeight(msg->hwnd)
                           << "Window titlebar height:"
                           << titlebarHeight(msg->hwnd);
    }
    switch (msg->message) {
    case WM_NCCALCSIZE: {
        // Sent when the size and position of a window's client area must be calculated. By processing this message, an application can control the content of the window's client area when the size or position of the window changes.
        // If wParam is TRUE, lParam points to an NCCALCSIZE_PARAMS structure that contains information an application can use to calculate the new size and position of the client rectangle.
        // If wParam is FALSE, lParam points to a RECT structure. On entry, the structure contains the proposed window rectangle for the window. On exit, the structure should contain the screen coordinates of the corresponding window client area.
        auto &rect = static_cast<BOOL>(msg->wParam) ? (*reinterpret_cast<LPNCCALCSIZE_PARAMS>(msg->lParam)).rgrc[0] : *reinterpret_cast<LPRECT>(msg->lParam);
        if (IsMaximized(msg->hwnd)) {
            const HMONITOR windowMonitor =
                MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo;
            SecureZeroMemory(&monitorInfo, sizeof(monitorInfo));
            monitorInfo.cbSize = sizeof(monitorInfo);
            GetMonitorInfoW(windowMonitor, &monitorInfo);
            rect = monitorInfo.rcWork;
            // If the client rectangle is the same as the monitor's
            // rectangle, the shell assumes that the window has gone
            // fullscreen, so it removes the topmost attribute from any
            // auto-hide appbars, making them inaccessible. To avoid
            // this, reduce the size of the client area by one pixel on
            // a certain edge. The edge is chosen based on which side of
            // the monitor is likely to contain an auto-hide appbar, so
            // the missing client area is covered by it.
            if (EqualRect(&monitorInfo.rcWork, &monitorInfo.rcMonitor)) {
                APPBARDATA abd;
                SecureZeroMemory(&abd, sizeof(abd));
                abd.cbSize = sizeof(abd);
                const UINT taskbarState =
                    m_lpSHAppBarMessage(ABM_GETSTATE, &abd);
                if (taskbarState & ABS_AUTOHIDE) {
                    int edge = -1;
                    abd.hWnd =
                        FindWindowW(L"Shell_TrayWnd", nullptr);
                    if (abd.hWnd) {
                        const HMONITOR taskbarMonitor =
                            MonitorFromWindow(
                                abd.hWnd, MONITOR_DEFAULTTOPRIMARY);
                        if (taskbarMonitor &&
                            (taskbarMonitor == windowMonitor)) {
                            m_lpSHAppBarMessage(ABM_GETTASKBARPOS,
                                              &abd);
                            edge = abd.uEdge;
                        }
                    }
                    if (edge == ABE_BOTTOM) {
                        rect.bottom--;
                    } else if (edge == ABE_LEFT) {
                        rect.left++;
                    } else if (edge == ABE_TOP) {
                        rect.top++;
                    } else if (edge == ABE_RIGHT) {
                        rect.right--;
                    }
                }
            }
        }
        // If the wParam parameter is FALSE, the application should return zero.
        // If wParam is TRUE and an application returns zero, the old client area is preserved and is aligned with the upper-left corner of the new client area.
        // "*result = 0" removes the window frame (including the titlebar).
        // Don't use "*result = WVR_REDRAW", although it can also remove
        // the window frame, it will cause child widgets have strange behaviors.
        // "*result = 0" means we have processed this message and let Windows
        // ignore it to avoid Windows process this message again.
        // "return true" means we have filtered this message and let Qt ignore
        // it, in other words, it'll block Qt's own handling of this message,
        // so if you don't know what Qt does internally, don't block it.
        *result = 0;
        return true;
    }
    case WM_NCUAHDRAWCAPTION:
    case WM_NCUAHDRAWFRAME: {
        // These undocumented messages are sent to draw themed window
        // borders. Block them to prevent drawing borders over the client
        // area.
        *result = 0;
        return true;
    }
    case WM_NCPAINT: {
        if (data->dwmCompositionEnabled) {
            break;
        } else {
            // Only block WM_NCPAINT when composition is disabled. If it's
            // blocked when composition is enabled, the window shadow won't
            // be drawn.
            *result = 0;
            return true;
        }
    }
    case WM_NCACTIVATE: {
        // DefWindowProc won't repaint the window border if lParam (normally
        // a HRGN) is -1.
        *result = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, -1);
        return true;
    }
    case WM_NCHITTEST: {
        if (data->windowData.mouseTransparent) {
            // Mouse events will be passed to the parent window.
            *result = HTTRANSPARENT;
            return true;
        }
        const auto getHTResult = [](HWND _hWnd, LPARAM _lParam,
                                    const WINDOW *_data) -> LRESULT {
            const auto isInSpecificAreas = [](int x, int y,
                                              const QVector<QRect> &areas,
                                              qreal dpr) -> bool {
                for (auto &&area : qAsConst(areas)) {
                    if (!area.isValid()) {
                        continue;
                    }
                    if (QRect(std::round(area.x() * dpr), std::round(area.y() * dpr),
                              std::round(area.width() * dpr), std::round(area.height() * dpr))
                            .contains(x, y, true)) {
                        return true;
                    }
                }
                return false;
            };
            RECT clientRect = {0, 0, 0, 0};
            GetClientRect(_hWnd, &clientRect);
            const LONG ww = clientRect.right;
            const LONG wh = clientRect.bottom;
            POINT mouse;
            mouse.x = GET_X_LPARAM(_lParam);
            mouse.y = GET_Y_LPARAM(_lParam);
            ScreenToClient(_hWnd, &mouse);
            // These values are DPI-aware.
            const LONG bw = borderWidth(_hWnd);
            const LONG bh = borderHeight(_hWnd);
            const LONG tbh = titlebarHeight(_hWnd);
            const bool isInsideWindow = (mouse.x > 0) && (mouse.x < ww) &&
                (mouse.y > 0) && (mouse.y < wh);
            const qreal dpr = getDevicePixelRatioForWindow(_hWnd);
            const bool isTitlebar = isInsideWindow && (mouse.y < tbh) &&
                !isInSpecificAreas(mouse.x, mouse.y,
                                   _data->windowData.ignoreAreas, dpr) &&
                (_data->windowData.draggableAreas.isEmpty()
                     ? true
                     : isInSpecificAreas(mouse.x, mouse.y,
                                         _data->windowData.draggableAreas,
                                         dpr));
            if (IsMaximized(_hWnd)) {
                if (isTitlebar) {
                    return HTCAPTION;
                }
                return HTCLIENT;
            }
            if (_data->windowData.fixedSize) {
                if (isTitlebar) {
                    return HTCAPTION;
                } else {
                    // Un-resizeable border.
                    return HTBORDER;
                }
            } else {
                const bool isTop = isInsideWindow && (mouse.y < bh);
                const bool isBottom = isInsideWindow && (mouse.y > (wh - bh));
                // Make the border wider to let the user easy to resize on corners.
                const int factor = (isTop || isBottom) ? 2 : 1;
                const bool isLeft = isInsideWindow && (mouse.x < (bw * factor));
                const bool isRight = isInsideWindow && (mouse.x > (ww - (bw * factor)));
                if (isTop) {
                    if (isLeft) {
                        return HTTOPLEFT;
                    }
                    if (isRight) {
                        return HTTOPRIGHT;
                    }
                    return HTTOP;
                }
                if (isBottom) {
                    if (isLeft) {
                        return HTBOTTOMLEFT;
                    }
                    if (isRight) {
                        return HTBOTTOMRIGHT;
                    }
                    return HTBOTTOM;
                }
                if (isLeft) {
                    return HTLEFT;
                }
                if (isRight) {
                    return HTRIGHT;
                }
                if (isTitlebar) {
                    return HTCAPTION;
                }
            }
            return HTCLIENT;
        };
        *result = getHTResult(msg->hwnd, msg->lParam, data);
        return true;
    }
    case WM_GETMINMAXINFO: {
        // Don't cover the taskbar when maximized.
        const HMONITOR monitor =
            MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo;
        SecureZeroMemory(&monitorInfo, sizeof(monitorInfo));
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(monitor, &monitorInfo);
        const RECT rcWorkArea = monitorInfo.rcWork;
        const RECT rcMonitorArea = monitorInfo.rcMonitor;
        auto &mmi = *reinterpret_cast<LPMINMAXINFO>(msg->lParam);
        if (QOperatingSystemVersion::current() <
            QOperatingSystemVersion::Windows8) {
            // FIXME: Buggy on Windows 7:
            // The origin of coordinates is the top left edge of the
            // monitor's work area. Why? It should be the top left edge of
            // the monitor's area.
            mmi.ptMaxPosition.x = rcMonitorArea.left;
            mmi.ptMaxPosition.y = rcMonitorArea.top;
        } else {
            // Works fine on Windows 8/8.1/10
            mmi.ptMaxPosition.x =
                std::abs(rcWorkArea.left - rcMonitorArea.left);
            mmi.ptMaxPosition.y = std::abs(rcWorkArea.top - rcMonitorArea.top);
        }
        if (data->windowData.maximumSize.isEmpty()) {
            mmi.ptMaxSize.x = std::abs(rcWorkArea.right - rcWorkArea.left);
            mmi.ptMaxSize.y = std::abs(rcWorkArea.bottom - rcWorkArea.top);
        } else {
            mmi.ptMaxSize.x = std::round(getDevicePixelRatioForWindow(msg->hwnd) *
                data->windowData.maximumSize.width());
            mmi.ptMaxSize.y = std::round(getDevicePixelRatioForWindow(msg->hwnd) *
                data->windowData.maximumSize.height());
        }
        mmi.ptMaxTrackSize.x = mmi.ptMaxSize.x;
        mmi.ptMaxTrackSize.y = mmi.ptMaxSize.y;
        if (!data->windowData.minimumSize.isEmpty()) {
            mmi.ptMinTrackSize.x = std::round(getDevicePixelRatioForWindow(msg->hwnd) *
                data->windowData.minimumSize.width());
            mmi.ptMinTrackSize.y = std::round(getDevicePixelRatioForWindow(msg->hwnd) *
                data->windowData.minimumSize.height());
        }
        *result = 0;
        return true;
    }
    case WM_SETICON:
    case WM_SETTEXT: {
        // Disable painting while these messages are handled to prevent them
        // from drawing a window caption over the client area, but only when
        // composition is disabled. These messages don't paint
        // when composition is enabled and blocking WM_NCUAHDRAWCAPTION should
        // be enough to prevent painting when theming is enabled.
        if (!data->dwmCompositionEnabled) {
            const LONG_PTR oldStyle = GetWindowLongPtrW(msg->hwnd, GWL_STYLE);
            // Prevent Windows from drawing the default title bar by temporarily
            // toggling the WS_VISIBLE style.
            SetWindowLongPtrW(msg->hwnd, GWL_STYLE, oldStyle & ~WS_VISIBLE);
            const LRESULT ret = DefWindowProcW(msg->hwnd, msg->message,
                                               msg->wParam, msg->lParam);
            SetWindowLongPtrW(msg->hwnd, GWL_STYLE, oldStyle);
            *result = ret;
            return true;
        }
        break;
    }
    case WM_DWMCOMPOSITIONCHANGED: {
        handleDwmCompositionChanged(data);
        break;
    }
    case WM_THEMECHANGED:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        const HWND _hWnd = msg->hwnd;
        QTimer::singleShot(50, [this, _hWnd](){
            redrawWindow(_hWnd);
        });
        break;
    }
    case WM_DPICHANGED: {
        // Qt will do the scaling internally and automatically.
        // See: qt/qtbase/src/plugins/platforms/windows/qwindowscontext.cpp
        const auto dpiX = LOWORD(msg->wParam);
        const auto dpiY = HIWORD(msg->wParam);
        // dpiX and dpiY are identical. Just to silence a compiler warning.
        const auto dpi = dpiX == dpiY ? dpiY : dpiX;
        qDebug().noquote() << "Window DPI changed: new DPI -->" << dpi
                           << ", new DPR -->"
                           << getPreferedNumber(qreal(dpi) /
                                                qreal(m_defaultDotsPerInch));
        // Record the window handle now, don't use msg->hwnd directly because
        // when we finally execute the function, it has changed.
        const HWND _hWnd = msg->hwnd;
        // Wait some time for Qt to adjust the window size, but don't wait too
        // long, we want to refresh the window as soon as possible.
        // We can intercept Qt's handling of this message and resize the window
        // ourself, but after reading Qt's source code, I found that Qt does
        // more than resizing, so it's safer to let Qt do the scaling.
        QTimer::singleShot(50, [this, _hWnd]() {
            RECT rect = {0, 0, 0, 0};
            GetWindowRect(_hWnd, &rect);
            const int x = rect.left;
            const int y = rect.top;
            const int width = std::abs(rect.right - rect.left);
            const int height = std::abs(rect.bottom - rect.top);
            // Don't increase the window size too much, otherwise it would be
            // too obvious for the user and the experience is not good.
            MoveWindow(_hWnd, x, y, width + 1, height + 1, TRUE);
            // Re-paint the window after resizing.
            redrawWindow(_hWnd);
            // Restore and repaint.
            MoveWindow(_hWnd, x, y, width, height, TRUE);
            redrawWindow(_hWnd);
        });
        break;
    }
    default: {
        break;
    }
    }
    return false;
}

void WinNativeEventFilter::handleDwmCompositionChanged(WINDOW *data) {
    BOOL enabled = FALSE;
    m_lpDwmIsCompositionEnabled(&enabled);
    data->dwmCompositionEnabled = enabled;
    if (enabled) {
        // The frame shadow is drawn on the non-client area and thus we have
        // to make sure the non-client area rendering is enabled first.
        const DWMNCRENDERINGPOLICY ncrp = DWMNCRP_ENABLED;
        m_lpDwmSetWindowAttribute(data->hWnd, DWMWA_NCRENDERING_POLICY, &ncrp,
                                sizeof(ncrp));
        // Negative margins have special meaning to
        // DwmExtendFrameIntoClientArea. Negative margins create the "sheet
        // of glass" effect, where the client area is rendered as a solid
        // surface with no window border.
        const MARGINS margins = {-1, -1, -1, -1};
        m_lpDwmExtendFrameIntoClientArea(data->hWnd, &margins);
    }
    redrawWindow(data->hWnd);
}

UINT WinNativeEventFilter::getDotsPerInchForWindow(HWND handle) {
    const auto getScreenDpi = [](UINT defaultValue) -> UINT {
#if 0
        // Using Direct2D to get screen DPI. Available since Windows 7.
        ID2D1Factory *m_pDirect2dFactory = nullptr;
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                        &m_pDirect2dFactory)) &&
            m_pDirect2dFactory) {
            m_pDirect2dFactory->ReloadSystemMetrics();
            FLOAT dpiX = defaultValue, dpiY = defaultValue;
            m_pDirect2dFactory->GetDesktopDpi(&dpiX, &dpiY);
            // The values of *dpiX and *dpiY are identical.
            return dpiX;
        }
#endif
        // Available since Windows 2000.
        const HDC hdc = GetDC(nullptr);
        if (hdc) {
            const int dpiX = m_lpGetDeviceCaps(hdc, LOGPIXELSX);
            const int dpiY = m_lpGetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(nullptr, hdc);
            // The values of dpiX and dpiY are identical actually, just to
            // silence a compiler warning.
            return dpiX == dpiY ? dpiY : dpiX;
        }
        return defaultValue;
    };
    if (!handle) {
        if (m_lpGetSystemDpiForProcess) {
            return m_lpGetSystemDpiForProcess(GetCurrentProcess());
        } else if (m_lpGetDpiForSystem) {
            return m_lpGetDpiForSystem();
        } else {
            return getScreenDpi(m_defaultDotsPerInch);
        }
    }
    if (m_lpGetDpiForWindow) {
        return m_lpGetDpiForWindow(handle);
    }
    if (m_lpGetDpiForMonitor) {
        UINT dpiX = m_defaultDotsPerInch, dpiY = m_defaultDotsPerInch;
        m_lpGetDpiForMonitor(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST),
                           MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        // The values of *dpiX and *dpiY are identical.
        return dpiX;
    }
    return getScreenDpi(m_defaultDotsPerInch);
}

qreal WinNativeEventFilter::getDevicePixelRatioForWindow(HWND handle) {
    const qreal dpr = handle
        ? (qreal(getDotsPerInchForWindow(handle)) / qreal(m_defaultDotsPerInch))
        : m_defaultDevicePixelRatio;
    return getPreferedNumber(dpr);
}

int WinNativeEventFilter::getSystemMetricsForWindow(HWND handle, int index) {
    if (m_lpGetSystemMetricsForDpi) {
        return m_lpGetSystemMetricsForDpi(index,
                                        static_cast<UINT>(std::round(getPreferedNumber(
                                            getDotsPerInchForWindow(handle)))));
    } else {
        return std::round(GetSystemMetrics(index) * getDevicePixelRatioForWindow(handle));
    }
}

void WinNativeEventFilter::setWindowData(HWND window, const WINDOWDATA *data) {
    if (window && data) {
        createUserData(window, data);
    }
}

WinNativeEventFilter::WINDOWDATA *
WinNativeEventFilter::windowData(HWND window) {
    if (window) {
        createUserData(window);
        return &reinterpret_cast<WINDOW *>(
                    GetWindowLongPtrW(window, GWLP_USERDATA))
                    ->windowData;
    }
    return nullptr;
}

void WinNativeEventFilter::createUserData(HWND handle, const WINDOWDATA *data) {
    if (handle) {
        const auto userData = reinterpret_cast<WINDOW *>(
            GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (userData) {
            if (data) {
                userData->windowData = *data;
            }
        } else {
            WINDOW *_data = new WINDOW;
            _data->hWnd = handle;
            if (data) {
                _data->windowData = *data;
            }
            SetWindowLongPtrW(handle, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(_data));
        }
    }
}

void WinNativeEventFilter::initWin32Api() {
    // Available since Windows 2000.
    WNEF_RESOLVE_WINAPI(Gdi32, GetDeviceCaps)
    // Available since Windows XP.
    WNEF_RESOLVE_WINAPI(Shell32, SHAppBarMessage)
    // Available since Windows Vista.
    WNEF_RESOLVE_WINAPI(Dwmapi, DwmIsCompositionEnabled)
    WNEF_RESOLVE_WINAPI(Dwmapi, DwmExtendFrameIntoClientArea)
    WNEF_RESOLVE_WINAPI(Dwmapi, DwmSetWindowAttribute)
    if (QOperatingSystemVersion::current() >=
        QOperatingSystemVersion::Windows8_1) {
        WNEF_RESOLVE_WINAPI(SHCore, GetDpiForMonitor)
    }
    // Windows 10, version 1607 (10.0.14393)
    if (QOperatingSystemVersion::current() >=
        QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0,
                                14393)) {
        WNEF_RESOLVE_WINAPI(User32, GetDpiForWindow)
        WNEF_RESOLVE_WINAPI(User32, GetDpiForSystem)
        WNEF_RESOLVE_WINAPI(User32, GetSystemMetricsForDpi)
    }
    // Windows 10, version 1803 (10.0.17134)
    if (QOperatingSystemVersion::current() >=
        QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0,
                                17134)) {
        WNEF_RESOLVE_WINAPI(User32, GetSystemDpiForProcess)
    }
}

void WinNativeEventFilter::setBorderWidth(int bw) {
    if (m_borderWidth != bw) {
        m_borderWidth = bw;
    }
}

void WinNativeEventFilter::setBorderHeight(int bh) {
    if (m_borderHeight != bh) {
        m_borderHeight = bh;
    }
}

void WinNativeEventFilter::setTitlebarHeight(int tbh) {
    if (m_titlebarHeight != tbh) {
        m_titlebarHeight = tbh;
    }
}

qreal WinNativeEventFilter::getPreferedNumber(qreal num) {
    qreal result = -1.0;
    const auto getRoundedNumber = [](qreal in) -> qreal {
        // If the given number is not very large, we assume it's a
        // device pixel ratio (DPR), otherwise we assume it's a DPI.
        if (in < m_defaultDotsPerInch) {
            return std::round(in);
        } else {
            if (in < (m_defaultDotsPerInch * 1.5)) {
                return m_defaultDotsPerInch;
            } else if (in == (m_defaultDotsPerInch * 1.5)) {
                return m_defaultDotsPerInch * 1.5;
            } else if (in < (m_defaultDotsPerInch * 2.5)) {
                return m_defaultDotsPerInch * 2;
            } else if (in == (m_defaultDotsPerInch * 2.5)) {
                return m_defaultDotsPerInch * 2.5;
            } else if (in < (m_defaultDotsPerInch * 3.5)) {
                return m_defaultDotsPerInch * 3;
            } else if (in == (m_defaultDotsPerInch * 3.5)) {
                return m_defaultDotsPerInch * 3.5;
            } else if (in < (m_defaultDotsPerInch * 4.5)) {
                return m_defaultDotsPerInch * 4;
            } else {
                qWarning().noquote()
                    << "DPI too large:" << static_cast<int>(in);
            }
        }
        return -1.0;
    };
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    switch (QGuiApplication::highDpiScaleFactorRoundingPolicy()) {
    case Qt::HighDpiScaleFactorRoundingPolicy::PassThrough:
        // Default behavior for Qt 6.
        result = num;
        break;
    case Qt::HighDpiScaleFactorRoundingPolicy::Floor:
        result = std::floor(num);
        break;
    case Qt::HighDpiScaleFactorRoundingPolicy::Ceil:
        result = std::ceil(num);
        break;
    default:
        // Default behavior for Qt 5.6 to 5.15
        result = getRoundedNumber(num);
        break;
    }
#else
    // Default behavior for Qt 5.6 to 5.15
    result = getRoundedNumber(num);
#endif
    return result;
}

void WinNativeEventFilter::redrawWindow(HWND handle)
{
    if (handle) {
        RedrawWindow(handle, nullptr, nullptr,
                            RDW_INVALIDATE | RDW_NOERASE | RDW_NOINTERNALPAINT |
                                RDW_ERASENOW | RDW_ALLCHILDREN);
    }
}
