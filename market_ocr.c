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
static HWND g_target_hwnd = NULL;

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

static int get_effective_region(const Region* in_model_region, Region* out_capture_region) {
    if (!g_config.use_window) {
        *out_capture_region = *in_model_region;
        return 1;
    }

    {
        RECT wr;
        if (!get_target_window_rect(&wr)) return 0;
        out_capture_region->x = wr.left + in_model_region->x;
        out_capture_region->y = wr.top + in_model_region->y;
        out_capture_region->w = in_model_region->w;
        out_capture_region->h = in_model_region->h;
    }
    return 1;
}

static void store_selected_window(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    g_target_hwnd = root;
    g_config.use_window = 1;
    g_config.window_title[0] = '\0';
    g_config.window_class[0] = '\0';
    GetWindowTextA(root, g_config.window_title, (int)sizeof(g_config.window_title));
    GetClassNameA(root, g_config.window_class, (int)sizeof(g_config.window_class));
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
                    RECT wr;
                    if (get_target_window_rect(&wr)) {
                        g_config.price.region.x -= wr.left;
                        g_config.price.region.y -= wr.top;
                        g_config.last_lot.region.x -= wr.left;
                        g_config.last_lot.region.y -= wr.top;
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

        if (!read_one_model(f, &a) || !read_one_model(f, &b)) {
            fclose(f);
            return 0;
        }
    } else {
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
    g_target_hwnd = NULL;
}

static void print_status(const OCRConfig* cfg) {
    if (cfg->use_window) {
        printf("window bind: ON, title='%s', class='%s', hwnd=%p\n",
            cfg->window_title,
            cfg->window_class,
            (void*)resolve_target_window());
    } else {
        printf("window bind: OFF\n");
    }
    printf("price region: x=%d y=%d w=%d h=%d\n", cfg->price.region.x, cfg->price.region.y, cfg->price.region.w, cfg->price.region.h);
    printf("price templates=%d threshold_bias=%d\n", cfg->price.template_count, cfg->price.threshold_bias);
    printf("last_lot region: x=%d y=%d w=%d h=%d\n", cfg->last_lot.region.x, cfg->last_lot.region.y, cfg->last_lot.region.w, cfg->last_lot.region.h);
    printf("last_lot templates=%d threshold_bias=%d\n", cfg->last_lot.template_count, cfg->last_lot.threshold_bias);
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
    printf("Watch started. Press ESC in this console window to stop.\n");
    while (!(GetAsyncKeyState(VK_ESCAPE) & 1)) {
        double p = get_price();
        double lp = last_lot_price();
        if (p < 0) printf("\rprice=ERR ");
        else printf("\rprice=%.6f ", p);
        if (lp < 0) printf("last=ERR   ");
        else printf("last=%.6f   ", lp);
        fflush(stdout);
    }
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
        RECT wr;
        if (!get_target_window_rect(&wr)) {
            printf("Selected window is not available. Re-select window first.\n");
            return 0;
        }
        model_region->x = abs_region.x - wr.left;
        model_region->y = abs_region.y - wr.top;
        model_region->w = abs_region.w;
        model_region->h = abs_region.h;
        printf("%s region set relative to window: x=%d y=%d w=%d h=%d\n",
            name, model_region->x, model_region->y, model_region->w, model_region->h);
    } else {
        *model_region = abs_region;
        printf("%s region set absolute: x=%d y=%d w=%d h=%d\n",
            name, model_region->x, model_region->y, model_region->w, model_region->h);
    }
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
        printf("8 - Test once\n");
        printf("9 - Watch loop\n");
        printf("10 - Save config\n");
        printf("11 - Load config\n");
        printf("12 - Status\n");
        printf("0 - Exit\n");
        printf("> ");

        if (!fgets(line, sizeof(line), stdin)) break;
        switch (atoi(line)) {
        case 1:
            select_target_window_by_click();
            break;
        case 2:
            if (g_config.use_window) {
                RECT wr;
                if (get_target_window_rect(&wr)) {
                    g_config.price.region.x += wr.left;
                    g_config.price.region.y += wr.top;
                    g_config.last_lot.region.x += wr.left;
                    g_config.last_lot.region.y += wr.top;
                }
            }
            g_config.use_window = 0;
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
            test_once();
            break;
        case 9:
            watch_loop();
            break;
        case 10:
            if (save_config(&g_config, CONFIG_FILE)) printf("Config saved.\n");
            else printf("Save failed.\n");
            break;
        case 11:
            if (load_config(&g_config, CONFIG_FILE)) printf("Config loaded.\n");
            else printf("Load failed.\n");
            break;
        case 12:
            print_status(&g_config);
            break;
        case 0:
            CoUninitialize();
            return 0;
        default:
            printf("Unknown command.\n");
            break;
        }
    }

    CoUninitialize();
    return 0;
}
