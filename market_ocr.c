#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NORM_W 20
#define NORM_H 30
#define NORM_PIXELS (NORM_W * NORM_H)
#define MAX_TEMPLATES 16
#define MAX_SEGMENTS 64
#define CONFIG_FILE "ocr_config.txt"

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Region;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Box;

typedef struct {
    char ch;
    unsigned char bits[NORM_PIXELS];
} GlyphTemplate;

typedef struct {
    Region region;
    int threshold_bias;
    int template_count;
    GlyphTemplate templates[MAX_TEMPLATES];
} OCRModel;

typedef struct {
    OCRModel price;
    OCRModel last_lot;
    int use_window;
    char window_title[256];
    char window_class[128];
    int base_client_w;
    int base_client_h;
    POINT render_point;
    int render_point_set;
    int overlay_enabled;
} OCRConfig;

typedef struct {
    HBITMAP screenshot;
    int vx;
    int vy;
    int vw;
    int vh;
    int dragging;
    int has_rect;
    int done;
    int canceled;
    POINT p0;
    POINT p1;
    Region result;
} SelectorState;

static OCRConfig g_config;
static const char* g_selector_class = "OCRSelectorWindowClass";
static const char* g_point_selector_class = "OCRPointSelectorWindowClass";
static const char* g_overlay_class = "OCROverlayWindowClass";
static HWND g_target_hwnd = NULL;
static HWND g_overlay_hwnd = NULL;
static HFONT g_overlay_font = NULL;
static Region g_overlay_price = {0};
static Region g_overlay_last = {0};
static int g_overlay_has_price = 0;
static int g_overlay_has_last = 0;
static POINT g_overlay_text_point = {0, 0};
static int g_overlay_has_point = 0;
static char g_overlay_text[64] = "";

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int file_exists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void trim_newline(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static void configure_console_runtime(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, mode);
    }
}

static HWND resolve_target_window(void) {
    HWND h;
    if (!g_config.use_window) return NULL;

    if (g_target_hwnd && IsWindow(g_target_hwnd)) {
        return g_target_hwnd;
    }

    h = NULL;
    if (g_config.window_class[0] || g_config.window_title[0]) {
        h = FindWindowA(
            g_config.window_class[0] ? g_config.window_class : NULL,
            g_config.window_title[0] ? g_config.window_title : NULL
        );
    }
    if (!h && g_config.window_class[0]) {
        h = FindWindowA(g_config.window_class, NULL);
    }
    if (!h && g_config.window_title[0]) {
        h = FindWindowA(NULL, g_config.window_title);
    }
    if (h && IsWindow(h)) {
        g_target_hwnd = h;
        return h;
    }
    return NULL;
}

static int get_target_window_rect(RECT* out_rc) {
    HWND h = resolve_target_window();
    if (!h) return 0;
    return GetWindowRect(h, out_rc) ? 1 : 0;
}

static int get_target_client_rect_screen(RECT* out_rc) {
    HWND h = resolve_target_window();
    RECT cr;
    POINT p0, p1;
    if (!h) return 0;
    if (!GetClientRect(h, &cr)) return 0;
    p0.x = cr.left;
    p0.y = cr.top;
    p1.x = cr.right;
    p1.y = cr.bottom;
    if (!ClientToScreen(h, &p0)) return 0;
    if (!ClientToScreen(h, &p1)) return 0;
    out_rc->left = p0.x;
    out_rc->top = p0.y;
    out_rc->right = p1.x;
    out_rc->bottom = p1.y;
    return 1;
}

static int get_effective_region(const Region* in_model_region, Region* out_capture_region) {
    if (!g_config.use_window) {
        *out_capture_region = *in_model_region;
        return 1;
    }

    {
        RECT cr;
        int cw;
        int ch;
        int bw = g_config.base_client_w;
        int bh = g_config.base_client_h;
        if (!get_target_client_rect_screen(&cr)) return 0;
        cw = cr.right - cr.left;
        ch = cr.bottom - cr.top;
        if (cw <= 0 || ch <= 0) return 0;
        if (bw <= 0 || bh <= 0) {
            bw = cw;
            bh = ch;
            g_config.base_client_w = bw;
            g_config.base_client_h = bh;
        }
        out_capture_region->x = cr.left + (int)((double)in_model_region->x * cw / bw + 0.5);
        out_capture_region->y = cr.top + (int)((double)in_model_region->y * ch / bh + 0.5);
        out_capture_region->w = (int)((double)in_model_region->w * cw / bw + 0.5);
        out_capture_region->h = (int)((double)in_model_region->h * ch / bh + 0.5);
        if (out_capture_region->w < 1) out_capture_region->w = 1;
        if (out_capture_region->h < 1) out_capture_region->h = 1;
    }
    return 1;
}

static int get_effective_point(const POINT* in_model_point, POINT* out_capture_point) {
    if (!g_config.use_window) {
        *out_capture_point = *in_model_point;
        return 1;
    }
    {
        RECT cr;
        int cw;
        int ch;
        int bw = g_config.base_client_w;
        int bh = g_config.base_client_h;
        if (!get_target_client_rect_screen(&cr)) return 0;
        cw = cr.right - cr.left;
        ch = cr.bottom - cr.top;
        if (cw <= 0 || ch <= 0) return 0;
        if (bw <= 0 || bh <= 0) {
            bw = cw;
            bh = ch;
            g_config.base_client_w = bw;
            g_config.base_client_h = bh;
        }
        out_capture_point->x = cr.left + (int)((double)in_model_point->x * cw / bw + 0.5);
        out_capture_point->y = cr.top + (int)((double)in_model_point->y * ch / bh + 0.5);
    }
    return 1;
}

static void store_selected_window(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    RECT cr;
    if (!root) root = hwnd;
    g_target_hwnd = root;
    g_config.use_window = 1;
    g_config.window_title[0] = '\0';
    g_config.window_class[0] = '\0';
    GetWindowTextA(root, g_config.window_title, (int)sizeof(g_config.window_title));
    GetClassNameA(root, g_config.window_class, (int)sizeof(g_config.window_class));
    if (get_target_client_rect_screen(&cr)) {
        g_config.base_client_w = cr.right - cr.left;
        g_config.base_client_h = cr.bottom - cr.top;
    }
}

static int select_target_window_by_click(void) {
    POINT p;
    int was_using_window = g_config.use_window;
    printf("Move mouse over target window and click LEFT mouse button.\n");
    printf("Press ESC to cancel.\n");
    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 1) {
            printf("Window selection canceled.\n");
            return 0;
        }
        if (GetAsyncKeyState(VK_LBUTTON) & 1) {
            if (!GetCursorPos(&p)) return 0;
            {
                HWND h = WindowFromPoint(p);
                if (!h) {
                    printf("No window at cursor.\n");
                    return 0;
                }
                store_selected_window(h);
                if (!was_using_window) {
                    RECT cr;
                    if (get_target_client_rect_screen(&cr)) {
                        g_config.price.region.x -= cr.left;
                        g_config.price.region.y -= cr.top;
                        g_config.last_lot.region.x -= cr.left;
                        g_config.last_lot.region.y -= cr.top;
                    }
                }
                printf("Selected window: title='%s', class='%s'\n", g_config.window_title, g_config.window_class);
                return 1;
            }
        }
        Sleep(20);
    }
}

static void binarize_text(const unsigned char* gray, int w, int h, int threshold_bias, unsigned char* bin_out) {
    int i;
    int mn = 255;
    int mx = 0;
    int threshold;
    int ones = 0;

    for (i = 0; i < w * h; i++) {
        if (gray[i] < mn) mn = gray[i];
        if (gray[i] > mx) mx = gray[i];
    }

    threshold = ((mn + mx) / 2) + threshold_bias;
    threshold = clamp_int(threshold, 0, 255);

    for (i = 0; i < w * h; i++) {
        bin_out[i] = (gray[i] >= threshold) ? 1 : 0;
        ones += bin_out[i];
    }

    if (ones > (w * h) / 2) {
        for (i = 0; i < w * h; i++) {
            bin_out[i] = (unsigned char)(1 - bin_out[i]);
        }
    }
}

static int segment_glyphs(const unsigned char* bin, int w, int h, Box* out_boxes, int max_boxes) {
    int x;
    int count = 0;

    for (x = 0; x < w;) {
        int y;
        int has = 0;
        for (y = 0; y < h; y++) {
            if (bin[y * w + x]) {
                has = 1;
                break;
            }
        }
        if (!has) {
            x++;
            continue;
        }

        {
            int x0 = x;
            int x1;
            int top = h - 1;
            int bottom = 0;
            int xx;

            while (x < w) {
                int col_has = 0;
                for (y = 0; y < h; y++) {
                    if (bin[y * w + x]) {
                        col_has = 1;
                        break;
                    }
                }
                if (!col_has) break;
                x++;
            }
            x1 = x - 1;

            for (xx = x0; xx <= x1; xx++) {
                for (y = 0; y < h; y++) {
                    if (bin[y * w + xx]) {
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                    }
                }
            }

            if (count < max_boxes && x1 >= x0 && bottom >= top) {
                out_boxes[count].x = x0;
                out_boxes[count].y = top;
                out_boxes[count].w = x1 - x0 + 1;
                out_boxes[count].h = bottom - top + 1;
                count++;
            }
        }
    }
    return count;
}

static void normalize_box_to_template(const unsigned char* bin, int img_w, const Box* b, unsigned char* out_bits) {
    int y;
    for (y = 0; y < NORM_H; y++) {
        int x;
        int sy = b->y + (y * b->h) / NORM_H;
        if (sy >= b->y + b->h) sy = b->y + b->h - 1;
        for (x = 0; x < NORM_W; x++) {
            int sx = b->x + (x * b->w) / NORM_W;
            if (sx >= b->x + b->w) sx = b->x + b->w - 1;
            out_bits[y * NORM_W + x] = bin[sy * img_w + sx] ? 1 : 0;
        }
    }
}

static int template_distance(const unsigned char* a, const unsigned char* b) {
    int i;
    int d = 0;
    for (i = 0; i < NORM_PIXELS; i++) {
        d += (a[i] != b[i]) ? 1 : 0;
    }
    return d;
}

static int capture_region_gray(const Region* r, unsigned char** out_gray, int* out_w, int* out_h) {
    HDC hdc_screen = NULL;
    HDC hdc_mem = NULL;
    HBITMAP hbm = NULL;
    BITMAPINFO bi;
    unsigned int* pixels = NULL;
    unsigned char* gray = NULL;
    int i;
    int ok = 0;

    if (r->w <= 0 || r->h <= 0) return 0;

    hdc_screen = GetDC(NULL);
    if (!hdc_screen) goto cleanup;
    hdc_mem = CreateCompatibleDC(hdc_screen);
    if (!hdc_mem) goto cleanup;
    hbm = CreateCompatibleBitmap(hdc_screen, r->w, r->h);
    if (!hbm) goto cleanup;

    SelectObject(hdc_mem, hbm);
    if (!BitBlt(hdc_mem, 0, 0, r->w, r->h, hdc_screen, r->x, r->y, SRCCOPY)) goto cleanup;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = r->w;
    bi.bmiHeader.biHeight = -r->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    pixels = (unsigned int*)malloc((size_t)r->w * (size_t)r->h * sizeof(unsigned int));
    if (!pixels) goto cleanup;
    if (!GetDIBits(hdc_mem, hbm, 0, (UINT)r->h, pixels, &bi, DIB_RGB_COLORS)) goto cleanup;

    gray = (unsigned char*)malloc((size_t)r->w * (size_t)r->h);
    if (!gray) goto cleanup;

    for (i = 0; i < r->w * r->h; i++) {
        unsigned int p = pixels[i];
        unsigned char b = (unsigned char)(p & 0xFF);
        unsigned char g = (unsigned char)((p >> 8) & 0xFF);
        unsigned char rr = (unsigned char)((p >> 16) & 0xFF);
        gray[i] = (unsigned char)((30 * rr + 59 * g + 11 * b) / 100);
    }

    *out_gray = gray;
    *out_w = r->w;
    *out_h = r->h;
    gray = NULL;
    ok = 1;

cleanup:
    if (gray) free(gray);
    if (pixels) free(pixels);
    if (hbm) DeleteObject(hbm);
    if (hdc_mem) DeleteDC(hdc_mem);
    if (hdc_screen) ReleaseDC(NULL, hdc_screen);
    return ok;
}

static int capture_virtual_screen_bitmap(HBITMAP* out_bmp, int* out_vx, int* out_vy, int* out_vw, int* out_vh) {
    HDC hdc_screen = NULL;
    HDC hdc_mem = NULL;
    HBITMAP hbm = NULL;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int ok = 0;

    if (vw <= 0 || vh <= 0) return 0;

    hdc_screen = GetDC(NULL);
    if (!hdc_screen) goto cleanup;
    hdc_mem = CreateCompatibleDC(hdc_screen);
    if (!hdc_mem) goto cleanup;
    hbm = CreateCompatibleBitmap(hdc_screen, vw, vh);
    if (!hbm) goto cleanup;

    SelectObject(hdc_mem, hbm);
    if (!BitBlt(hdc_mem, 0, 0, vw, vh, hdc_screen, vx, vy, SRCCOPY)) goto cleanup;

    *out_bmp = hbm;
    *out_vx = vx;
    *out_vy = vy;
    *out_vw = vw;
    *out_vh = vh;
    hbm = NULL;
    ok = 1;

cleanup:
    if (hbm) DeleteObject(hbm);
    if (hdc_mem) DeleteDC(hdc_mem);
    if (hdc_screen) ReleaseDC(NULL, hdc_screen);
    return ok;
}

static void selector_rect_client(const SelectorState* s, RECT* rc) {
    rc->left = (s->p0.x < s->p1.x) ? s->p0.x : s->p1.x;
    rc->top = (s->p0.y < s->p1.y) ? s->p0.y : s->p1.y;
    rc->right = (s->p0.x > s->p1.x) ? s->p0.x : s->p1.x;
    rc->bottom = (s->p0.y > s->p1.y) ? s->p0.y : s->p1.y;
}

static LRESULT CALLBACK selector_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SelectorState* s = (SelectorState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (s) {
            s->dragging = 1;
            s->has_rect = 1;
            s->p0.x = GET_X_LPARAM(lParam);
            s->p0.y = GET_Y_LPARAM(lParam);
            s->p1 = s->p0;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s && s->dragging) {
            s->p1.x = GET_X_LPARAM(lParam);
            s->p1.y = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s && s->dragging) {
            s->p1.x = GET_X_LPARAM(lParam);
            s->p1.y = GET_Y_LPARAM(lParam);
            s->dragging = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (!s) return 0;
        if (wParam == VK_ESCAPE) {
            s->canceled = 1;
            s->done = 1;
            DestroyWindow(hwnd);
        } else if (wParam == VK_RETURN) {
            if (s->has_rect) {
                RECT rc;
                selector_rect_client(s, &rc);
                if (rc.right > rc.left && rc.bottom > rc.top) {
                    s->result.x = s->vx + rc.left;
                    s->result.y = s->vy + rc.top;
                    s->result.w = rc.right - rc.left + 1;
                    s->result.h = rc.bottom - rc.top + 1;
                    s->done = 1;
                    DestroyWindow(hwnd);
                }
            }
        }
        return 0;
    case WM_PAINT:
        if (s) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ oldbmp = SelectObject(mem, s->screenshot);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 80, 80));
            HGDIOBJ oldpen;
            HGDIOBJ oldbrush;
            RECT rc;
            const char* hint = "Drag mouse to select region. ENTER = confirm, ESC = cancel";

            BitBlt(hdc, 0, 0, s->vw, s->vh, mem, 0, 0, SRCCOPY);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            TextOutA(hdc, 20, 20, hint, (int)strlen(hint));

            if (s->has_rect) {
                selector_rect_client(s, &rc);
                oldpen = SelectObject(hdc, pen);
                oldbrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(hdc, oldbrush);
                SelectObject(hdc, oldpen);
            }

            SelectObject(mem, oldbmp);
            DeleteObject(pen);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static int select_region_from_screenshot(Region* out_region) {
    HBITMAP bmp = NULL;
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    SelectorState st;
    int vx, vy, vw, vh;
    HINSTANCE hinst = GetModuleHandleA(NULL);
    static int class_registered = 0;

    if (!capture_virtual_screen_bitmap(&bmp, &vx, &vy, &vw, &vh)) {
        printf("Failed to capture screenshot.\n");
        return 0;
    }

    if (!class_registered) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = selector_wnd_proc;
        wc.hInstance = hinst;
        wc.lpszClassName = g_selector_class;
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        if (!RegisterClassA(&wc)) {
            DeleteObject(bmp);
            printf("Failed to register selector window class.\n");
            return 0;
        }
        class_registered = 1;
    }

    memset(&st, 0, sizeof(st));
    st.screenshot = bmp;
    st.vx = vx;
    st.vy = vy;
    st.vw = vw;
    st.vh = vh;

    hwnd = CreateWindowExA(
        WS_EX_TOPMOST,
        g_selector_class,
        "Select OCR Region",
        WS_POPUP | WS_VISIBLE,
        st.vx, st.vy, st.vw, st.vh,
        NULL,
        NULL,
        hinst,
        &st
    );

    if (!hwnd) {
        DeleteObject(bmp);
        printf("Failed to create selector window.\n");
        return 0;
    }

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    while (!st.done && IsWindow(hwnd)) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                st.canceled = 1;
                st.done = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (st.done) break;
        }
        Sleep(1);
    }

    DeleteObject(bmp);
    if (st.done && !st.canceled && st.result.w > 0 && st.result.h > 0) {
        *out_region = st.result;
        return 1;
    }
    printf("Region selection canceled.\n");
    return 0;
}

typedef struct {
    HBITMAP screenshot;
    int vx;
    int vy;
    int vw;
    int vh;
    int done;
    int canceled;
    int has_point;
    POINT point;
} PointSelectorState;

static LRESULT CALLBACK point_selector_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PointSelectorState* s = (PointSelectorState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lParam;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (s) {
            s->point.x = s->vx + GET_X_LPARAM(lParam);
            s->point.y = s->vy + GET_Y_LPARAM(lParam);
            s->has_point = 1;
            s->done = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_KEYDOWN:
        if (s && wParam == VK_ESCAPE) {
            s->canceled = 1;
            s->done = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_PAINT:
        if (s) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ oldbmp = SelectObject(mem, s->screenshot);
            const char* hint = "Click to set render point. ESC = cancel";
            BitBlt(hdc, 0, 0, s->vw, s->vh, mem, 0, 0, SRCCOPY);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            TextOutA(hdc, 20, 20, hint, (int)strlen(hint));
            SelectObject(mem, oldbmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
        }
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static int select_point_from_screenshot(POINT* out_point) {
    HBITMAP bmp = NULL;
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    PointSelectorState st;
    int vx, vy, vw, vh;
    HINSTANCE hinst = GetModuleHandleA(NULL);
    static int class_registered = 0;

    if (!capture_virtual_screen_bitmap(&bmp, &vx, &vy, &vw, &vh)) {
        printf("Failed to capture screenshot.\n");
        return 0;
    }

    if (!class_registered) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = point_selector_wnd_proc;
        wc.hInstance = hinst;
        wc.lpszClassName = g_point_selector_class;
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        if (!RegisterClassA(&wc)) {
            DeleteObject(bmp);
            printf("Failed to register point selector class.\n");
            return 0;
        }
        class_registered = 1;
    }

    memset(&st, 0, sizeof(st));
    st.screenshot = bmp;
    st.vx = vx;
    st.vy = vy;
    st.vw = vw;
    st.vh = vh;

    hwnd = CreateWindowExA(
        WS_EX_TOPMOST,
        g_point_selector_class,
        "Select Render Point",
        WS_POPUP | WS_VISIBLE,
        st.vx, st.vy, st.vw, st.vh,
        NULL, NULL, hinst, &st
    );
    if (!hwnd) {
        DeleteObject(bmp);
        printf("Failed to create point selector window.\n");
        return 0;
    }
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    while (!st.done && IsWindow(hwnd)) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                st.canceled = 1;
                st.done = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (st.done) break;
        }
        Sleep(1);
    }

    DeleteObject(bmp);
    if (st.done && !st.canceled && st.has_point) {
        *out_point = st.point;
        return 1;
    }
    printf("Point selection canceled.\n");
    return 0;
}

static RECT expand_rect_pixels(const Region* r, int pad) {
    RECT rc;
    rc.left = r->x - pad;
    rc.top = r->y - pad;
    rc.right = r->x + r->w + pad;
    rc.bottom = r->y + r->h + pad;
    return rc;
}

static LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    (void)lParam;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HPEN pen_price = CreatePen(PS_SOLID, 2, RGB(0, 220, 255));
        HPEN pen_last = CreatePen(PS_SOLID, 2, RGB(255, 160, 0));
        HGDIOBJ old_pen = SelectObject(hdc, pen_price);
        HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        /* Transparent background via colorkey; fill black */
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));

        if (g_overlay_has_price) {
            RECT rc = expand_rect_pixels(&g_overlay_price, 3);
            SelectObject(hdc, pen_price);
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        }
        if (g_overlay_has_last) {
            RECT rc = expand_rect_pixels(&g_overlay_last, 3);
            SelectObject(hdc, pen_last);
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        }
        if (g_overlay_has_point && g_overlay_text[0]) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 210, 120));
            if (g_overlay_font) SelectObject(hdc, g_overlay_font);
            TextOutA(hdc, g_overlay_text_point.x, g_overlay_text_point.y, g_overlay_text, (int)strlen(g_overlay_text));
        }

        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen_price);
        DeleteObject(pen_last);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static void overlay_hide(void) {
    if (g_overlay_hwnd && IsWindow(g_overlay_hwnd)) {
        ShowWindow(g_overlay_hwnd, SW_HIDE);
    }
}

static int overlay_ensure(void) {
    WNDCLASSA wc;
    HINSTANCE hinst = GetModuleHandleA(NULL);
    static int class_registered = 0;
    int vx, vy, vw, vh;

    if (!class_registered) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = overlay_wnd_proc;
        wc.hInstance = hinst;
        wc.lpszClassName = g_overlay_class;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClassA(&wc)) return 0;
        class_registered = 1;
    }

    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (!g_overlay_hwnd || !IsWindow(g_overlay_hwnd)) {
        g_overlay_hwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            g_overlay_class,
            "OCR Overlay",
            WS_POPUP,
            vx, vy, vw, vh,
            NULL, NULL, hinst, NULL
        );
        if (!g_overlay_hwnd) return 0;
        SetLayeredWindowAttributes(g_overlay_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    } else {
        SetWindowPos(g_overlay_hwnd, HWND_TOPMOST, vx, vy, vw, vh, SWP_NOACTIVATE);
    }

    if (!g_overlay_font) {
        HDC hdc = GetDC(NULL);
        int h = -MulDiv(16, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        ReleaseDC(NULL, hdc);
        g_overlay_font = CreateFontA(
            h, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Comic Sans MS"
        );
    }

    ShowWindow(g_overlay_hwnd, SW_SHOWNOACTIVATE);
    return 1;
}

static void overlay_update(const Region* price_r, const Region* last_r, const POINT* text_pt, const char* text) {
    if (!g_config.overlay_enabled) {
        overlay_hide();
        return;
    }
    if (!overlay_ensure()) return;
    g_overlay_has_price = (price_r && price_r->w > 0 && price_r->h > 0) ? 1 : 0;
    g_overlay_has_last = (last_r && last_r->w > 0 && last_r->h > 0) ? 1 : 0;
    if (g_overlay_has_price) g_overlay_price = *price_r;
    if (g_overlay_has_last) g_overlay_last = *last_r;
    g_overlay_has_point = (text_pt != NULL) ? 1 : 0;
    if (g_overlay_has_point) g_overlay_text_point = *text_pt;
    if (text) {
        strncpy(g_overlay_text, text, sizeof(g_overlay_text) - 1);
        g_overlay_text[sizeof(g_overlay_text) - 1] = '\0';
    } else {
        g_overlay_text[0] = '\0';
    }
    InvalidateRect(g_overlay_hwnd, NULL, TRUE);
}

static int load_png_gray_wic(const char* path, unsigned char** out_gray, int* out_w, int* out_h) {
    wchar_t wpath[MAX_PATH];
    IWICImagingFactory* factory = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    unsigned char* rgba = NULL;
    unsigned char* gray = NULL;
    UINT w = 0;
    UINT h = 0;
    UINT stride;
    UINT bytes;
    HRESULT hr;
    int ok = 0;
    UINT i;

    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH) <= 0) return 0;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (LPVOID*)&factory);
    if (FAILED(hr)) goto cleanup;

    hr = IWICImagingFactory_CreateDecoderFromFilename(factory, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) goto cleanup;

    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) goto cleanup;

    hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) goto cleanup;

    hr = IWICFormatConverter_Initialize(
        converter,
        (IWICBitmapSource*)frame,
        &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        NULL,
        0.0,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) goto cleanup;

    hr = IWICFormatConverter_GetSize(converter, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) goto cleanup;

    stride = w * 4;
    bytes = stride * h;
    rgba = (unsigned char*)malloc(bytes);
    if (!rgba) goto cleanup;

    hr = IWICFormatConverter_CopyPixels(converter, NULL, stride, bytes, rgba);
    if (FAILED(hr)) goto cleanup;

    gray = (unsigned char*)malloc((size_t)w * (size_t)h);
    if (!gray) goto cleanup;

    for (i = 0; i < w * h; i++) {
        unsigned char b = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char r = rgba[i * 4 + 2];
        gray[i] = (unsigned char)((30 * r + 59 * g + 11 * b) / 100);
    }

    *out_gray = gray;
    *out_w = (int)w;
    *out_h = (int)h;
    gray = NULL;
    ok = 1;

cleanup:
    if (gray) free(gray);
    if (rgba) free(rgba);
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
    return ok;
}

static int add_template_from_png(OCRModel* model, char ch, const char* path) {
    unsigned char* gray = NULL;
    unsigned char* bin = NULL;
    Box boxes[MAX_SEGMENTS];
    int w = 0;
    int h = 0;
    int count;
    int best = -1;
    int best_area = -1;
    int i;
    unsigned char norm[NORM_PIXELS];

    if (model->template_count >= MAX_TEMPLATES) return 0;
    if (!load_png_gray_wic(path, &gray, &w, &h)) {
        printf("Failed to load PNG: %s\n", path);
        return 0;
    }

    bin = (unsigned char*)malloc((size_t)w * (size_t)h);
    if (!bin) {
        free(gray);
        return 0;
    }

    binarize_text(gray, w, h, model->threshold_bias, bin);
    count = segment_glyphs(bin, w, h, boxes, MAX_SEGMENTS);
    if (count <= 0) {
        printf("No glyph detected in: %s\n", path);
        free(bin);
        free(gray);
        return 0;
    }

    for (i = 0; i < count; i++) {
        int area = boxes[i].w * boxes[i].h;
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }

    normalize_box_to_template(bin, w, &boxes[best], norm);
    model->templates[model->template_count].ch = ch;
    memcpy(model->templates[model->template_count].bits, norm, NORM_PIXELS);
    model->template_count++;

    free(bin);
    free(gray);
    return 1;
}

static int load_templates_from_folder(OCRModel* model, const char* folder) {
    OCRModel tmp = *model;
    char path[MAX_PATH];
    int d;
    int has_decimal = 0;

    tmp.template_count = 0;

    for (d = 0; d <= 9; d++) {
        snprintf(path, sizeof(path), "%s\\%d.png", folder, d);
        if (!file_exists(path)) {
            printf("Missing template file: %s\n", path);
            return 0;
        }
        if (!add_template_from_png(&tmp, (char)('0' + d), path)) {
            return 0;
        }
    }

    snprintf(path, sizeof(path), "%s\\dot.png", folder);
    if (file_exists(path)) {
        if (!add_template_from_png(&tmp, '.', path)) return 0;
        has_decimal = 1;
    }

    snprintf(path, sizeof(path), "%s\\comma.png", folder);
    if (file_exists(path)) {
        if (!add_template_from_png(&tmp, ',', path)) return 0;
        has_decimal = 1;
    }

    if (!has_decimal) {
        printf("Missing decimal separator template. Add dot.png or comma.png\n");
        return 0;
    }

    *model = tmp;
    return 1;
}

static int recognize_text(const OCRModel* model, char* out_text, size_t out_size) {
    unsigned char* gray = NULL;
    unsigned char* bin = NULL;
    Region capture_region;
    Box boxes[MAX_SEGMENTS];
    int w = 0;
    int h = 0;
    int count;
    int i;
    size_t pos = 0;
    int ok = 0;

    if (out_size == 0 || model->template_count <= 0) return 0;
    out_text[0] = '\0';

    if (!get_effective_region(&model->region, &capture_region)) return 0;
    if (!capture_region_gray(&capture_region, &gray, &w, &h)) return 0;
    bin = (unsigned char*)malloc((size_t)w * (size_t)h);
    if (!bin) goto cleanup;

    binarize_text(gray, w, h, model->threshold_bias, bin);
    count = segment_glyphs(bin, w, h, boxes, MAX_SEGMENTS);
    if (count <= 0) goto cleanup;

    for (i = 0; i < count; i++) {
        unsigned char norm[NORM_PIXELS];
        int best_idx = -1;
        int best_dist = 1000000;
        int t;

        if (pos + 1 >= out_size) break;
        normalize_box_to_template(bin, w, &boxes[i], norm);

        for (t = 0; t < model->template_count; t++) {
            int d = template_distance(norm, model->templates[t].bits);
            if (d < best_dist) {
                best_dist = d;
                best_idx = t;
            }
        }

        if (best_idx < 0 || best_dist > 190) goto cleanup;
        out_text[pos++] = model->templates[best_idx].ch;
    }

    out_text[pos] = '\0';
    ok = (pos > 0);

cleanup:
    if (bin) free(bin);
    if (gray) free(gray);
    return ok;
}

static double parse_price_text(const char* s) {
    char buf[128];
    size_t i;
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (i = 0; i < n; i++) {
        buf[i] = (s[i] == ',') ? '.' : s[i];
    }
    buf[n] = '\0';
    return strtod(buf, NULL);
}

double get_price(void) {
    char txt[128];
    if (!recognize_text(&g_config.price, txt, sizeof(txt))) return -1.0;
    return parse_price_text(txt);
}

double last_lot_price(void) {
    char txt[128];
    if (!recognize_text(&g_config.last_lot, txt, sizeof(txt))) return -1.0;
    return parse_price_text(txt);
}

static int save_model(FILE* f, const char* name, const OCRModel* m) {
    int i, j;
    if (fprintf(f, "MODEL %s %d %d %d %d %d %d\n",
        name,
        m->region.x, m->region.y, m->region.w, m->region.h,
        m->threshold_bias, m->template_count) < 0) return 0;
    for (i = 0; i < m->template_count; i++) {
        if (fprintf(f, "T %c ", m->templates[i].ch) < 0) return 0;
        for (j = 0; j < NORM_PIXELS; j++) {
            if (fputc(m->templates[i].bits[j] ? '1' : '0', f) == EOF) return 0;
        }
        if (fputc('\n', f) == EOF) return 0;
    }
    return 1;
}

static int parse_model_header(const char* line, OCRModel* m) {
    char name_buf[32];
    int parsed = sscanf(line, "MODEL %31s %d %d %d %d %d %d",
        name_buf,
        &m->region.x, &m->region.y, &m->region.w, &m->region.h,
        &m->threshold_bias, &m->template_count);
    if (parsed != 7) return 0;
    if (m->template_count < 0 || m->template_count > MAX_TEMPLATES) return 0;
    return 1;
}

static int read_model_body(FILE* f, OCRModel* m) {
    char line[1024];
    int i;
    for (i = 0; i < m->template_count; i++) {
        char tag[4];
        char ch;
        char bits[NORM_PIXELS + 4];
        int j;
        if (!fgets(line, sizeof(line), f)) return 0;
        if (sscanf(line, "%3s %c %603s", tag, &ch, bits) != 3) return 0;
        if (strcmp(tag, "T") != 0) return 0;
        if ((int)strlen(bits) != NORM_PIXELS) return 0;
        m->templates[i].ch = ch;
        for (j = 0; j < NORM_PIXELS; j++) {
            m->templates[i].bits[j] = (bits[j] == '1') ? 1 : 0;
        }
    }
    return 1;
}

static int read_one_model(FILE* f, OCRModel* m) {
    char line[1024];
    memset(m, 0, sizeof(*m));
    if (!fgets(line, sizeof(line), f)) return 0;
    if (!parse_model_header(line, m)) return 0;
    return read_model_body(f, m);
}

static int read_one_model_with_header_line(FILE* f, const char* header_line, OCRModel* m) {
    memset(m, 0, sizeof(*m));
    if (!parse_model_header(header_line, m)) return 0;
    return read_model_body(f, m);
}

static int save_config(const OCRConfig* cfg, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    if (fprintf(f, "OCRCFG1\n") < 0) {
        fclose(f);
        return 0;
    }
    if (fprintf(f, "WINDOW %d\n", cfg->use_window ? 1 : 0) < 0) {
        fclose(f);
        return 0;
    }
    if (fprintf(f, "WTITLE %s\n", cfg->window_title) < 0) {
        fclose(f);
        return 0;
    }
    if (fprintf(f, "WCLASS %s\n", cfg->window_class) < 0) {
        fclose(f);
        return 0;
    }
    if (fprintf(f, "BASECLIENT %d %d\n", cfg->base_client_w, cfg->base_client_h) < 0) {
        fclose(f);
        return 0;
    }
    if (fprintf(f, "RENDER_POINT %d %d %d\n", cfg->render_point_set ? 1 : 0, cfg->render_point.x, cfg->render_point.y) < 0) {
        fclose(f);
        return 0;
    }
    if (fprintf(f, "OVERLAY %d\n", cfg->overlay_enabled ? 1 : 0) < 0) {
        fclose(f);
        return 0;
    }
    if (!save_model(f, "price", &cfg->price)) {
        fclose(f);
        return 0;
    }
    if (!save_model(f, "last_lot", &cfg->last_lot)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int load_config(OCRConfig* cfg, const char* path) {
    FILE* f = fopen(path, "rb");
    char magic[32];
    char line[1024];
    OCRModel a, b;
    if (!f) return 0;
    if (!fgets(magic, sizeof(magic), f)) {
        fclose(f);
        return 0;
    }
    if (strncmp(magic, "OCRCFG1", 7) != 0) {
        fclose(f);
        return 0;
    }

    cfg->use_window = 0;
    cfg->window_title[0] = '\0';
    cfg->window_class[0] = '\0';
    cfg->base_client_w = 0;
    cfg->base_client_h = 0;
    cfg->render_point_set = 0;
    cfg->render_point.x = 0;
    cfg->render_point.y = 0;
    cfg->overlay_enabled = 1;

    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }

    if (strncmp(line, "WINDOW ", 7) == 0) {
        int use_w = 0;
        if (sscanf(line + 7, "%d", &use_w) == 1) {
            cfg->use_window = use_w ? 1 : 0;
        }

        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return 0;
        }
        if (strncmp(line, "WTITLE ", 7) == 0) {
            strncpy(cfg->window_title, line + 7, sizeof(cfg->window_title) - 1);
            cfg->window_title[sizeof(cfg->window_title) - 1] = '\0';
            trim_newline(cfg->window_title);
        }

        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return 0;
        }
        if (strncmp(line, "WCLASS ", 7) == 0) {
            strncpy(cfg->window_class, line + 7, sizeof(cfg->window_class) - 1);
            cfg->window_class[sizeof(cfg->window_class) - 1] = '\0';
            trim_newline(cfg->window_class);
        }
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return 0;
        }
        if (strncmp(line, "BASECLIENT ", 11) == 0) {
            int bw = 0, bh = 0;
            if (sscanf(line + 11, "%d %d", &bw, &bh) == 2) {
                cfg->base_client_w = bw;
                cfg->base_client_h = bh;
            }
            if (!fgets(line, sizeof(line), f)) {
                fclose(f);
                return 0;
            }
        }
        while (strncmp(line, "RENDER_POINT ", 13) == 0 || strncmp(line, "OVERLAY ", 8) == 0) {
            if (strncmp(line, "RENDER_POINT ", 13) == 0) {
                int en = 0, x = 0, y = 0;
                if (sscanf(line + 13, "%d %d %d", &en, &x, &y) == 3) {
                    cfg->render_point_set = en ? 1 : 0;
                    cfg->render_point.x = x;
                    cfg->render_point.y = y;
                }
            } else if (strncmp(line, "OVERLAY ", 8) == 0) {
                int en = 1;
                if (sscanf(line + 8, "%d", &en) == 1) cfg->overlay_enabled = en ? 1 : 0;
            }
            if (!fgets(line, sizeof(line), f)) {
                fclose(f);
                return 0;
            }
        }
        if (!read_one_model_with_header_line(f, line, &a) || !read_one_model(f, &b)) {
            fclose(f);
            return 0;
        }
    } else {
        while (strncmp(line, "RENDER_POINT ", 13) == 0 || strncmp(line, "OVERLAY ", 8) == 0) {
            if (strncmp(line, "RENDER_POINT ", 13) == 0) {
                int en = 0, x = 0, y = 0;
                if (sscanf(line + 13, "%d %d %d", &en, &x, &y) == 3) {
                    cfg->render_point_set = en ? 1 : 0;
                    cfg->render_point.x = x;
                    cfg->render_point.y = y;
                }
            } else if (strncmp(line, "OVERLAY ", 8) == 0) {
                int en = 1;
                if (sscanf(line + 8, "%d", &en) == 1) cfg->overlay_enabled = en ? 1 : 0;
            }
            if (!fgets(line, sizeof(line), f)) {
                fclose(f);
                return 0;
            }
        }
        if (!read_one_model_with_header_line(f, line, &a) || !read_one_model(f, &b)) {
            fclose(f);
            return 0;
        }
    }

    if (cfg->use_window) {
        g_target_hwnd = FindWindowA(
            cfg->window_class[0] ? cfg->window_class : NULL,
            cfg->window_title[0] ? cfg->window_title : NULL
        );
    } else {
        g_target_hwnd = NULL;
    }

    if (!a.template_count || !b.template_count) {
        fclose(f);
        return 0;
    }
    cfg->price = a;
    cfg->last_lot = b;
    fclose(f);
    return 1;
}

static void init_defaults(OCRConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->price.threshold_bias = 0;
    cfg->last_lot.threshold_bias = 0;
    cfg->use_window = 0;
    cfg->window_title[0] = '\0';
    cfg->window_class[0] = '\0';
    cfg->base_client_w = 0;
    cfg->base_client_h = 0;
    cfg->render_point_set = 0;
    cfg->render_point.x = 0;
    cfg->render_point.y = 0;
    cfg->overlay_enabled = 1;
    g_target_hwnd = NULL;
}

static void print_status(const OCRConfig* cfg) {
    if (cfg->use_window) {
        printf("window bind: ON, title='%s', class='%s', hwnd=%p\n",
            cfg->window_title,
            cfg->window_class,
            (void*)resolve_target_window());
        printf("base client size: %dx%d\n", cfg->base_client_w, cfg->base_client_h);
    } else {
        printf("window bind: OFF\n");
    }
    printf("price region: x=%d y=%d w=%d h=%d\n", cfg->price.region.x, cfg->price.region.y, cfg->price.region.w, cfg->price.region.h);
    printf("price templates=%d threshold_bias=%d\n", cfg->price.template_count, cfg->price.threshold_bias);
    printf("last_lot region: x=%d y=%d w=%d h=%d\n", cfg->last_lot.region.x, cfg->last_lot.region.y, cfg->last_lot.region.w, cfg->last_lot.region.h);
    printf("last_lot templates=%d threshold_bias=%d\n", cfg->last_lot.template_count, cfg->last_lot.threshold_bias);
    printf("render point: %s x=%d y=%d\n", cfg->render_point_set ? "SET" : "NOT SET", cfg->render_point.x, cfg->render_point.y);
    printf("overlay: %s\n", cfg->overlay_enabled ? "ON" : "OFF");
}

static void set_threshold_bias(OCRModel* model, const char* name) {
    char line[64];
    int v;
    printf("Current %s threshold_bias=%d. New value (-100..100): ", name, model->threshold_bias);
    if (!fgets(line, sizeof(line), stdin)) return;
    if (sscanf(line, "%d", &v) == 1) {
        model->threshold_bias = clamp_int(v, -100, 100);
    }
}

static void test_once(void) {
    char a[128] = {0};
    char b[128] = {0};
    int ok1 = recognize_text(&g_config.price, a, sizeof(a));
    int ok2 = recognize_text(&g_config.last_lot, b, sizeof(b));

    if (ok1) printf("get_price text='%s' value=%.6f\n", a, parse_price_text(a));
    else printf("get_price failed\n");
    if (ok2) printf("last_lot_price text='%s' value=%.6f\n", b, parse_price_text(b));
    else printf("last_lot_price failed\n");
}

static void watch_loop(void) {
    ULONGLONG last_console_log = 0;
    MSG msg;
    HWND console_hwnd = GetConsoleWindow();
    printf("Watch started. Press ESC to stop.\n");
    while (!(GetAsyncKeyState(VK_ESCAPE) & 1)) {
        double p = get_price();
        double lp = last_lot_price();
        Region rp;
        Region rl;
        Region* prp = NULL;
        Region* prl = NULL;
        POINT draw_pt;
        POINT* pdraw = NULL;
        char text[64];
        ULONGLONG now = GetTickCount64();

        if (get_effective_region(&g_config.price.region, &rp)) prp = &rp;
        if (get_effective_region(&g_config.last_lot.region, &rl)) prl = &rl;
        if (g_config.render_point_set && get_effective_point(&g_config.render_point, &draw_pt)) pdraw = &draw_pt;

        if (lp >= 0.0) snprintf(text, sizeof(text), "%.2f", lp * 0.8);
        else strcpy(text, "ERR");
        overlay_update(prp, prl, pdraw, text);

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (!console_hwnd || !IsIconic(console_hwnd)) {
            if (now - last_console_log >= 80) {
                if (p < 0) printf("\rprice=ERR ");
                else printf("\rprice=%.6f ", p);
                if (lp < 0) printf("last=ERR   ");
                else printf("last=%.6f   ", lp);
                fflush(stdout);
                last_console_log = now;
            }
        }
        Sleep(1);
    }
    overlay_hide();
    printf("\n");
}

static void load_templates_cli(void) {
    char folder[MAX_PATH];
    int ok1, ok2;
    printf("Templates folder (ENTER for current '.'): ");
    if (!fgets(folder, sizeof(folder), stdin)) return;
    trim_newline(folder);
    if (folder[0] == '\0') strcpy(folder, ".");

    ok1 = load_templates_from_folder(&g_config.price, folder);
    ok2 = load_templates_from_folder(&g_config.last_lot, folder);
    if (ok1 && ok2) {
        printf("Templates loaded from %s\n", folder);
    } else {
        printf("Template loading failed.\n");
    }
}

static int select_model_region_with_window_support(Region* model_region, const char* name) {
    Region abs_region;
    if (!select_region_from_screenshot(&abs_region)) return 0;

    if (g_config.use_window) {
        RECT cr;
        int cw, ch;
        if (!get_target_client_rect_screen(&cr)) {
            printf("Selected window is not available. Re-select window first.\n");
            return 0;
        }
        cw = cr.right - cr.left;
        ch = cr.bottom - cr.top;
        if (cw <= 0 || ch <= 0) return 0;
        if (g_config.base_client_w <= 0 || g_config.base_client_h <= 0) {
            g_config.base_client_w = cw;
            g_config.base_client_h = ch;
        }
        model_region->x = (int)((abs_region.x - cr.left) * ((double)g_config.base_client_w / cw) + 0.5);
        model_region->y = (int)((abs_region.y - cr.top) * ((double)g_config.base_client_h / ch) + 0.5);
        model_region->w = (int)(abs_region.w * ((double)g_config.base_client_w / cw) + 0.5);
        model_region->h = (int)(abs_region.h * ((double)g_config.base_client_h / ch) + 0.5);
        if (model_region->w < 1) model_region->w = 1;
        if (model_region->h < 1) model_region->h = 1;
        printf("%s region set relative to window: x=%d y=%d w=%d h=%d\n",
            name, model_region->x, model_region->y, model_region->w, model_region->h);
    } else {
        *model_region = abs_region;
        printf("%s region set absolute: x=%d y=%d w=%d h=%d\n",
            name, model_region->x, model_region->y, model_region->w, model_region->h);
    }
    return 1;
}

static int select_render_point_with_window_support(void) {
    POINT abs_point;
    if (!select_point_from_screenshot(&abs_point)) return 0;

    if (g_config.use_window) {
        RECT cr;
        int cw, ch;
        if (!get_target_client_rect_screen(&cr)) {
            printf("Selected window is not available. Re-select window first.\n");
            return 0;
        }
        cw = cr.right - cr.left;
        ch = cr.bottom - cr.top;
        if (cw <= 0 || ch <= 0) return 0;
        if (g_config.base_client_w <= 0 || g_config.base_client_h <= 0) {
            g_config.base_client_w = cw;
            g_config.base_client_h = ch;
        }
        g_config.render_point.x = (int)((abs_point.x - cr.left) * ((double)g_config.base_client_w / cw) + 0.5);
        g_config.render_point.y = (int)((abs_point.y - cr.top) * ((double)g_config.base_client_h / ch) + 0.5);
    } else {
        g_config.render_point = abs_point;
    }
    g_config.render_point_set = 1;
    printf("Render point set: x=%d y=%d\n", g_config.render_point.x, g_config.render_point.y);
    return 1;
}

int main(void) {
    char line[64];
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        printf("COM init failed.\n");
        return 1;
    }

    configure_console_runtime();
    init_defaults(&g_config);
    if (load_config(&g_config, CONFIG_FILE)) {
        printf("Loaded config from %s\n", CONFIG_FILE);
    } else {
        printf("Config not found, defaults loaded.\n");
    }

    while (1) {
        printf("\n=== OCR menu ===\n");
        printf("1 - Select target window (click)\n");
        printf("2 - Disable window binding (use absolute regions)\n");
        printf("3 - Select price region (screenshot)\n");
        printf("4 - Select last_lot region (screenshot)\n");
        printf("5 - Load templates from PNG files (0.png..9.png + dot.png/comma.png)\n");
        printf("6 - Set price threshold bias\n");
        printf("7 - Set last_lot threshold bias\n");
        printf("8 - Select render point (screenshot click)\n");
        printf("9 - Toggle overlay ON/OFF\n");
        printf("10 - Test once\n");
        printf("11 - Watch loop\n");
        printf("12 - Save config\n");
        printf("13 - Load config\n");
        printf("14 - Status\n");
        printf("0 - Exit\n");
        printf("> ");

        if (!fgets(line, sizeof(line), stdin)) break;
        switch (atoi(line)) {
        case 1:
            select_target_window_by_click();
            break;
        case 2:
            if (g_config.use_window) {
                Region abs_price;
                Region abs_last;
                POINT abs_point;
                if (get_effective_region(&g_config.price.region, &abs_price)) {
                    g_config.price.region = abs_price;
                }
                if (get_effective_region(&g_config.last_lot.region, &abs_last)) {
                    g_config.last_lot.region = abs_last;
                }
                if (g_config.render_point_set && get_effective_point(&g_config.render_point, &abs_point)) {
                    g_config.render_point = abs_point;
                }
            }
            g_config.use_window = 0;
            g_config.base_client_w = 0;
            g_config.base_client_h = 0;
            g_target_hwnd = NULL;
            printf("Window binding disabled.\n");
            break;
        case 3:
            select_model_region_with_window_support(&g_config.price.region, "Price");
            break;
        case 4:
            select_model_region_with_window_support(&g_config.last_lot.region, "Last_lot");
            break;
        case 5:
            load_templates_cli();
            break;
        case 6:
            set_threshold_bias(&g_config.price, "price");
            break;
        case 7:
            set_threshold_bias(&g_config.last_lot, "last_lot");
            break;
        case 8:
            select_render_point_with_window_support();
            break;
        case 9:
            g_config.overlay_enabled = g_config.overlay_enabled ? 0 : 1;
            printf("overlay: %s\n", g_config.overlay_enabled ? "ON" : "OFF");
            if (!g_config.overlay_enabled) overlay_hide();
            break;
        case 10:
            test_once();
            break;
        case 11:
            watch_loop();
            break;
        case 12:
            if (save_config(&g_config, CONFIG_FILE)) printf("Config saved.\n");
            else printf("Save failed.\n");
            break;
        case 13:
            if (load_config(&g_config, CONFIG_FILE)) printf("Config loaded.\n");
            else printf("Load failed.\n");
            break;
        case 14:
            print_status(&g_config);
            break;
        case 0:
            overlay_hide();
            if (g_overlay_font) {
                DeleteObject(g_overlay_font);
                g_overlay_font = NULL;
            }
            CoUninitialize();
            return 0;
        default:
            printf("Unknown command.\n");
            break;
        }
    }

    overlay_hide();
    if (g_overlay_font) {
        DeleteObject(g_overlay_font);
        g_overlay_font = NULL;
    }
    CoUninitialize();
    return 0;
}
