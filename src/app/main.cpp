// USDZ Viewer: a native Windows viewer for USD / USDZ files and their animations.
//
// Layout: menu bar; a tab strip; a hierarchy tree and property list on the left; the Direct3D viewport; a
// transport bar (play/pause, clip, timeline, speed, loop); a status bar. Files load on worker threads. Each tab
// holds one or more models ("Open Additional" adds to the current tab): they are placed side by side and animate
// together; the transport bar controls the active model (the one last selected in the hierarchy, or the last one
// opened). Every tab keeps its own camera, selection and playback state.
#include "app/resource.h"
#include "render/image.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "usd/fileio.h"
#include "usd/stage.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT WM_APP_LOADED = WM_APP + 1;

enum : UINT {
    ID_FILE_OPEN = 100, ID_FILE_RELOAD, ID_FILE_PREV, ID_FILE_NEXT, ID_FILE_EXIT, ID_FILE_OPEN_ADDITIONAL,
    ID_FILE_CLOSE_MODEL, ID_FILE_NEW_TAB, ID_FILE_CLOSE_TAB, ID_FILE_NEXT_TAB, ID_FILE_PREV_TAB, ID_FILE_CLOSE_OTHER_TABS,
    ID_VIEW_FRAME = 200, ID_VIEW_HIERARCHY, ID_VIEW_GRID, ID_VIEW_SKELETON, ID_VIEW_AXES, ID_VIEW_SHADED,
    ID_VIEW_WIREFRAME, ID_VIEW_SHADED_WIRE, ID_VIEW_CYCLE_WIRE, ID_VIEW_BACKFACES, ID_VIEW_LIT, ID_VIEW_BASECOLOR,
    ID_VIEW_NORMALS, ID_VIEW_EXPOSURE_UP, ID_VIEW_EXPOSURE_DOWN, ID_VIEW_EXPOSURE_RESET, ID_VIEW_INFO,
    ID_VIEW_CLEAR_SELECTION, ID_VIEW_RESET_CAMERA, ID_VIEW_FOLLOW, ID_VIEW_SHADOW, ID_VIEW_STEADY, ID_VIEW_RULER,
    ID_ANIM_PLAY = 300, ID_ANIM_PREV_FRAME, ID_ANIM_NEXT_FRAME, ID_ANIM_START, ID_ANIM_END, ID_ANIM_LOOP,
    ID_ANIM_PREV_CLIP, ID_ANIM_NEXT_CLIP,
    ID_ANIM_SPEED = 320,  // + index into kSpeeds
    ID_ANIM_BLEND = 340,  // + index into kBlendSeconds
    ID_HELP_CONTROLS = 400, ID_HELP_ABOUT,
    ID_TAB_SELECT = 500,  // + 0..7: that tab, + 8: the last tab (Ctrl+1 .. Ctrl+9)
    IDC_PLAY = 1000, IDC_CLIP, IDC_TIMELINE, IDC_TIME, IDC_SPEED, IDC_LOOP, IDC_TREE, IDC_PROPS, IDC_STATUS, IDC_INFO_EDIT,
};

const double kSpeeds[] = {0.1, 0.25, 0.5, 1.0, 1.5, 2.0};
const wchar_t* kSpeedLabels[] = {L"0.1×", L"0.25×", L"0.5×", L"1×", L"1.5×", L"2×"};
constexpr int kDefaultSpeed = 3;
// Crossfade length when the animation switches (another clip, a loop restarting, a clip boundary).
const double kBlendSeconds[] = {0, 0.15, 0.3, 0.5, 1.0};
const wchar_t* kBlendLabels[] = {L"&Off", L"0.15 s", L"0.3 s", L"0.5 s", L"1 s"};
constexpr int kDefaultBlend = 2;
constexpr int kTabStripDip = 34;  // height of the tab strip

using usd::Utf8ToWide;
using usd::WideToUtf8;

std::wstring FileName(const std::wstring& path) {
    size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

std::wstring DirName(const std::wstring& path) {
    size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

bool IsUsdFile(const std::wstring& name) {
    const wchar_t* ext = PathFindExtensionW(name.c_str());
    return _wcsicmp(ext, L".usdz") == 0 || _wcsicmp(ext, L".usda") == 0 || _wcsicmp(ext, L".usdc") == 0 ||
           _wcsicmp(ext, L".usd") == 0;
}

std::wstring FormatTime(double t) {
    wchar_t buf[32];
    if (std::fabs(t - std::round(t)) < 1e-6) std::swprintf(buf, 32, L"%.0f", t);
    else std::swprintf(buf, 32, L"%.1f", t);
    return buf;
}

std::wstring WithThousands(size_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), L",");
    return s;
}

double Now() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

scene::Bounds Shifted(const scene::Bounds& b, const scene::Vec3& offset) {
    if (b.IsEmpty()) return b;
    scene::Bounds r;
    r.min = b.min + offset;
    r.max = b.max + offset;
    return r;
}

struct LoadResult {
    uint64_t tabId = 0;     // the tab the file opens in (dropped if the tab was closed meanwhile)
    uint64_t targetId = 0;  // non-zero: replaces this model (reload, next/previous file)
    uint64_t ticket = 0;    // the target's request number when this load started
    uint64_t order = 0;     // position among the open models (files load in parallel, finish in any order)
    bool keepView = false;
    std::wstring path;
    std::unique_ptr<usd::Stage> stage;
    std::unique_ptr<scene::Scene> scene;
    std::vector<render::DecodedImage> images;
    scene::Bounds animBounds;
    scene::Bounds frameBounds;
    std::string error;
    double seconds = 0;
};

// Runs on a worker thread: parse + compose + build the scene + decode textures.
void LoadWorker(HWND notify, std::unique_ptr<LoadResult> r) {
    const double t0 = Now();
    r->stage = usd::Stage::Open(r->path, r->error);
    if (r->stage) {
        r->scene = std::make_unique<scene::Scene>();
        r->scene->Build(*r->stage);

        // Decode textures in parallel.
        const size_t n = r->scene->images.size();
        r->images.resize(n);
        std::atomic<size_t> next{0};
        auto work = [&]() {
            for (size_t i; (i = next.fetch_add(1)) < n;) {
                std::vector<uint8_t> bytes;
                std::string err;
                if (!r->stage->ReadAsset(r->scene->images[i].location, bytes, err)) {
                    r->images[i].error = err;
                    continue;
                }
                r->images[i] = render::DecodeImage(bytes.data(), bytes.size());
            }
        };
        unsigned threads = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads && t < n; ++t) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();

        // Bounds over the whole animation, for framing the camera, spacing models and placing the grid.
        scene::Scene& sc = *r->scene;
        size_t points = 0;
        for (auto& m : sc.meshes) points += m.pointCount;
        const int samples = sc.animated ? (points > 400000 ? 4 : 24) : 1;
        for (int i = 0; i < samples; ++i) {
            double t = samples == 1 ? sc.startTime : sc.startTime + (sc.endTime - sc.startTime) * i / (samples - 1);
            sc.Evaluate(t);
            r->animBounds.Add(sc.ComputeBounds());
        }
        sc.Evaluate(sc.startTime);
        r->frameBounds = sc.ComputeBindPoseBounds();
        if (r->frameBounds.IsEmpty()) r->frameBounds = r->animBounds;
    }
    r->seconds = Now() - t0;
    if (!PostMessageW(notify, WM_APP_LOADED, 0, reinterpret_cast<LPARAM>(r.get()))) return;  // window gone
    r.release();
}

// One open file.
struct Model {
    uint64_t id = 0;
    uint64_t ticket = 0;  // last reload / replace request
    uint64_t order = 0;   // left-to-right position
    std::wstring path;
    std::unique_ptr<usd::Stage> stage;
    std::unique_ptr<scene::Scene> scene;
    scene::Bounds animBounds, frameBounds;  // without placement
    scene::Vec3 offset;                     // placement in the viewer
    double loadSeconds = 0;
    std::unordered_map<const usd::Prim*, int> nodeOfPrim;
    // playback
    double time = 0, rangeStart = 0, rangeEnd = 0;
    int clip = -1;
    // Crossfade from the animation that was showing into the current one.
    struct Transition {
        bool active = false;
        bool snapshot = false;     // fade from `fromPose` (switched again mid-fade) instead of sampling `fromTime`
        scene::Pose fromPose;
        bool fromAdvance = false;  // the outgoing animation keeps playing (up to `fromEnd`) instead of holding
        double fromTime = 0, fromEnd = 0;
        double elapsed = 0, duration = 0;  // seconds of animation time
        bool started = false;      // false until the first frame of the fade has been drawn
    } blend;

    scene::Bounds WorldAnimBounds() const { return Shifted(animBounds, offset); }
    scene::Bounds WorldFrameBounds() const { return Shifted(frameBounds, offset); }
    double MetersPerUnit() const { return stage && stage->metersPerUnit > 0 ? stage->metersPerUnit : 1.0; }
};

struct TreeEntry {
    Model* model;
    const usd::Prim* prim;  // nullptr: the model's own row
};

// One tab: a set of models shown together, with its own camera, selection and playback state.
struct Tab {
    uint64_t id = 0;
    std::vector<std::unique_ptr<Model>> models;
    Model* active = nullptr;                  // the model the transport bar controls
    Model* selModel = nullptr;                // selection in the hierarchy
    const usd::Prim* selPrim = nullptr;       // nullptr with selModel set: the model row is selected
    render::Camera camera;
    bool playing = false;
    int pendingLoads = 0;
    std::wstring loadingName;
    // Derived from the models by LayoutModels: world-space bounds of all animations, grid and shadow setup.
    scene::Bounds allAnimBounds;
    double gridCell = 0.1, gridExtent = 10, gridHeight = 0;
    // Hierarchy rows that were expanded when the tree last showed this tab (rows of `treeModels` only).
    std::set<std::pair<const Model*, const usd::Prim*>> expanded;
    std::set<const Model*> treeModels;

    bool IsEmpty() const { return models.empty() && pendingLoads == 0; }
};

class App {
public:
    bool Create(HINSTANCE inst, int show, const std::vector<std::wstring>& initialFiles);
    int Run();

private:
    // windows
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr, viewport_ = nullptr, tree_ = nullptr, props_ = nullptr, status_ = nullptr;
    HWND playBtn_ = nullptr, clipCombo_ = nullptr, timeline_ = nullptr, timeLabel_ = nullptr, speedCombo_ = nullptr,
         loopCheck_ = nullptr;
    HWND infoWnd_ = nullptr, infoEdit_ = nullptr;
    HMENU menu_ = nullptr;
    HACCEL accel_ = nullptr;
    HFONT font_ = nullptr, monoFont_ = nullptr;
    UINT dpi_ = 96;

    // rendering
    render::Renderer renderer_;
    render::RenderOptions options_;
    bool rendererOk_ = false;

    // documents: tabs_ is never empty, tab_ is the one shown
    std::vector<std::unique_ptr<Tab>> tabs_;
    Tab* tab_ = nullptr;
    Tab* treeTab_ = nullptr;  // the tab whose rows the hierarchy shows
    std::vector<TreeEntry> treeEntries_;
    uint64_t nextTabId_ = 1;
    uint64_t nextModelId_ = 1;
    uint64_t nextTicket_ = 1;
    uint64_t nextOrder_ = 1;
    int pendingLoads_ = 0;  // all tabs
    std::wstring loadingName_;
    bool fillingTree_ = false;
    render::RenderOptions::RulerBox statusRuler_;

    // tab strip
    HWND tabStrip_ = nullptr;
    HFONT iconFont_ = nullptr;
    int tabScroll_ = 0;              // pixels the tabs are scrolled left when they do not fit
    const Tab* tabHover_ = nullptr;  // under the mouse
    int tabHoverPart_ = 0;           // 0 the tab, 1 its close button; 2 the new-tab button (tabHover_ null)
    Tab* tabPressClose_ = nullptr;   // close button being clicked
    Tab* tabDrag_ = nullptr;         // tab being dragged to another position
    bool tabTracking_ = false;

    // playback
    bool scrubbing_ = false;
    double tickScale_ = 1;
    int speedIndex_ = kDefaultSpeed;
    bool loop_ = true;
    int blendIndex_ = kDefaultBlend;
    bool needsEvaluate_ = true;
    bool needsRedraw_ = true;
    int lastTimelinePos_ = -1;
    double lastFrameTime_ = 0;

    // interaction
    bool showPanel_ = true;
    bool follow_ = false;
    bool followDirty_ = false;
    // Present every refresh while the viewer is in front, re-showing the last image when nothing changed.
    // Variable-refresh displays (G-SYNC / FreeSync, especially OLEDs) flicker when the frame rate jumps
    // between idle and active.
    bool steadyPacing_ = true;
    int panelWidthDip_ = 300;
    bool splitterDrag_ = false;
    int dragButton_ = 0;  // 0 none, 1 orbit, 2 pan, 3 zoom
    POINT dragStart_{}, lastMouse_{};
    bool dragMoved_ = false;

    // fps
    int frames_ = 0;
    double fpsTime_ = 0;
    double fps_ = 0;

    int S(int dip) const { return MulDiv(dip, int(dpi_), 96); }

    static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK ViewportProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK InfoProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK TabsProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT HandleMain(UINT m, WPARAM w, LPARAM l);
    LRESULT HandleViewport(UINT m, WPARAM w, LPARAM l);
    LRESULT HandleTabs(UINT m, WPARAM w, LPARAM l);

    void CreateMenus();
    void CreateControls();
    void CreateFonts();
    void Layout();
    RECT SplitterRect() const;
    bool PreTranslate(MSG& msg);
    bool NeedsFrame() const;
    bool SteadyActive() const;
    void Frame(bool redrawScene);
    void PumpFrame();
    void Command(UINT id);
    void UpdateMenuChecks();
    int Message(const std::wstring& text, UINT flags, const wchar_t* title = L"USDZ Viewer");
    int modalDepth_ = 0;

    // tabs
    Tab* NewTab(bool show);
    void SwitchTab(Tab* t);
    void CloseTab(Tab* t);
    Tab* FindTab(uint64_t id) const;
    int TabIndex(const Tab* t) const;
    std::wstring TabTitle(const Tab& t) const;
    struct TabRects {
        RECT tab, close;
    };
    void TabGeometry(int& width, int& area) const;
    std::vector<TabRects> LayoutTabs(int* tabsRight = nullptr) const;
    RECT NewTabButtonRect() const;
    Tab* TabAt(POINT p, int* part) const;
    void ScrollTabIntoView(const Tab* t);
    void UpdateTabs();
    void PaintTabs(HDC dc);
    void ShowTabMenu(POINT screen, Tab* t);

    // documents
    void OpenFiles(const std::vector<std::wstring>& files, bool additional);
    void RequestLoad(Tab& tab, const std::wstring& path, Model* target, bool keepView);
    void OnLoaded(std::unique_ptr<LoadResult> r);
    void ShowOpenDialog(bool additional);
    void OpenSibling(int delta);
    void CloseModel(Model* m);
    void LayoutModels(Tab& t);
    void SetActive(Model* m);
    static bool AnyAnimated(const Tab& t);
    bool AnyAnimated() const { return AnyAnimated(*tab_); }
    void SaveTreeState();
    void FillTree();
    void FillClips();
    void ShowProperties();
    void SetSelection(Model* m, const usd::Prim* prim);
    void UpdateHighlight();
    void ShowInfo(bool bringToFront = true);
    void UpdateTitle();
    void UpdateStatus();
    void SetStatus(int part, const std::wstring& text);

    // playback (active model)
    void RefreshTransport();
    void SelectClip(int clip);
    void SetTime(double t);
    void SetPlaying(bool p);
    void StepFrame(int dir);
    void UpdateTimeline();
    void StartTransition(Model& m, bool advance, double fromTime, double fromEnd);
    bool JumpsAt(const Model& m, double before, double after) const;
    bool AnyBlending() const;

    // camera
    scene::Bounds CurrentBounds() const;
    static scene::Bounds FrameBoundsAll(const Tab& t);
    void FrameBounds(Tab& t, const scene::Bounds& b);
    void ResetView();
    void Pick(int x, int y);
};

App* g_app = nullptr;

// ---- creation --------------------------------------------------------------------------------

bool App::Create(HINSTANCE inst, int show, const std::vector<std::wstring>& initialFiles) {
    inst_ = inst;
    g_app = this;
    tab_ = NewTab(false);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES |
                                                 ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"USDZViewerMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW vc = {sizeof(vc)};
    vc.style = CS_DBLCLKS;
    vc.lpfnWndProc = ViewportProc;
    vc.hInstance = inst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = L"USDZViewerViewport";
    RegisterClassExW(&vc);

    WNDCLASSEXW ic = {sizeof(ic)};
    ic.lpfnWndProc = InfoProc;
    ic.hInstance = inst;
    ic.hIcon = icon;
    ic.hIconSm = icon;
    ic.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ic.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    ic.lpszClassName = L"USDZViewerInfo";
    RegisterClassExW(&ic);

    WNDCLASSEXW tc = {sizeof(tc)};
    tc.style = CS_DBLCLKS;
    tc.lpfnWndProc = TabsProc;
    tc.hInstance = inst;
    tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tc.lpszClassName = L"USDZViewerTabs";
    RegisterClassExW(&tc);

    dpi_ = GetDpiForSystem();
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"USDZ Viewer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, S(1440), S(900), nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    CreateFonts();
    CreateMenus();
    CreateControls();
    DragAcceptFiles(hwnd_, TRUE);

    std::string err;
    rendererOk_ = renderer_.Init(viewport_, err);
    if (!rendererOk_) {
        MessageBoxW(hwnd_, Utf8ToWide("Could not initialize Direct3D 11:\n" + err).c_str(), L"USDZ Viewer", MB_ICONERROR);
        return false;
    }
    options_.uiScale = float(dpi_) / 96.0f;
    ResetView();
    UpdateMenuChecks();
    RefreshTransport();
    UpdateStatus();
    Layout();
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    if (!initialFiles.empty()) OpenFiles(initialFiles, false);
    return true;
}

void App::CreateFonts() {
    if (font_) DeleteObject(font_);
    if (monoFont_) DeleteObject(monoFont_);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    if (iconFont_) DeleteObject(iconFont_);
    iconFont_ = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    LOGFONTW mono = {};
    mono.lfHeight = -MulDiv(10, int(dpi_), 72);
    mono.lfWeight = FW_NORMAL;
    mono.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(mono.lfFaceName, L"Consolas");
    monoFont_ = CreateFontIndirectW(&mono);
}

void App::CreateMenus() {
    menu_ = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_ADDITIONAL, L"Open &Additional...\tCtrl+Shift+O");
    AppendMenuW(file, MF_STRING, ID_FILE_RELOAD, L"&Reload\tF5");
    AppendMenuW(file, MF_STRING, ID_FILE_CLOSE_MODEL, L"&Close Model\tCtrl+W");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_NEW_TAB, L"New &Tab\tCtrl+T");
    AppendMenuW(file, MF_STRING, ID_FILE_CLOSE_TAB, L"Close Ta&b\tCtrl+F4");
    AppendMenuW(file, MF_STRING, ID_FILE_CLOSE_OTHER_TABS, L"Close &Other Tabs");
    AppendMenuW(file, MF_STRING, ID_FILE_NEXT_TAB, L"Ne&xt Tab\tCtrl+Tab");
    AppendMenuW(file, MF_STRING, ID_FILE_PREV_TAB, L"Pre&vious Tab\tCtrl+Shift+Tab");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_PREV, L"&Previous File in Folder\tPage Up");
    AppendMenuW(file, MF_STRING, ID_FILE_NEXT, L"&Next File in Folder\tPage Down");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"E&xit\tAlt+F4");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_VIEW_FRAME, L"&Frame Scene\tF");
    AppendMenuW(view, MF_STRING, ID_VIEW_RESET_CAMERA, L"&Reset Camera\tR");
    AppendMenuW(view, MF_STRING, ID_VIEW_FOLLOW, L"Follow &Animation\tT");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_HIERARCHY, L"&Hierarchy Panel\tCtrl+H");
    AppendMenuW(view, MF_STRING, ID_VIEW_GRID, L"&Grid\tG");
    AppendMenuW(view, MF_STRING, ID_VIEW_SHADOW, L"Ground Shado&w\tS");
    AppendMenuW(view, MF_STRING, ID_VIEW_SKELETON, L"S&keleton\tK");
    AppendMenuW(view, MF_STRING, ID_VIEW_RULER, L"R&uler (measure size)\tM");
    AppendMenuW(view, MF_STRING, ID_VIEW_AXES, L"&Axis Gizmo");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_SHADED, L"&Shaded");
    AppendMenuW(view, MF_STRING, ID_VIEW_WIREFRAME, L"&Wireframe");
    AppendMenuW(view, MF_STRING, ID_VIEW_SHADED_WIRE, L"Shaded + Wire&frame\t(W cycles)");
    AppendMenuW(view, MF_STRING, ID_VIEW_BACKFACES, L"Show &Back Faces\tB");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_LIT, L"&Lit\t1");
    AppendMenuW(view, MF_STRING, ID_VIEW_BASECOLOR, L"Base &Color\t2");
    AppendMenuW(view, MF_STRING, ID_VIEW_NORMALS, L"&Normals\t3");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_EXPOSURE_UP, L"Brighter\t+");
    AppendMenuW(view, MF_STRING, ID_VIEW_EXPOSURE_DOWN, L"Darker\t-");
    AppendMenuW(view, MF_STRING, ID_VIEW_EXPOSURE_RESET, L"Reset Exposure\t0");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_STEADY, L"Steady &Frame Rate (prevents G-SYNC flicker)");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_VIEW_CLEAR_SELECTION, L"Clear Selection\tEsc");
    AppendMenuW(view, MF_STRING, ID_VIEW_INFO, L"Scene &Info...\tCtrl+I");

    HMENU anim = CreatePopupMenu();
    AppendMenuW(anim, MF_STRING, ID_ANIM_PLAY, L"&Play / Pause\tSpace");
    AppendMenuW(anim, MF_STRING, ID_ANIM_PREV_FRAME, L"Previous &Frame\tLeft");
    AppendMenuW(anim, MF_STRING, ID_ANIM_NEXT_FRAME, L"&Next Frame\tRight");
    AppendMenuW(anim, MF_STRING, ID_ANIM_START, L"Go to &Start\tHome");
    AppendMenuW(anim, MF_STRING, ID_ANIM_END, L"Go to &End\tEnd");
    AppendMenuW(anim, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(anim, MF_STRING, ID_ANIM_PREV_CLIP, L"Previous &Clip\tUp");
    AppendMenuW(anim, MF_STRING, ID_ANIM_NEXT_CLIP, L"Next C&lip\tDown");
    AppendMenuW(anim, MF_STRING, ID_ANIM_LOOP, L"&Loop\tL");
    HMENU speed = CreatePopupMenu();
    for (int i = 0; i < int(std::size(kSpeeds)); ++i) AppendMenuW(speed, MF_STRING, ID_ANIM_SPEED + UINT(i), kSpeedLabels[i]);
    AppendMenuW(anim, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"Spee&d");
    HMENU blend = CreatePopupMenu();
    for (int i = 0; i < int(std::size(kBlendSeconds)); ++i)
        AppendMenuW(blend, MF_STRING, ID_ANIM_BLEND + UINT(i), kBlendLabels[i]);
    AppendMenuW(anim, MF_POPUP, reinterpret_cast<UINT_PTR>(blend), L"Clip &Blending");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_CONTROLS, L"&Controls\tF1");
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"&About USDZ Viewer");

    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(anim), L"&Animation");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(hwnd_, menu_);

    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'O', ID_FILE_OPEN},
        {FCONTROL | FSHIFT | FVIRTKEY, 'O', ID_FILE_OPEN_ADDITIONAL},
        {FCONTROL | FVIRTKEY, 'W', ID_FILE_CLOSE_MODEL},
        {FCONTROL | FVIRTKEY, 'T', ID_FILE_NEW_TAB},
        {FCONTROL | FVIRTKEY, VK_F4, ID_FILE_CLOSE_TAB},
        {FCONTROL | FVIRTKEY, VK_TAB, ID_FILE_NEXT_TAB},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, ID_FILE_PREV_TAB},
        {FCONTROL | FVIRTKEY, VK_NEXT, ID_FILE_NEXT_TAB},
        {FCONTROL | FVIRTKEY, VK_PRIOR, ID_FILE_PREV_TAB},
        {FCONTROL | FVIRTKEY, '1', ID_TAB_SELECT + 0},
        {FCONTROL | FVIRTKEY, '2', ID_TAB_SELECT + 1},
        {FCONTROL | FVIRTKEY, '3', ID_TAB_SELECT + 2},
        {FCONTROL | FVIRTKEY, '4', ID_TAB_SELECT + 3},
        {FCONTROL | FVIRTKEY, '5', ID_TAB_SELECT + 4},
        {FCONTROL | FVIRTKEY, '6', ID_TAB_SELECT + 5},
        {FCONTROL | FVIRTKEY, '7', ID_TAB_SELECT + 6},
        {FCONTROL | FVIRTKEY, '8', ID_TAB_SELECT + 7},
        {FCONTROL | FVIRTKEY, '9', ID_TAB_SELECT + 8},
        {FVIRTKEY, VK_F5, ID_FILE_RELOAD},
        {FVIRTKEY, VK_PRIOR, ID_FILE_PREV},
        {FVIRTKEY, VK_NEXT, ID_FILE_NEXT},
        {FVIRTKEY, 'F', ID_VIEW_FRAME},
        {FVIRTKEY, 'R', ID_VIEW_RESET_CAMERA},
        {FVIRTKEY, 'T', ID_VIEW_FOLLOW},
        {FCONTROL | FVIRTKEY, 'H', ID_VIEW_HIERARCHY},
        {FVIRTKEY, 'G', ID_VIEW_GRID},
        {FVIRTKEY, 'S', ID_VIEW_SHADOW},
        {FVIRTKEY, 'K', ID_VIEW_SKELETON},
        {FVIRTKEY, 'M', ID_VIEW_RULER},
        {FVIRTKEY, 'W', ID_VIEW_CYCLE_WIRE},
        {FVIRTKEY, 'B', ID_VIEW_BACKFACES},
        {FVIRTKEY, '1', ID_VIEW_LIT},
        {FVIRTKEY, '2', ID_VIEW_BASECOLOR},
        {FVIRTKEY, '3', ID_VIEW_NORMALS},
        {FVIRTKEY, VK_OEM_PLUS, ID_VIEW_EXPOSURE_UP},
        {FVIRTKEY, VK_ADD, ID_VIEW_EXPOSURE_UP},
        {FVIRTKEY, VK_OEM_MINUS, ID_VIEW_EXPOSURE_DOWN},
        {FVIRTKEY, VK_SUBTRACT, ID_VIEW_EXPOSURE_DOWN},
        {FVIRTKEY, '0', ID_VIEW_EXPOSURE_RESET},
        {FCONTROL | FVIRTKEY, 'I', ID_VIEW_INFO},
        {FVIRTKEY, VK_ESCAPE, ID_VIEW_CLEAR_SELECTION},
        {FVIRTKEY, VK_SPACE, ID_ANIM_PLAY},
        {FVIRTKEY, VK_LEFT, ID_ANIM_PREV_FRAME},
        {FVIRTKEY, VK_RIGHT, ID_ANIM_NEXT_FRAME},
        {FVIRTKEY, VK_HOME, ID_ANIM_START},
        {FVIRTKEY, VK_END, ID_ANIM_END},
        {FVIRTKEY, VK_UP, ID_ANIM_PREV_CLIP},
        {FVIRTKEY, VK_DOWN, ID_ANIM_NEXT_CLIP},
        {FVIRTKEY, VK_OEM_4, ID_ANIM_PREV_CLIP},
        {FVIRTKEY, VK_OEM_6, ID_ANIM_NEXT_CLIP},
        {FVIRTKEY, 'L', ID_ANIM_LOOP},
        {FVIRTKEY, VK_F1, ID_HELP_CONTROLS},
    };
    accel_ = CreateAcceleratorTableW(acc, int(std::size(acc)));
}

void App::CreateControls() {
    const DWORD child = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    tabStrip_ = CreateWindowExW(0, L"USDZViewerTabs", L"", child, 0, 0, 10, 10, hwnd_, nullptr, inst_, nullptr);
    viewport_ =CreateWindowExW(0, L"USDZViewerViewport", L"", child | WS_TABSTOP, 0, 0, 10, 10, hwnd_, nullptr, inst_, nullptr);

    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"", child | WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT |
                                                      TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_TREE), inst_, nullptr);
    SetWindowLongPtrW(tree_, GWL_STYLE, GetWindowLongPtrW(tree_, GWL_STYLE) | TVS_CHECKBOXES);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    SetWindowTheme(tree_, L"Explorer", nullptr);

    props_ = CreateWindowExW(0, WC_LISTVIEWW, L"", child | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_PROPS), inst_, nullptr);
    ListView_SetExtendedListViewStyle(props_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(props_, L"Explorer", nullptr);
    const wchar_t* cols[] = {L"Property", L"Type", L"Value"};
    const int widths[] = {130, 70, 300};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = S(widths[i]);
        ListView_InsertColumn(props_, i, &c);
    }

    playBtn_ = CreateWindowExW(0, L"BUTTON", L"Play", child | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(IDC_PLAY), inst_, nullptr);
    clipCombo_ = CreateWindowExW(0, WC_COMBOBOXW, L"", child | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 10, S(300),
                                 hwnd_, reinterpret_cast<HMENU>(IDC_CLIP), inst_, nullptr);
    timeline_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", child | WS_TABSTOP | TBS_HORZ | TBS_BOTTOM, 0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(IDC_TIMELINE), inst_, nullptr);
    timeLabel_ = CreateWindowExW(0, L"STATIC", L"", child | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(IDC_TIME), inst_, nullptr);
    speedCombo_ = CreateWindowExW(0, WC_COMBOBOXW, L"", child | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 10, S(300), hwnd_,
                                  reinterpret_cast<HMENU>(IDC_SPEED), inst_, nullptr);
    for (const wchar_t* s : kSpeedLabels) SendMessageW(speedCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(speedCombo_, CB_SETCURSEL, kDefaultSpeed, 0);
    loopCheck_ = CreateWindowExW(0, L"BUTTON", L"Loop", child | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(IDC_LOOP), inst_, nullptr);
    Button_SetCheck(loopCheck_, BST_CHECKED);
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(IDC_STATUS), inst_, nullptr);

    for (HWND h : {tree_, props_, playBtn_, clipCombo_, timeline_, timeLabel_, speedCombo_, loopCheck_, status_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    FillClips();
}

RECT App::SplitterRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    RECT sb;
    GetWindowRect(status_, &sb);
    const int statusH = sb.bottom - sb.top;
    const int panelW = S(panelWidthDip_);
    return RECT{panelW, S(kTabStripDip), panelW + S(5), rc.bottom - statusH - S(42)};
}

void App::Layout() {
    if (!status_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(status_, &sb);
    const int statusH = sb.bottom - sb.top;
    const int transportH = S(42);
    const int tabsH = S(kTabStripDip);
    const int contentH = std::max(0, int(rc.bottom) - statusH - transportH - tabsH);
    int panelW = showPanel_ ? std::min(S(panelWidthDip_), std::max(0, int(rc.right) - S(200))) : 0;
    const int splitW = showPanel_ ? S(5) : 0;

    HDWP dwp = BeginDeferWindowPos(13);
    auto place = [&](HWND h, int x, int y, int w, int hh, bool visible = true) {
        dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(0, w), std::max(0, hh),
                             SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    };
    const int treeH = contentH * 58 / 100;
    place(tabStrip_, 0, 0, rc.right, tabsH);
    place(tree_, 0, tabsH, panelW, treeH, showPanel_);
    place(props_, 0, tabsH + treeH + S(3), panelW, contentH - treeH - S(3), showPanel_);
    place(viewport_, panelW + splitW, tabsH, rc.right - panelW - splitW, contentH);

    const int y = tabsH + contentH + (transportH - S(26)) / 2;
    int x = S(8);
    place(playBtn_, x, y - S(1), S(76), S(28));
    x += S(84);
    place(clipCombo_, x, y, S(240), S(300));
    x += S(248);
    const int right = rc.right - S(8);
    const int loopW = S(56), speedW = S(70), labelW = S(170);
    place(loopCheck_, right - loopW, y, loopW, S(26));
    place(speedCombo_, right - loopW - S(8) - speedW, y, speedW, S(300));
    place(timeLabel_, right - loopW - speedW - S(16) - labelW, y, labelW, S(26));
    place(timeline_, x, y - S(2), right - loopW - speedW - labelW - S(24) - x, S(30));
    EndDeferWindowPos(dwp);

    const int parts[3] = {S(380), std::max(S(380) + S(200), int(rc.right) - S(330)), -1};
    SendMessageW(status_, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    ListView_SetColumnWidth(props_, 2, std::max(S(120), panelW - S(130) - S(70) - S(24)));
    ScrollTabIntoView(tab_);
    needsRedraw_ = true;
}

// ---- message handling ------------------------------------------------------------------------

LRESULT CALLBACK App::MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app && (g_app->hwnd_ == h || !g_app->hwnd_)) {
        if (!g_app->hwnd_) g_app->hwnd_ = h;
        return g_app->HandleMain(m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::ViewportProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app && g_app->viewport_ == h) return g_app->HandleViewport(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::TabsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app && g_app->tabStrip_ == h) return g_app->HandleTabs(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::InfoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (g_app && g_app->infoEdit_) MoveWindow(g_app->infoEdit_, 0, 0, LOWORD(l), HIWORD(l), TRUE);
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::HandleMain(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize = {S(640), S(420)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            options_.uiScale = float(dpi_) / 96.0f;
            CreateFonts();
            for (HWND h : {tree_, props_, playBtn_, clipCombo_, timeline_, timeLabel_, speedCombo_, loopCheck_, status_})
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            if (infoEdit_) SendMessageW(infoEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont_), TRUE);
            InvalidateRect(tabStrip_, nullptr, FALSE);
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_COMMAND: {
            const UINT id = LOWORD(w), code = HIWORD(w);
            if (id == IDC_PLAY && code == BN_CLICKED) {
                SetPlaying(!tab_->playing);
                SetFocus(viewport_);
            } else if (id == IDC_LOOP && code == BN_CLICKED) {
                loop_ = Button_GetCheck(loopCheck_) == BST_CHECKED;
                UpdateMenuChecks();
                SetFocus(viewport_);
            } else if (id == IDC_CLIP && code == CBN_SELCHANGE) {
                SelectClip(int(SendMessageW(clipCombo_, CB_GETCURSEL, 0, 0)) - 1);
            } else if (id == IDC_CLIP && code == CBN_CLOSEUP) {
                SetFocus(viewport_);
            } else if (id == IDC_SPEED && code == CBN_SELCHANGE) {
                speedIndex_ = std::max(0, int(SendMessageW(speedCombo_, CB_GETCURSEL, 0, 0)));
                UpdateMenuChecks();
            } else if (id == IDC_SPEED && code == CBN_CLOSEUP) {
                SetFocus(viewport_);
            } else if (l == 0 || id >= ID_FILE_OPEN && id < IDC_PLAY) {
                Command(id);
            }
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(l) == timeline_ && tab_->active) {
                const int code = LOWORD(w);
                const int pos = int(SendMessageW(timeline_, TBM_GETPOS, 0, 0));
                if (code == TB_ENDTRACK) {
                    scrubbing_ = false;
                    ShowProperties();
                    SetFocus(viewport_);
                } else {
                    scrubbing_ = true;
                    if (tab_->playing) SetPlaying(false);
                }
                lastTimelinePos_ = pos;
                SetTime(tab_->active->rangeStart + pos / tickScale_);
            }
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (hdr->hwndFrom == tree_) {
                if (hdr->code == TVN_SELCHANGEDW && !fillingTree_) {
                    auto* nm = reinterpret_cast<NMTREEVIEWW*>(l);
                    size_t idx = size_t(nm->itemNew.lParam);
                    if (nm->itemNew.hItem && idx < treeEntries_.size()) SetSelection(treeEntries_[idx].model, treeEntries_[idx].prim);
                    else SetSelection(nullptr, nullptr);
                } else if (hdr->code == TVN_ITEMCHANGEDW && !fillingTree_) {
                    auto* nm = reinterpret_cast<NMTVITEMCHANGE*>(l);
                    if ((nm->uStateNew ^ nm->uStateOld) & TVIS_STATEIMAGEMASK) {
                        const bool checked = ((nm->uStateNew & TVIS_STATEIMAGEMASK) >> 12) == 2;
                        const size_t idx = size_t(nm->lParam);
                        if (idx < treeEntries_.size()) {
                            const TreeEntry& e = treeEntries_[idx];
                            if (!e.prim) {
                                e.model->scene->hidden = !checked;  // the model's own row
                            } else {
                                auto it = e.model->nodeOfPrim.find(e.prim);
                                if (it != e.model->nodeOfPrim.end()) e.model->scene->nodes[size_t(it->second)].userHidden = !checked;
                            }
                            needsEvaluate_ = true;
                        }
                    }
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT sr = SplitterRect();
            if (showPanel_ && PtInRect(&sr, pt)) {
                splitterDrag_ = true;
                SetCapture(hwnd_);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (splitterDrag_) {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                int x = std::clamp(int(GET_X_LPARAM(l)), S(150), std::max(S(150), int(rc.right) - S(250)));
                panelWidthDip_ = MulDiv(x, 96, int(dpi_));
                Layout();
            }
            return 0;
        case WM_LBUTTONUP:
            if (splitterDrag_) {
                splitterDrag_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && reinterpret_cast<HWND>(w) == hwnd_) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                RECT sr = SplitterRect();
                if (showPanel_ && PtInRect(&sr, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            if (pendingLoads_ > 0 && LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
                return TRUE;
            }
            break;
        case WM_DROPFILES: {
            // Dropped files open in new tabs; hold Ctrl to add them to the current tab instead.
            HDROP drop = reinterpret_cast<HDROP>(w);
            const bool additional = GetKeyState(VK_CONTROL) < 0;
            std::vector<std::wstring> files;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t file[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, file, UINT(std::size(file)))) files.push_back(file);
            }
            DragFinish(drop);
            OpenFiles(files, additional);
            SetForegroundWindow(hwnd_);
            return 0;
        }
        case WM_APP_LOADED:
            OnLoaded(std::unique_ptr<LoadResult>(reinterpret_cast<LoadResult*>(l)));
            return 0;
        case WM_ENTERIDLE:
            // A menu or modal dialog is running its own message loop, so Run() is paused. Keep frames
            // coming (Present waits for vsync) and post a no-op so the modal loop goes idle again.
            if (NeedsFrame() || SteadyActive()) {
                PumpFrame();
                PostMessageW(hwnd_, WM_NULL, 0, 0);
            }
            return 0;
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd_, 1, USER_TIMER_MINIMUM, nullptr);  // moving/resizing also runs a modal loop
            return 0;
        case WM_EXITSIZEMOVE:
            KillTimer(hwnd_, 1);
            return 0;
        case WM_TIMER:
            if (w == 1 || w == 2) PumpFrame();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) SetFocus(viewport_);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

LRESULT App::HandleViewport(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (rendererOk_) renderer_.Resize(LOWORD(l), HIWORD(l));
            needsRedraw_ = true;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(viewport_, &ps);
            EndPaint(viewport_, &ps);
            needsRedraw_ = true;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            SetFocus(viewport_);
            const bool shift = (w & MK_SHIFT) != 0, ctrl = (w & MK_CONTROL) != 0;
            if (m == WM_LBUTTONDOWN) dragButton_ = shift ? 2 : ctrl ? 3 : 1;
            else if (m == WM_RBUTTONDOWN) dragButton_ = ctrl ? 3 : 2;
            else dragButton_ = 2;
            dragStart_ = lastMouse_ = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            dragMoved_ = false;
            SetCapture(viewport_);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragButton_) {
                POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                const double dx = p.x - lastMouse_.x, dy = p.y - lastMouse_.y;
                lastMouse_ = p;
                if (std::abs(p.x - dragStart_.x) + std::abs(p.y - dragStart_.y) > 3) dragMoved_ = true;
                RECT rc;
                GetClientRect(viewport_, &rc);
                const double h = std::max<LONG>(1, rc.bottom);
                if (dragButton_ == 1) {
                    tab_->camera.yaw -= dx * 0.008;
                    tab_->camera.pitch = std::clamp(tab_->camera.pitch + dy * 0.008, -1.55, 1.55);
                } else if (dragButton_ == 2) {
                    const double wpp = 2.0 * tab_->camera.distance * std::tan(tab_->camera.fovY / 2) / h;
                    tab_->camera.target = tab_->camera.target - tab_->camera.Right() * (dx * wpp) + tab_->camera.Up() * (dy * wpp);
                } else {
                    tab_->camera.distance = std::max(1e-5, tab_->camera.distance * std::pow(1.01, dy));
                }
                needsRedraw_ = true;
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (dragButton_) {
                if (m == WM_LBUTTONUP && dragButton_ == 1 && !dragMoved_) Pick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                dragButton_ = 0;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            dragButton_ = 0;
            return 0;
        case WM_LBUTTONDBLCLK:
            Command(ID_VIEW_FRAME);
            return 0;
        case WM_MOUSEWHEEL: {
            const double steps = GET_WHEEL_DELTA_WPARAM(w) / double(WHEEL_DELTA);
            tab_->camera.distance = std::max(1e-5, tab_->camera.distance * std::pow(0.87, steps));
            needsRedraw_ = true;
            return 0;
        }
        case WM_SETCURSOR:
            if (pendingLoads_ > 0 && LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
                return TRUE;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(viewport_, m, w, l);
}

bool App::PreTranslate(MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    HWND focus = GetFocus();
    if (focus && GetAncestor(focus, GA_ROOT) != hwnd_) return false;  // e.g. the scene info window
    const WPARAM vk = msg.wParam;
    const bool tabKey = GetKeyState(VK_CONTROL) < 0 && (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_TAB);
    const bool nav = !tabKey && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME ||
                                 vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_SPACE);
    if (nav && (focus == tree_ || focus == props_ || focus == clipCombo_ || focus == speedCombo_ || focus == timeline_))
        return false;
    return TranslateAcceleratorW(hwnd_, accel_, &msg) != 0;
}

bool App::NeedsFrame() const {
    if (IsIconic(hwnd_)) return false;
    return tab_->playing || needsRedraw_ || needsEvaluate_ || followDirty_ || dragButton_ != 0 || AnyBlending();
}

bool App::SteadyActive() const {
    return steadyPacing_ && rendererOk_ && !IsIconic(hwnd_) && GetForegroundWindow() == hwnd_;
}

// Renders one frame if anything needs it, or re-presents the last one to keep the pace steady.
void App::PumpFrame() {
    const bool needs = NeedsFrame();
    if (needs || SteadyActive()) Frame(needs);
}

void App::Frame(bool redrawScene) {
    const double now = Now();
    double dt = lastFrameTime_ > 0 ? now - lastFrameTime_ : 0;
    lastFrameTime_ = now;
    dt = std::min(dt, 0.25);

    // Every model advances along its own range (clip) at the shared speed.
    const double blendSeconds = kBlendSeconds[blendIndex_];
    if (tab_->playing) {
        bool running = false;
        for (auto& m : tab_->models) {
            const scene::Scene& sc = *m->scene;
            if (!sc.animated || m->rangeEnd <= m->rangeStart) continue;
            const double advance = dt * sc.timeCodesPerSecond * kSpeeds[speedIndex_];
            Model::Transition& b = m->blend;
            if (b.active && b.fromAdvance && b.started) b.fromTime = std::min(b.fromTime + advance, b.fromEnd);
            const double before = m->time;
            double t = before + advance;
            if (t > m->rangeEnd) {
                if (loop_) {
                    t = m->rangeStart + std::fmod(t - m->rangeStart, m->rangeEnd - m->rangeStart);
                    running = true;
                    // The loop restarts: fade from its last frame, unless it continues seamlessly (or is too short).
                    if (blendSeconds > 0 && m->rangeEnd - m->rangeStart > 2 * blendSeconds * sc.timeCodesPerSecond &&
                        JumpsAt(*m, m->rangeEnd, m->rangeStart))
                        StartTransition(*m, false, m->rangeEnd, m->rangeEnd);
                } else {
                    t = m->rangeEnd;
                }
            } else {
                running = true;
                // Playing the whole timeline: fade from the end of one clip into the next.
                if (m->clip < 0 && blendSeconds > 0) {
                    for (size_t i = 0; i < sc.clips.size(); ++i) {
                        const scene::Clip& c = sc.clips[i];
                        if (before < c.start || before > c.end || t <= c.end) continue;
                        if (JumpsAt(*m, c.end, i + 1 < sc.clips.size() ? std::max(t, sc.clips[i + 1].start) : t))
                            StartTransition(*m, false, c.end, c.end);
                        break;
                    }
                }
            }
            m->time = t;
        }
        needsEvaluate_ = true;
        if (!running) SetPlaying(false);
    }
    if (needsEvaluate_ || AnyBlending()) {
        // Fades run in animation time while playing (they slow down with the playback speed), in real time otherwise.
        const double rate = tab_->playing ? kSpeeds[speedIndex_] : 1.0;
        for (auto& m : tab_->models) {
            Model::Transition& b = m->blend;
            if (b.active) {
                if (b.started) b.elapsed += dt * rate;
                b.started = true;
                if (b.elapsed >= b.duration) b.active = false;
            }
            if (b.active) {
                const double u = b.elapsed / b.duration, w = u * u * (3 - 2 * u);
                if (b.snapshot) m->scene->EvaluateBlend(b.fromPose, m->time, w);
                else m->scene->EvaluateBlend(b.fromTime, m->time, w);
            } else {
                m->scene->Evaluate(m->time);
            }
        }
        UpdateTimeline();
        if (follow_) followDirty_ = true;
    }
    needsEvaluate_ = false;

    // Follow mode: ease the orbit target towards the active model's center.
    if (tab_->active && follow_ && followDirty_) {
        scene::Bounds b = tab_->active->scene->ComputeBounds();
        followDirty_ = false;
        if (!b.IsEmpty()) {
            const scene::Vec3 c = b.Center();
            const double k = dt > 0 ? 1.0 - std::exp(-dt * 8.0) : 1.0;
            const scene::Vec3 delta = c - tab_->camera.target;
            tab_->camera.target = tab_->camera.target + delta * k;
            if (delta.Length() > 1e-4 * std::max(1e-3, b.Radius())) followDirty_ = true;  // keep easing
        }
    }

    // Ruler: the selected part, the selected model, or every visible model, as currently posed.
    if (options_.ruler && (redrawScene || needsRedraw_)) {
        options_.rulers.clear();
        statusRuler_ = {};
        std::vector<char> mask;
        if (tab_->selModel && tab_->selPrim) {
            for (auto& mesh : tab_->selModel->scene->meshes) mask.push_back(mesh.highlighted ? 1 : 0);
            const bool hasGeometry = std::find(mask.begin(), mask.end(), 1) != mask.end();
            options_.rulers.push_back({tab_->selModel->scene->ComputeBounds(hasGeometry ? &mask : nullptr), tab_->selModel->MetersPerUnit()});
            statusRuler_ = options_.rulers.back();
        } else {
            for (auto& m : tab_->models) {
                if (m->scene->hidden || (tab_->selModel && m.get() != tab_->selModel)) continue;
                options_.rulers.push_back({m->scene->ComputeBounds(), m->MetersPerUnit()});
                if (m.get() == tab_->active) statusRuler_ = options_.rulers.back();
            }
        }
    }

    // Only the current tab's models are drawn, on its grid and shadow.
    std::vector<const scene::Scene*> shown;
    for (auto& m : tab_->models) shown.push_back(m->scene.get());
    if (rendererOk_) renderer_.ShowScenes(shown);
    options_.shadowBounds = tab_->allAnimBounds;
    options_.gridCell = tab_->gridCell;
    options_.gridExtent = tab_->gridExtent;
    options_.gridHeight = tab_->gridHeight;

    // Clip planes follow the camera distance and the scene size.
    const double radius = tab_->allAnimBounds.IsEmpty() ? 1.0 : std::max(tab_->allAnimBounds.Radius(), 1e-4);
    tab_->camera.nearClip = std::max(tab_->camera.distance * 0.004, radius * 1e-5);
    tab_->camera.farClip = tab_->camera.distance * 20 + radius * 8;
    if (rendererOk_) renderer_.Render(tab_->camera, options_, redrawScene || needsRedraw_);
    needsRedraw_ = false;
    if (rendererOk_ && renderer_.DeviceLost()) {
        rendererOk_ = false;
        MessageBoxW(hwnd_, L"The graphics device was removed or reset. Please restart USDZ Viewer.", L"USDZ Viewer",
                    MB_ICONERROR);
    }

    ++frames_;
    if (now - fpsTime_ >= 0.5) {
        fps_ = frames_ / (now - fpsTime_);
        frames_ = 0;
        fpsTime_ = now;
        UpdateStatus();
    }
}

// A message box during which the viewport keeps animating: its modal loop does not reach Run(), and it does not
// reliably send WM_ENTERIDLE, so a timer drives the frames.
int App::Message(const std::wstring& text, UINT flags, const wchar_t* title) {
    if (modalDepth_++ == 0) SetTimer(hwnd_, 2, USER_TIMER_MINIMUM, nullptr);
    const int result = MessageBoxW(hwnd_, text.c_str(), title, flags);
    if (--modalDepth_ == 0) KillTimer(hwnd_, 2);
    return result;
}

int App::Run() {
    MSG msg;
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return int(msg.wParam);
            if (!PreTranslate(msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (NeedsFrame() || SteadyActive()) {
            PumpFrame();  // Present() waits for the next refresh, which paces this loop
        } else {
            lastFrameTime_ = 0;
            WaitMessage();
        }
    }
}

// ---- commands --------------------------------------------------------------------------------

void App::Command(UINT id) {
    switch (id) {
        case ID_FILE_OPEN: ShowOpenDialog(false); break;
        case ID_FILE_OPEN_ADDITIONAL: ShowOpenDialog(true); break;
        case ID_FILE_CLOSE_MODEL:
            // Closing a tab's last model closes the tab (unless it is the only one).
            if (tab_->models.size() <= 1 && tab_->pendingLoads == 0 && tabs_.size() > 1) CloseTab(tab_);
            else if (tab_->active) CloseModel(tab_->active);
            break;
        case ID_FILE_RELOAD:
            if (tab_->active) RequestLoad(*tab_, tab_->active->path, tab_->active, true);
            break;
        case ID_FILE_NEW_TAB: NewTab(true); break;
        case ID_FILE_CLOSE_TAB: CloseTab(tab_); break;
        case ID_FILE_CLOSE_OTHER_TABS: {
            std::vector<Tab*> others;
            for (auto& t : tabs_)
                if (t.get() != tab_) others.push_back(t.get());
            for (Tab* t : others) CloseTab(t);
            break;
        }
        case ID_FILE_NEXT_TAB:
        case ID_FILE_PREV_TAB: {
            const int n = int(tabs_.size());
            SwitchTab(tabs_[size_t(((TabIndex(tab_) + (id == ID_FILE_NEXT_TAB ? 1 : -1)) % n + n) % n)].get());
            break;
        }
        case ID_FILE_PREV: OpenSibling(-1); break;
        case ID_FILE_NEXT: OpenSibling(+1); break;
        case ID_FILE_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
        case ID_VIEW_FRAME: FrameBounds(*tab_, CurrentBounds()); break;
        case ID_VIEW_RESET_CAMERA: ResetView(); break;
        case ID_VIEW_FOLLOW:
            follow_ = !follow_;
            followDirty_ = follow_;
            break;
        case ID_VIEW_HIERARCHY:
            showPanel_ = !showPanel_;
            Layout();
            break;
        case ID_VIEW_GRID: options_.grid = !options_.grid; break;
        case ID_VIEW_SHADOW: options_.groundShadow = !options_.groundShadow; break;
        case ID_VIEW_STEADY: steadyPacing_ = !steadyPacing_; break;
        case ID_VIEW_RULER:
            options_.ruler = !options_.ruler;
            UpdateStatus();
            break;
        case ID_VIEW_SKELETON: options_.skeleton = !options_.skeleton; break;
        case ID_VIEW_AXES: options_.axes = !options_.axes; break;
        case ID_VIEW_SHADED: options_.wireframe = 0; break;
        case ID_VIEW_WIREFRAME: options_.wireframe = 1; break;
        case ID_VIEW_SHADED_WIRE: options_.wireframe = 2; break;
        case ID_VIEW_CYCLE_WIRE: options_.wireframe = (options_.wireframe + 1) % 3; break;
        case ID_VIEW_BACKFACES: options_.showBackfaces = !options_.showBackfaces; break;
        case ID_VIEW_LIT: options_.shading = 0; break;
        case ID_VIEW_BASECOLOR: options_.shading = 1; break;
        case ID_VIEW_NORMALS: options_.shading = 2; break;
        case ID_VIEW_EXPOSURE_UP: options_.exposure = std::min(16.0f, options_.exposure * 1.41421356f); break;
        case ID_VIEW_EXPOSURE_DOWN: options_.exposure = std::max(1.0f / 16, options_.exposure / 1.41421356f); break;
        case ID_VIEW_EXPOSURE_RESET: options_.exposure = 1.0f; break;
        case ID_VIEW_INFO: ShowInfo(); break;
        case ID_VIEW_CLEAR_SELECTION:
            TreeView_SelectItem(tree_, nullptr);
            SetSelection(nullptr, nullptr);
            break;
        case ID_ANIM_PLAY:
            if (AnyAnimated()) SetPlaying(!tab_->playing);
            break;
        case ID_ANIM_PREV_FRAME: StepFrame(-1); break;
        case ID_ANIM_NEXT_FRAME: StepFrame(+1); break;
        case ID_ANIM_START:
            if (tab_->active) {
                SetTime(tab_->active->rangeStart);
                ShowProperties();
            }
            break;
        case ID_ANIM_END:
            if (tab_->active) {
                SetTime(tab_->active->rangeEnd);
                ShowProperties();
            }
            break;
        case ID_ANIM_LOOP:
            loop_ = !loop_;
            Button_SetCheck(loopCheck_, loop_ ? BST_CHECKED : BST_UNCHECKED);
            break;
        case ID_ANIM_PREV_CLIP:
            if (tab_->active && !tab_->active->scene->clips.empty())
                SelectClip(tab_->active->clip <= -1 ? int(tab_->active->scene->clips.size()) - 1 : tab_->active->clip - 1);
            break;
        case ID_ANIM_NEXT_CLIP:
            if (tab_->active && !tab_->active->scene->clips.empty())
                SelectClip(tab_->active->clip + 1 >= int(tab_->active->scene->clips.size()) ? -1 : tab_->active->clip + 1);
            break;
        case ID_HELP_CONTROLS:
            Message(L"Mouse\n"
                    L"   Left drag\tOrbit\n"
                    L"   Right / middle drag, Shift + left drag\tPan\n"
                    L"   Wheel, Ctrl + drag\tZoom\n"
                    L"   Click\tSelect a mesh (and its model)\n"
                    L"   Double-click\tFrame the scene\n\n"
                    L"Animation (the timeline and clips belong to the active model)\n"
                    L"   Space\tPlay / pause all models\n"
                    L"   Left / Right\tPrevious / next frame\n"
                    L"   Home / End\tFirst / last frame\n"
                    L"   Up / Down,  [ / ]\tPrevious / next clip (crossfades; see Animation > Clip Blending)\n"
                    L"   L\tLoop\n\n"
                    L"View\n"
                    L"   F / R\tFrame scene / reset camera\n"
                    L"   T\tFollow the active model\n"
                    L"   G, S, K\tGrid, ground shadow, skeleton\n"
                    L"   M\tRuler (measures the selection, or every model)\n"
                    L"   W\tShaded / wireframe / both\n"
                    L"   B\tShow back faces\n"
                    L"   1, 2, 3\tLit, base color, normals\n"
                    L"   + / - / 0\tExposure\n"
                    L"   Ctrl+H, Ctrl+I\tHierarchy panel, scene info\n\n"
                    L"Files\n"
                    L"   Ctrl+O, drag and drop\tOpen in a new tab\n"
                    L"   Ctrl+Shift+O, Ctrl + drop\tAdd models to the current tab\n"
                    L"   Ctrl+W\tClose the active model (a tab's last model: the tab)\n"
                    L"   Page Up / Page Down\tReplace with the previous / next file in the folder\n"
                    L"   F5\tReload the active model\n\n"
                    L"Tabs\n"
                    L"   Ctrl+T, Ctrl+F4\tNew tab, close tab (or middle-click it)\n"
                    L"   Ctrl+Tab, Ctrl+Shift+Tab\tNext / previous tab (also Ctrl+Page Down / Up)\n"
                    L"   Ctrl+1 … Ctrl+9\tGo to a tab (Ctrl+9: the last one)\n"
                    L"   Drag a tab\tReorder",
                    MB_OK, L"USDZ Viewer Controls");
            break;
        case ID_HELP_ABOUT: {
            std::wstring text = L"USDZ Viewer 1.0\n\nA native viewer for USD files (.usdz, .usdc, .usda, .usd) and "
                                L"their animations: transforms, skeletal (UsdSkel) skinning, blend shapes and point "
                                L"animation, with UsdPreviewSurface materials.\n\nGraphics: " +
                                Utf8ToWide(renderer_.AdapterName()) + L" (Direct3D 11, " +
                                std::to_wstring(renderer_.MsaaSamples()) + L"x MSAA)";
            Message(text, MB_OK | MB_ICONINFORMATION, L"About USDZ Viewer");
            break;
        }
        default:
            if (id >= ID_ANIM_SPEED && id < ID_ANIM_SPEED + std::size(kSpeeds)) {
                speedIndex_ = int(id - ID_ANIM_SPEED);
                SendMessageW(speedCombo_, CB_SETCURSEL, WPARAM(speedIndex_), 0);
            } else if (id >= ID_TAB_SELECT && id <= ID_TAB_SELECT + 8) {
                const size_t i = id == ID_TAB_SELECT + 8 ? tabs_.size() - 1 : size_t(id - ID_TAB_SELECT);
                if (i < tabs_.size()) SwitchTab(tabs_[i].get());
            } else if (id >= ID_ANIM_BLEND && id < ID_ANIM_BLEND + std::size(kBlendSeconds)) {
                blendIndex_ = int(id - ID_ANIM_BLEND);
                if (kBlendSeconds[blendIndex_] <= 0)
                    for (auto& m : tab_->models) m->blend.active = false;
                needsEvaluate_ = true;
            }
            break;
    }
    UpdateMenuChecks();
    needsRedraw_ = true;
}

void App::UpdateMenuChecks() {
    auto check = [&](UINT id, bool on) { CheckMenuItem(menu_, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    check(ID_VIEW_HIERARCHY, showPanel_);
    check(ID_VIEW_FOLLOW, follow_);
    check(ID_VIEW_GRID, options_.grid);
    check(ID_VIEW_SHADOW, options_.groundShadow);
    check(ID_VIEW_STEADY, steadyPacing_);
    check(ID_VIEW_RULER, options_.ruler);
    check(ID_VIEW_SKELETON, options_.skeleton);
    check(ID_VIEW_AXES, options_.axes);
    check(ID_VIEW_BACKFACES, options_.showBackfaces);
    check(ID_ANIM_LOOP, loop_);
    CheckMenuRadioItem(menu_, ID_VIEW_SHADED, ID_VIEW_SHADED_WIRE, ID_VIEW_SHADED + UINT(options_.wireframe), MF_BYCOMMAND);
    CheckMenuRadioItem(menu_, ID_VIEW_LIT, ID_VIEW_NORMALS, ID_VIEW_LIT + UINT(options_.shading), MF_BYCOMMAND);
    CheckMenuRadioItem(menu_, ID_ANIM_SPEED, ID_ANIM_SPEED + UINT(std::size(kSpeeds)) - 1, ID_ANIM_SPEED + UINT(speedIndex_),
                       MF_BYCOMMAND);
    CheckMenuRadioItem(menu_, ID_ANIM_BLEND, ID_ANIM_BLEND + UINT(std::size(kBlendSeconds)) - 1,
                       ID_ANIM_BLEND + UINT(blendIndex_), MF_BYCOMMAND);
    const UINT needModel = MF_BYCOMMAND | (tab_->active ? MF_ENABLED : MF_GRAYED);
    for (UINT id : {ID_FILE_CLOSE_MODEL, ID_FILE_RELOAD, ID_FILE_PREV, ID_FILE_NEXT}) EnableMenuItem(menu_, id, needModel);
    const UINT needTabs = MF_BYCOMMAND | (tabs_.size() > 1 ? MF_ENABLED : MF_GRAYED);
    for (UINT id : {ID_FILE_CLOSE_OTHER_TABS, ID_FILE_NEXT_TAB, ID_FILE_PREV_TAB}) EnableMenuItem(menu_, id, needTabs);
}

// ---- tabs ------------------------------------------------------------------------------------

Tab* App::NewTab(bool show) {
    auto t = std::make_unique<Tab>();
    t->id = nextTabId_++;
    t->camera.yaw = 0.5;
    t->camera.pitch = 0.22;
    Tab* added = t.get();
    tabs_.push_back(std::move(t));
    if (show) SwitchTab(added);
    else UpdateTabs();
    return added;
}

// Shows a tab: its models, hierarchy, transport state and camera. Other tabs keep their state (and their
// models stay on the GPU) but do not play while hidden.
void App::SwitchTab(Tab* t) {
    if (!t) return;
    if (t != tab_) {
        if (GetCapture() == viewport_) ReleaseCapture();
        tab_ = t;
    }
    FillTree();
    UpdateHighlight();
    SetActive(tab_->active);
    SetPlaying(tab_->playing);
    if (infoWnd_ && IsWindowVisible(infoWnd_)) ShowInfo(false);
    ScrollTabIntoView(tab_);
    UpdateTabs();
    followDirty_ = follow_;
    needsEvaluate_ = true;
    needsRedraw_ = true;
}

void App::CloseTab(Tab* t) {
    const int index = TabIndex(t);
    if (index < 0 || (tabs_.size() == 1 && t->IsEmpty())) return;
    for (auto& m : t->models) renderer_.RemoveScene(m->scene.get());
    if (tabHover_ == t) tabHover_ = nullptr;
    if (tabPressClose_ == t) tabPressClose_ = nullptr;
    if (tabDrag_ == t) tabDrag_ = nullptr;
    if (treeTab_ == t) treeTab_ = nullptr;
    std::unique_ptr<Tab> closing = std::move(tabs_[size_t(index)]);  // its pending loads are dropped on arrival
    tabs_.erase(tabs_.begin() + index);
    if (tabs_.empty()) NewTab(false);
    if (tab_ == t) {
        tab_ = nullptr;
        SwitchTab(tabs_[std::min(size_t(index), tabs_.size() - 1)].get());  // the tab to the right, else the left
    } else {
        ScrollTabIntoView(tab_);
        UpdateTabs();
    }
}

Tab* App::FindTab(uint64_t id) const {
    for (auto& t : tabs_)
        if (t->id == id) return t.get();
    return nullptr;
}

int App::TabIndex(const Tab* t) const {
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i].get() == t) return int(i);
    return -1;
}

std::wstring App::TabTitle(const Tab& t) const {
    auto stem = [](const std::wstring& path) {
        std::wstring name = FileName(path);
        const size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    };
    if (!t.models.empty()) {
        std::wstring s = stem(t.models.front()->path);
        if (t.models.size() > 1) s += L"  +" + std::to_wstring(t.models.size() - 1);
        return s;
    }
    if (t.pendingLoads > 0) return L"Loading " + stem(t.loadingName) + L"…";
    return L"New Tab";
}

// Tabs share the strip equally, between a minimum and a maximum width; when they do not fit, the strip scrolls.
void App::TabGeometry(int& width, int& area) const {
    RECT rc;
    GetClientRect(tabStrip_, &rc);
    const int n = std::max<int>(1, int(tabs_.size()));
    area = std::max(0, int(rc.right) - S(6) - S(40));  // room left of the new-tab button
    width = std::clamp(area / n, S(120), S(220));
}

std::vector<App::TabRects> App::LayoutTabs(int* tabsRight) const {
    RECT rc;
    GetClientRect(tabStrip_, &rc);
    int w = 0, area = 0;
    TabGeometry(w, area);
    const int left = S(6), top = S(5), n = int(tabs_.size());
    std::vector<TabRects> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        RECT& r = out[size_t(i)].tab;
        r = {left + i * w - tabScroll_, top, left + (i + 1) * w - tabScroll_, rc.bottom};
        const int size = S(20), y = (r.top + r.bottom - size) / 2;
        out[size_t(i)].close = {r.right - S(7) - size, y, r.right - S(7), y + size};
    }
    if (tabsRight) *tabsRight = left + std::min(n * w, area);
    return out;
}

RECT App::NewTabButtonRect() const {
    RECT rc;
    GetClientRect(tabStrip_, &rc);
    int right = 0;
    LayoutTabs(&right);
    const int size = S(26), y = (S(5) + rc.bottom - size) / 2;
    return {right + S(6), y, right + S(6) + size, y + size};
}

// The tab under `p` (strip coordinates); `part` is 1 over its close button, 2 over the new-tab button.
Tab* App::TabAt(POINT p, int* part) const {
    *part = 0;
    const RECT button = NewTabButtonRect();
    if (PtInRect(&button, p)) {
        *part = 2;
        return nullptr;
    }
    int right = 0;
    const std::vector<TabRects> rects = LayoutTabs(&right);
    if (p.x >= right) return nullptr;
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!PtInRect(&rects[i].tab, p)) continue;
        if (PtInRect(&rects[i].close, p)) *part = 1;
        return tabs_[i].get();
    }
    return nullptr;
}

// Scrolls the strip so that `t` is fully visible (and keeps the scroll offset in range).
void App::ScrollTabIntoView(const Tab* t) {
    if (!tabStrip_) return;
    int w = 0, area = 0;
    TabGeometry(w, area);
    const int index = TabIndex(t);
    if (index >= 0) {
        tabScroll_ = std::min(tabScroll_, index * w);
        tabScroll_ = std::max(tabScroll_, (index + 1) * w - area);
    }
    tabScroll_ = std::clamp(tabScroll_, 0, std::max(0, int(tabs_.size()) * w - area));
    InvalidateRect(tabStrip_, nullptr, FALSE);
}

void App::UpdateTabs() {
    if (!tabStrip_) return;
    InvalidateRect(tabStrip_, nullptr, FALSE);
    UpdateMenuChecks();
}

void App::PaintTabs(HDC target) {
    RECT rc;
    GetClientRect(tabStrip_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bmp);
    auto fill = [&](const RECT& r, COLORREF c) {
        SetDCBrushColor(dc, c);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    };
    auto roundFill = [&](const RECT& r, int radius, COLORREF c) {
        SetDCBrushColor(dc, c);
        SetDCPenColor(dc, c);
        HGDIOBJ b = SelectObject(dc, GetStockObject(DC_BRUSH)), p = SelectObject(dc, GetStockObject(DC_PEN));
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc, b);
        SelectObject(dc, p);
    };
    const COLORREF stripColor = RGB(228, 228, 233), currentColor = RGB(255, 255, 255), hoverColor = RGB(240, 240, 244),
                   lineColor = RGB(204, 204, 211), textColor = RGB(24, 24, 28), dimColor = RGB(86, 86, 94),
                   buttonHover = RGB(212, 212, 219), buttonPressed = RGB(192, 192, 201),
                   accent = GetSysColor(COLOR_HIGHLIGHT);
    fill(rc, stripColor);
    fill({0, rc.bottom - 1, rc.right, rc.bottom}, lineColor);

    int right = 0;
    const std::vector<TabRects> rects = LayoutTabs(&right);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    IntersectClipRect(dc, 0, 0, right, rc.bottom);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const Tab* t = tabs_[i].get();
        const RECT& r = rects[i].tab;
        if (r.right < 0 || r.left > right) continue;
        const bool current = t == tab_, hover = t == tabHover_;
        if (current || hover) {
            // Rounded top corners; the bottom ones fall below the strip.
            roundFill({r.left + S(1), r.top, r.right - S(1), r.bottom + S(10)}, S(10), current ? currentColor : hoverColor);
            if (current) fill({r.left + S(1), rc.bottom - 1, r.right - S(1), rc.bottom}, currentColor);
            if (current) fill({r.left + S(8), r.top, r.right - S(8), r.top + S(2)}, accent);
        } else if (i + 1 < tabs_.size() && tabs_[i + 1].get() != tab_ && tabs_[i + 1].get() != tabHover_) {
            fill({r.right - 1, r.top + S(8), r.right, rc.bottom - S(8)}, lineColor);  // separator
        }
        RECT text = {r.left + S(12), r.top, rects[i].close.left - S(4), rc.bottom};
        SetTextColor(dc, current ? textColor : dimColor);
        const std::wstring title = TabTitle(*t);
        DrawTextW(dc, title.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT close = rects[i].close;
        if (hover && tabHoverPart_ == 1) roundFill(close, S(6), tabPressClose_ == t ? buttonPressed : buttonHover);
        SelectObject(dc, iconFont_);
        DrawTextW(dc, L"\xE8BB", 1, &close, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);  // ChromeClose
        SelectObject(dc, font_);
    }
    SelectClipRgn(dc, nullptr);

    RECT button = NewTabButtonRect();
    if (!tabHover_ && tabHoverPart_ == 2) roundFill(button, S(6), buttonHover);
    SetTextColor(dc, textColor);
    SelectObject(dc, iconFont_);
    DrawTextW(dc, L"\xE710", 1, &button, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);  // Add
    SelectObject(dc, oldFont);

    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void App::ShowTabMenu(POINT screen, Tab* t) {
    enum : UINT { kNew = 1, kClose, kCloseOthers };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kNew, L"New &Tab\tCtrl+T");
    if (t) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kClose, L"&Close Tab\tCtrl+F4");
        AppendMenuW(menu, MF_STRING | (tabs_.size() > 1 ? 0 : MF_GRAYED), kCloseOthers, L"Close &Other Tabs");
    }
    const UINT cmd = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd == kNew) {
        NewTab(true);
    } else if (t && TabIndex(t) >= 0 && cmd == kClose) {
        CloseTab(t);
    } else if (t && TabIndex(t) >= 0 && cmd == kCloseOthers) {
        SwitchTab(t);
        Command(ID_FILE_CLOSE_OTHER_TABS);
    }
}

LRESULT App::HandleTabs(UINT m, WPARAM w, LPARAM l) {
    const POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(tabStrip_, &ps);
            PaintTabs(dc);
            EndPaint(tabStrip_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            ScrollTabIntoView(tab_);
            return 0;
        case WM_MOUSEMOVE: {
            if (!tabTracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, tabStrip_, 0};
                TrackMouseEvent(&tme);
                tabTracking_ = true;
            }
            int part = 0;
            Tab* over = TabAt(p, &part);
            if (tabDrag_ && over && over != tabDrag_) {  // dragging a tab moves it
                const int from = TabIndex(tabDrag_), to = TabIndex(over);
                std::unique_ptr<Tab> moving = std::move(tabs_[size_t(from)]);
                tabs_.erase(tabs_.begin() + from);
                tabs_.insert(tabs_.begin() + to, std::move(moving));
                over = TabAt(p, &part);
            }
            if (over != tabHover_ || part != tabHoverPart_) {
                tabHover_ = over;
                tabHoverPart_ = part;
                InvalidateRect(tabStrip_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tabTracking_ = false;
            tabHover_ = nullptr;
            tabHoverPart_ = 0;
            InvalidateRect(tabStrip_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int part = 0;
            Tab* t = TabAt(p, &part);
            if (part == 2 || (!t && m == WM_LBUTTONDBLCLK)) {  // the + button, or a double-click on empty strip
                NewTab(true);
            } else if (t && part == 1) {
                tabPressClose_ = t;
                SetCapture(tabStrip_);
            } else if (t) {
                SwitchTab(t);
                tabDrag_ = t;
                SetCapture(tabStrip_);
            }
            InvalidateRect(tabStrip_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            Tab* pressed = tabPressClose_;
            tabPressClose_ = nullptr;
            tabDrag_ = nullptr;
            if (GetCapture() == tabStrip_) ReleaseCapture();
            int part = 0;
            if (pressed && TabAt(p, &part) == pressed && part == 1) CloseTab(pressed);
            InvalidateRect(tabStrip_, nullptr, FALSE);
            return 0;
        }
        case WM_MBUTTONUP: {
            int part = 0;
            if (Tab* t = TabAt(p, &part)) CloseTab(t);
            return 0;
        }
        case WM_RBUTTONUP: {
            int part = 0;
            Tab* t = TabAt(p, &part);
            POINT screen = p;
            ClientToScreen(tabStrip_, &screen);
            ShowTabMenu(screen, t);
            return 0;
        }
        case WM_CAPTURECHANGED:
            tabPressClose_ = nullptr;
            tabDrag_ = nullptr;
            InvalidateRect(tabStrip_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            tabScroll_ -= GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA;
            ScrollTabIntoView(nullptr);
            return 0;
    }
    return DefWindowProcW(tabStrip_, m, w, l);
}

// ---- documents -------------------------------------------------------------------------------

// Opens files: `additional` adds them to the current tab; otherwise each opens in a tab of its own (the current
// tab is used while it is still empty).
void App::OpenFiles(const std::vector<std::wstring>& files, bool additional) {
    if (files.empty()) return;
    if (additional) {
        for (const std::wstring& f : files) RequestLoad(*tab_, f, nullptr, false);
        return;
    }
    Tab* first = nullptr;
    for (const std::wstring& f : files) {
        Tab* t = !first && tab_->IsEmpty() ? tab_ : NewTab(false);
        if (!first) first = t;
        RequestLoad(*t, f, nullptr, false);
    }
    SwitchTab(first);
}

void App::RequestLoad(Tab& tab, const std::wstring& path, Model* target, bool keepView) {
    auto r = std::make_unique<LoadResult>();
    r->tabId = tab.id;
    r->path = path;
    r->keepView = keepView;
    if (target) {
        target->ticket = nextTicket_++;
        r->targetId = target->id;
        r->ticket = target->ticket;
        r->order = target->order;
    } else {
        r->order = nextOrder_++;
    }
    ++pendingLoads_;
    ++tab.pendingLoads;
    loadingName_ = FileName(path);
    tab.loadingName = loadingName_;
    UpdateStatus();
    UpdateTabs();
    POINT pt;
    GetCursorPos(&pt);
    SetCursorPos(pt.x, pt.y);  // refresh the cursor shape
    std::thread(LoadWorker, hwnd_, std::move(r)).detach();
}

bool App::AnyAnimated(const Tab& t) {
    for (auto& m : t.models)
        if (m->scene->animated) return true;
    return false;
}

void App::OnLoaded(std::unique_ptr<LoadResult> r) {
    pendingLoads_ = std::max(0, pendingLoads_ - 1);
    POINT pt;
    GetCursorPos(&pt);
    SetCursorPos(pt.x, pt.y);
    Tab* tab = FindTab(r->tabId);
    if (tab) tab->pendingLoads = std::max(0, tab->pendingLoads - 1);
    Model* target = nullptr;
    if (tab && r->targetId)
        for (auto& m : tab->models)
            if (m->id == r->targetId) target = m.get();
    const bool stale = !tab || (r->targetId && (!target || target->ticket != r->ticket));
    if (stale) {  // the tab or the model was closed, or the model was reloaded again since
        UpdateStatus();
        UpdateTabs();
        return;
    }
    if (!r->stage) {
        UpdateStatus();
        UpdateTabs();
        SetStatus(0, L"Failed to open " + FileName(r->path));
        Message(L"Could not open " + r->path + L"\n\n" + Utf8ToWide(r->error), MB_ICONERROR);
        // A tab opened just for this file goes away again (looked up anew: the message box ran a message loop).
        if (Tab* t = FindTab(r->tabId); t && t->IsEmpty() && tabs_.size() > 1) CloseTab(t);
        return;
    }

    auto model = std::make_unique<Model>();
    model->id = nextModelId_++;
    model->path = r->path;
    model->stage = std::move(r->stage);
    model->scene = std::move(r->scene);
    model->animBounds = r->animBounds;
    model->frameBounds = r->frameBounds;
    model->loadSeconds = r->seconds;
    model->order = r->order;
    for (size_t i = 0; i < model->scene->nodes.size(); ++i) model->nodeOfPrim[model->scene->nodes[i].prim] = int(i);
    renderer_.AddScene(model->scene.get(), std::move(r->images));
    for (auto& w : renderer_.TakeWarnings()) model->scene->warnings.push_back(w);

    // Playback state: keep the clip and time on reload, otherwise start at the beginning.
    const bool keep = target && r->keepView && _wcsicmp(target->path.c_str(), r->path.c_str()) == 0;
    model->clip = keep && target->clip < int(model->scene->clips.size()) ? target->clip : -1;
    if (model->clip >= 0) {
        model->rangeStart = model->scene->clips[size_t(model->clip)].start;
        model->rangeEnd = model->scene->clips[size_t(model->clip)].end;
    } else {
        model->rangeStart = model->scene->startTime;
        model->rangeEnd = model->scene->endTime;
    }
    model->time = keep ? std::clamp(target->time, model->rangeStart, std::max(model->rangeStart, model->rangeEnd))
                       : model->rangeStart;

    Model* added = model.get();
    const bool wasEmpty = tab->models.empty();
    if (target) {  // take over the replaced model's place in the list
        auto it = std::find_if(tab->models.begin(), tab->models.end(), [&](const auto& m) { return m.get() == target; });
        renderer_.RemoveScene(target->scene.get());
        if (tab->active == target) tab->active = nullptr;
        if (tab->selModel == target) {
            tab->selModel = nullptr;
            tab->selPrim = nullptr;
        }
        *it = std::move(model);  // destroys the old model after the renderer let go of it
    } else {  // keep the order the files were opened in
        auto pos = std::find_if(tab->models.begin(), tab->models.end(), [&](const auto& m) { return m->order > added->order; });
        tab->models.insert(pos, std::move(model));
    }

    LayoutModels(*tab);
    tab->active = added;
    if (wasEmpty) tab->playing = AnyAnimated(*tab);  // otherwise keep playing (or not); the new model joins in
    if (!(target && r->keepView)) FrameBounds(*tab, FrameBoundsAll(*tab));
    if (tab == tab_) {
        FillTree();
        SetActive(added);
        SetPlaying(tab->playing);
        if (infoWnd_ && IsWindowVisible(infoWnd_)) ShowInfo(false);
        needsEvaluate_ = true;
    }
    UpdateTabs();
}

void App::CloseModel(Model* m) {
    auto it = std::find_if(tab_->models.begin(), tab_->models.end(), [&](const auto& p) { return p.get() == m; });
    if (it == tab_->models.end()) return;
    renderer_.RemoveScene(m->scene.get());
    if (tab_->selModel == m) {
        tab_->selModel = nullptr;
        tab_->selPrim = nullptr;
    }
    const bool wasActive = tab_->active == m;
    tab_->models.erase(it);
    if (wasActive) tab_->active = nullptr;
    LayoutModels(*tab_);
    FillTree();
    SetActive(wasActive ? (tab_->models.empty() ? nullptr : tab_->models.back().get()) : tab_->active);
    if (!AnyAnimated()) SetPlaying(false);
    if (infoWnd_ && IsWindowVisible(infoWnd_)) ShowInfo(false);
    UpdateTabs();
    needsEvaluate_ = true;
}

// Places a tab's models side by side along +X, spaced by their animated extents so they do not collide.
void App::LayoutModels(Tab& t) {
    double widest = 0;
    for (auto& m : t.models) {
        const scene::Bounds& b = m->animBounds.IsEmpty() ? m->frameBounds : m->animBounds;
        if (!b.IsEmpty()) widest = std::max(widest, b.max.x - b.min.x);
    }
    const double gap = std::max(widest * 0.3, 1e-3);
    double cursor = 0;
    bool first = true;
    t.allAnimBounds = {};
    for (auto& m : t.models) {
        const scene::Bounds& b = m->animBounds.IsEmpty() ? m->frameBounds : m->animBounds;
        if (b.IsEmpty() || first) {
            m->offset = {0, 0, 0};
            if (!b.IsEmpty()) {
                cursor = b.max.x + gap;
                first = false;
            }
        } else {
            m->offset = {cursor - b.min.x, 0, 0};
            cursor = m->offset.x + b.max.x + gap;
        }
        m->scene->placement = scene::Mat4::Translate(m->offset.x, m->offset.y, m->offset.z);
        t.allAnimBounds.Add(m->WorldAnimBounds());
    }

    // Ground shadow, grid size and height from all models together.
    const double radius = t.allAnimBounds.IsEmpty() ? 1.0 : std::max(t.allAnimBounds.Radius(), 1e-4);
    double cellSource = 0;  // grid cells sized to the largest model rather than the whole row
    for (auto& m : t.models)
        if (!m->animBounds.IsEmpty()) cellSource = std::max(cellSource, m->animBounds.Radius());
    if (cellSource <= 0) cellSource = radius;
    t.gridCell = std::pow(10.0, std::floor(std::log10(2 * cellSource / 10) + 0.5));
    t.gridExtent = std::max(radius * 12, t.gridCell * 20);
    t.gridHeight = 0;
    if (!t.allAnimBounds.IsEmpty() && (t.allAnimBounds.min.y > radius * 0.25 || t.allAnimBounds.max.y < 0))
        t.gridHeight = t.allAnimBounds.min.y;
    needsEvaluate_ = true;
}

void App::SetActive(Model* m) {
    tab_->active = m;
    // The active model's row is shown in bold.
    for (size_t i = 0; i < treeEntries_.size(); ++i) {
        if (treeEntries_[i].prim) continue;
        std::function<HTREEITEM(HTREEITEM)> find = [&](HTREEITEM item) -> HTREEITEM {
            for (; item; item = TreeView_GetNextSibling(tree_, item)) {
                TVITEMW tv = {};
                tv.mask = TVIF_PARAM;
                tv.hItem = item;
                TreeView_GetItem(tree_, &tv);
                if (size_t(tv.lParam) == i) return item;
            }
            return nullptr;
        };
        if (HTREEITEM item = find(TreeView_GetRoot(tree_))) {
            TVITEMW tv = {};
            tv.mask = TVIF_STATE;
            tv.hItem = item;
            tv.stateMask = TVIS_BOLD;
            tv.state = treeEntries_[i].model == m && tab_->models.size() > 1 ? TVIS_BOLD : 0;
            TreeView_SetItem(tree_, &tv);
        }
    }
    RefreshTransport();
    ShowProperties();
    UpdateTitle();
    UpdateStatus();
    UpdateMenuChecks();
}

void App::ShowOpenDialog(bool additional) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {{L"USD files (*.usdz, *.usdc, *.usda, *.usd)", L"*.usdz;*.usdc;*.usda;*.usd"},
                                   {L"All files (*.*)", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(additional ? L"Open Additional USD Files" : L"Open USD Files");
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (tab_->active) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(DirName(tab_->active->path).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    std::vector<std::wstring> files;
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                PWSTR file = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &file))) {
                    files.push_back(file);
                    CoTaskMemFree(file);
                }
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    OpenFiles(files, additional);
}

// Replaces the active model with the previous/next USD file in its folder.
void App::OpenSibling(int delta) {
    if (!tab_->active) return;
    const std::wstring dir = DirName(tab_->active->path);
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsUsdFile(fd.cFileName)) files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (files.empty()) return;
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    const std::wstring current = FileName(tab_->active->path);
    int idx = -1;
    for (size_t i = 0; i < files.size(); ++i)
        if (_wcsicmp(files[i].c_str(), current.c_str()) == 0) idx = int(i);
    const int n = int(files.size());
    int next = idx < 0 ? 0 : ((idx + delta) % n + n) % n;
    if (next == idx) return;
    RequestLoad(*tab_, dir + L"\\" + files[size_t(next)], tab_->active, false);
}

// Remembers which hierarchy rows are expanded, so they come back when the tree is rebuilt or its tab shown again.
void App::SaveTreeState() {
    if (!treeTab_) return;
    Tab& t = *treeTab_;
    t.expanded.clear();
    t.treeModels.clear();
    for (const TreeEntry& e : treeEntries_) t.treeModels.insert(e.model);
    std::function<void(HTREEITEM)> walk = [&](HTREEITEM item) {
        for (; item; item = TreeView_GetNextSibling(tree_, item)) {
            TVITEMW tv = {};
            tv.mask = TVIF_PARAM | TVIF_STATE;
            tv.hItem = item;
            tv.stateMask = TVIS_EXPANDED;
            TreeView_GetItem(tree_, &tv);
            const size_t idx = size_t(tv.lParam);
            if ((tv.state & TVIS_EXPANDED) && idx < treeEntries_.size())
                t.expanded.insert({treeEntries_[idx].model, treeEntries_[idx].prim});
            walk(TreeView_GetChild(tree_, item));
        }
    };
    walk(TreeView_GetRoot(tree_));
}

void App::FillTree() {
    SaveTreeState();
    treeTab_ = tab_;
    fillingTree_ = true;
    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    treeEntries_.clear();
    HTREEITEM toSelect = nullptr;
    size_t count = 0;
    for (auto& mp : tab_->models) {
        Model* model = mp.get();
        const bool known = tab_->treeModels.count(model) > 0;  // shown before: restore its expanded rows
        auto insert = [&](HTREEITEM parent, const std::wstring& label, const usd::Prim* prim, bool expanded, bool checked) {
            if (known) expanded = tab_->expanded.count({model, prim}) > 0;
            std::wstring text = label;
            treeEntries_.push_back({model, prim});
            TVINSERTSTRUCTW ins = {};
            ins.hParent = parent;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
            ins.item.pszText = text.data();
            ins.item.lParam = LPARAM(treeEntries_.size() - 1);
            ins.item.stateMask = TVIS_STATEIMAGEMASK | TVIS_EXPANDED;
            ins.item.state = INDEXTOSTATEIMAGEMASK(checked ? 2 : 1) | (expanded ? TVIS_EXPANDED : 0);
            HTREEITEM item = TreeView_InsertItem(tree_, &ins);
            if (model == tab_->selModel && prim == tab_->selPrim) toSelect = item;
            return item;
        };
        HTREEITEM root = insert(TVI_ROOT, FileName(model->path), nullptr, true, !model->scene->hidden);
        std::function<void(const usd::Prim*, HTREEITEM, int)> add = [&](const usd::Prim* prim, HTREEITEM parent, int depth) {
            for (auto& c : prim->Children()) {
                if (++count > 100000) return;
                std::wstring text = Utf8ToWide(c->Name());
                if (!c->TypeName().empty()) text += L"  (" + Utf8ToWide(c->TypeName()) + L")";
                if (!c->IsActive()) text += L"  [inactive]";
                else if (c->IsAbstract()) text += L"  [class]";
                else if (!c->IsDefined()) text += L"  [over]";
                bool checked = true;
                auto it = model->nodeOfPrim.find(c.get());
                if (it != model->nodeOfPrim.end()) checked = !model->scene->nodes[size_t(it->second)].userHidden;
                HTREEITEM item = insert(parent, text, c.get(), depth < 2 && !c->Children().empty(), checked);
                add(c.get(), item, depth + 1);
            }
        };
        add(model->stage->PseudoRoot(), root, 0);
    }
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
    if (toSelect) TreeView_SelectItem(tree_, toSelect);
    fillingTree_ = false;
}

void App::FillClips() {
    SendMessageW(clipCombo_, CB_RESETCONTENT, 0, 0);
    const scene::Scene* sc = tab_->active ? tab_->active->scene.get() : nullptr;
    std::wstring full = L"Full timeline";
    if (sc && sc->animated) full += L"  (" + FormatTime(sc->startTime) + L"–" + FormatTime(sc->endTime) + L")";
    SendMessageW(clipCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(full.c_str()));
    if (sc)
        for (auto& c : sc->clips) {
            std::wstring s = Utf8ToWide(c.name) + L"  (" + FormatTime(c.start) + L"–" + FormatTime(c.end) + L")";
            SendMessageW(clipCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
        }
    SendMessageW(clipCombo_, CB_SETCURSEL, WPARAM(tab_->active ? tab_->active->clip + 1 : 0), 0);
    SendMessageW(clipCombo_, CB_SETDROPPEDWIDTH, S(320), 0);  // wide enough for long clip names
}

void App::SetSelection(Model* m, const usd::Prim* prim) {
    tab_->selModel = m;
    tab_->selPrim = m ? prim : nullptr;
    UpdateHighlight();
    if (m && m != tab_->active) SetActive(m);
    else ShowProperties();
    needsRedraw_ = true;
}

void App::ShowProperties() {
    SendMessageW(props_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(props_);
    int row = 0;
    auto add = [&](const std::wstring& name, const std::wstring& type, const std::wstring& value) {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(props_, &it);
        ListView_SetItemText(props_, row, 1, const_cast<wchar_t*>(type.c_str()));
        std::wstring v = value.size() > 400 ? value.substr(0, 400) + L"…" : value;
        ListView_SetItemText(props_, row, 2, const_cast<wchar_t*>(v.c_str()));
        ++row;
    };
    const Model* model = tab_->selModel ? tab_->selModel : tab_->active;
    const usd::Prim* p = tab_->selModel ? tab_->selPrim : nullptr;
    if (model && !p) {  // stage metadata
        const usd::Stage& st = *model->stage;
        auto num = [](double v) {
            wchar_t buf[32];
            std::swprintf(buf, 32, L"%g", v);
            return std::wstring(buf);
        };
        add(L"file", L"", FileName(model->path));
        add(L"layers", L"", std::to_wstring(st.Layers().size()));
        if (const usd::Value* dp = st.RootMetadata("defaultPrim")) add(L"defaultPrim", L"token", Utf8ToWide(dp->Str()));
        add(L"upAxis", L"token", Utf8ToWide(st.upAxis));
        add(L"metersPerUnit", L"double", num(st.metersPerUnit));
        add(L"timeCodesPerSecond", L"double", num(st.timeCodesPerSecond));
        add(L"framesPerSecond", L"double", num(st.framesPerSecond));
        if (st.hasTimeRange) {
            add(L"startTimeCode", L"double", num(st.startTimeCode));
            add(L"endTimeCode", L"double", num(st.endTimeCode));
        }
        if (const usd::Value* doc = st.RootMetadata("documentation")) add(L"doc", L"string", Utf8ToWide(doc->Str()));
        if (const usd::Value* cld = st.RootMetadata("customLayerData"))
            add(L"customLayerData", L"dictionary", Utf8ToWide(scene::SummarizeValue(*cld)));
    }
    if (model && p) {
        add(L"path", L"", Utf8ToWide(p->Path()));
        add(L"type", L"", Utf8ToWide(p->TypeName().empty() ? std::string("(none)") : p->TypeName()));
        if (const usd::Value* k = p->GetMetadata("kind")) add(L"kind", L"", Utf8ToWide(k->Str()));
        if (!p->APISchemas().empty()) {
            std::string s;
            for (auto& a : p->APISchemas()) s += (s.empty() ? "" : ", ") + a;
            add(L"apiSchemas", L"", Utf8ToWide(s));
        }
        if (const usd::Value* v = p->GetMetadata("variantSelection"); v && v->dict)
            for (auto& [set, sel] : *v->dict) add(L"variant " + Utf8ToWide(set), L"", Utf8ToWide(sel.Str()));
        for (const std::string& name : p->PropertyNames()) {
            bool isRel = false;
            for (auto& op : p->Opinions()) {
                if (const usd::Spec* s = op.layer->GetSpec(usd::path::AppendProperty(op.path, name))) {
                    isRel = s->type == usd::SpecType::Relationship;
                    break;
                }
            }
            if (isRel) {
                std::string targets;
                for (auto& t : p->GetTargets(name)) targets += (targets.empty() ? "<" : ", <") + t + ">";
                add(Utf8ToWide(name), L"rel", Utf8ToWide(targets));
                continue;
            }
            usd::Attribute a = p->GetAttribute(name);
            std::string value;
            if (!a.connections.empty()) {
                for (auto& c : a.connections) value += (value.empty() ? "→ <" : ", <") + c + ">";
            } else {
                usd::Value v;
                if (a.Get(model->scene->currentTime, v)) value = scene::SummarizeValue(v);
                else value = a.HasValue() ? "None" : "";
                if (a.IsAnimated()) value = "[" + std::to_string(a.samples->size()) + " samples] " + value;
            }
            add(Utf8ToWide(name), Utf8ToWide(a.typeName), Utf8ToWide(value));
        }
    }
    SendMessageW(props_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(props_, nullptr, TRUE);
}

void App::UpdateHighlight() {
    for (auto& m : tab_->models)
        for (auto& mesh : m->scene->meshes)
            mesh.highlighted = tab_->selPrim && m.get() == tab_->selModel && usd::path::HasPrefix(mesh.path, tab_->selPrim->Path());
    needsRedraw_ = true;
}

void App::ShowInfo(bool bringToFront) {
    if (!infoWnd_) {
        infoWnd_ = CreateWindowExW(0, L"USDZViewerInfo", L"Scene Info", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                   S(820), S(640), hwnd_, nullptr, inst_, nullptr);
        infoEdit_ = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                    0, 0, 10, 10, infoWnd_, reinterpret_cast<HMENU>(IDC_INFO_EDIT), inst_, nullptr);
        SendMessageW(infoEdit_, EM_SETLIMITTEXT, 64 * 1024 * 1024, 0);
        SendMessageW(infoEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont_), TRUE);
        RECT rc;
        GetClientRect(infoWnd_, &rc);
        MoveWindow(infoEdit_, 0, 0, rc.right, rc.bottom, TRUE);
    }
    std::string text;
    for (size_t i = 0; i < tab_->models.size(); ++i) {
        const Model& m = *tab_->models[i];
        if (tab_->models.size() > 1)
            text += "==== Model " + std::to_string(i + 1) + " of " + std::to_string(tab_->models.size()) + ": " +
                    WideToUtf8(FileName(m.path)) + " ====\r\n\r\n";
        text += scene::Describe(*m.stage, *m.scene);
        text += "\r\nLoaded in " + std::to_string(int(m.loadSeconds * 1000)) + " ms.\r\n\r\n";
    }
    if (tab_->models.empty()) text = "No file is open.\r\n";
    text += "Graphics: " + renderer_.AdapterName() + ", " + std::to_string(renderer_.MsaaSamples()) + "x MSAA, " +
            std::to_string(renderer_.TextureMemory() / (1024 * 1024)) + " MB of textures.\r\n";
    SetWindowTextW(infoEdit_, Utf8ToWide(text).c_str());
    std::wstring title = L"Scene Info";
    if (tab_->models.size() == 1) title += L" - " + FileName(tab_->models[0]->path);
    else if (tab_->models.size() > 1) title += L" - " + std::to_wstring(tab_->models.size()) + L" models";
    SetWindowTextW(infoWnd_, title.c_str());
    ShowWindow(infoWnd_, bringToFront ? SW_SHOW : SW_SHOWNA);
    if (bringToFront) SetForegroundWindow(infoWnd_);
}

void App::UpdateTitle() {
    std::wstring title = L"USDZ Viewer";
    if (tab_->active) {
        title = FileName(tab_->active->path);
        if (tab_->models.size() > 1) title += L" (+" + std::to_wstring(tab_->models.size() - 1) + L" more)";
        title += L" - USDZ Viewer";
    }
    SetWindowTextW(hwnd_, title.c_str());
}

void App::SetStatus(int part, const std::wstring& text) {
    SendMessageW(status_, SB_SETTEXTW, WPARAM(part), reinterpret_cast<LPARAM>(text.c_str()));
}

void App::UpdateStatus() {
    if (pendingLoads_ > 0) {
        SetStatus(0, L"Loading " + (pendingLoads_ > 1 ? std::to_wstring(pendingLoads_) + L" files" : loadingName_) + L"…");
    } else if (!tab_->active) {
        SetStatus(0, L"Open a .usdz / .usdc / .usda file (Ctrl+O) or drop one on the window");
    } else {
        wchar_t buf[96];
        std::swprintf(buf, 96, L" — loaded in %.0f ms", tab_->active->loadSeconds * 1000);
        std::wstring s = FileName(tab_->active->path) + buf;
        if (tab_->models.size() > 1) {
            size_t index = 0;
            for (size_t i = 0; i < tab_->models.size(); ++i)
                if (tab_->models[i].get() == tab_->active) index = i;
            s = L"[" + std::to_wstring(index + 1) + L"/" + std::to_wstring(tab_->models.size()) + L"] " + s;
        }
        SetStatus(0, s);
    }
    if (tab_->active) {
        const scene::Scene& sc = *tab_->active->scene;
        size_t joints = 0;
        for (auto& k : sc.skeletons) joints += k.joints.size();
        std::wstring s = WithThousands(sc.meshes.size()) + L" meshes · " + WithThousands(sc.triangleCount) + L" triangles";
        if (joints) s += L" · " + WithThousands(joints) + L" joints";
        s += L" · " + WithThousands(sc.images.size()) + L" textures";
        if (!sc.clips.empty()) s += L" · " + WithThousands(sc.clips.size()) + L" clips";
        if (!sc.warnings.empty()) s += L" · " + WithThousands(sc.warnings.size()) + L" warnings (Ctrl+I)";
        if (options_.ruler && !statusRuler_.bounds.IsEmpty()) {
            const scene::Bounds& b = statusRuler_.bounds;
            const double mpu = statusRuler_.metersPerUnit;
            s += L" · W × H × D: " + render::FormatLength((b.max.x - b.min.x) * mpu, false) + L" × " +
                 render::FormatLength((b.max.y - b.min.y) * mpu, false) + L" × " +
                 render::FormatLength((b.max.z - b.min.z) * mpu, false);
        }
        SetStatus(1, s);
    } else {
        SetStatus(1, L"");
    }
    wchar_t buf[256];
    std::swprintf(buf, 256, L"%s · %dx MSAA", Utf8ToWide(renderer_.AdapterName()).c_str(), renderer_.MsaaSamples());
    std::wstring gpu = buf;
    if (tab_->playing || SteadyActive()) {  // otherwise frames are only drawn on demand
        std::swprintf(buf, 256, L" · %.0f fps", fps_);
        gpu += buf;
    }
    SetStatus(2, gpu);
}

// ---- playback --------------------------------------------------------------------------------

// Clip list, timeline range and enabled state for the active model.
void App::RefreshTransport() {
    FillClips();
    const bool animated = tab_->active && tab_->active->scene->animated;
    const double start = tab_->active ? tab_->active->rangeStart : 0, end = tab_->active ? std::max(tab_->active->rangeStart, tab_->active->rangeEnd) : 0;
    const double len = end - start;
    tickScale_ = len >= 50 || len <= 0 ? 1.0 : std::ceil(50.0 / len);
    SendMessageW(timeline_, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(timeline_, TBM_SETRANGEMAX, TRUE, LPARAM(std::lround(len * tickScale_)));
    SendMessageW(timeline_, TBM_SETPAGESIZE, 0, LPARAM(std::max(1.0, std::round(len * tickScale_ / 10))));
    SendMessageW(timeline_, TBM_CLEARTICS, TRUE, 0);
    if (tab_->active && tab_->active->clip < 0)
        for (auto& c : tab_->active->scene->clips)
            if (c.start > start && c.start < end)
                SendMessageW(timeline_, TBM_SETTIC, 0, LPARAM(std::lround((c.start - start) * tickScale_)));
    EnableWindow(playBtn_, AnyAnimated());
    EnableWindow(timeline_, animated);
    EnableWindow(clipCombo_, animated);
    lastTimelinePos_ = -1;
    UpdateTimeline();
}

void App::SelectClip(int clip) {
    if (!tab_->active) return;
    const scene::Scene& sc = *tab_->active->scene;
    if (clip >= int(sc.clips.size())) clip = -1;
    // Fade from the outgoing animation, which keeps playing to the end of its clip meanwhile.
    double outgoingEnd = tab_->active->rangeEnd;
    if (tab_->active->clip < 0)
        for (const scene::Clip& c : sc.clips)
            if (tab_->active->time >= c.start && tab_->active->time <= c.end) outgoingEnd = c.end;
    StartTransition(*tab_->active, true, tab_->active->time, outgoingEnd);
    tab_->active->clip = clip;
    if (clip < 0) {
        tab_->active->rangeStart = sc.startTime;
        tab_->active->rangeEnd = sc.endTime;
    } else {
        tab_->active->rangeStart = sc.clips[size_t(clip)].start;
        tab_->active->rangeEnd = sc.clips[size_t(clip)].end;
    }
    tab_->active->time = tab_->active->rangeStart;
    RefreshTransport();
    needsEvaluate_ = true;
    ShowProperties();
}

void App::SetTime(double t) {
    if (!tab_->active) return;
    tab_->active->time = std::clamp(t, tab_->active->rangeStart, std::max(tab_->active->rangeStart, tab_->active->rangeEnd));
    tab_->active->blend.active = false;  // scrubbing and stepping show exact frames
    needsEvaluate_ = true;
    needsRedraw_ = true;
}

void App::SetPlaying(bool p) {
    tab_->playing = p && AnyAnimated();
    SetWindowTextW(playBtn_, tab_->playing ? L"Pause" : L"Play");
    if (!tab_->playing) ShowProperties();
    UpdateStatus();
    needsRedraw_ = true;
}

void App::StepFrame(int dir) {
    if (!tab_->active || !tab_->active->scene->animated) return;
    if (tab_->playing) SetPlaying(false);
    Model& m = *tab_->active;
    const double step = m.scene->timeCodesPerSecond / std::max(1e-6, m.scene->framesPerSecond);
    double t = std::round((m.time - m.rangeStart) / step) * step + m.rangeStart + dir * step;
    if (t > m.rangeEnd + 1e-9) t = loop_ ? m.rangeStart : m.rangeEnd;
    if (t < m.rangeStart - 1e-9) t = loop_ ? m.rangeEnd : m.rangeStart;
    SetTime(t);
    ShowProperties();
}

// Starts a crossfade on `m` from the animation at `fromTime` (which plays on up to `fromEnd` if `advance`), or
// from what is on screen when a fade is already running.
void App::StartTransition(Model& m, bool advance, double fromTime, double fromEnd) {
    Model::Transition& b = m.blend;
    const double seconds = kBlendSeconds[blendIndex_];
    if (seconds <= 0 || !m.scene->animated) {
        b.active = false;
        return;
    }
    b.snapshot = b.active;
    if (b.snapshot) b.fromPose = m.scene->CurrentPose();
    b.active = true;
    b.fromAdvance = advance && !b.snapshot;
    b.fromTime = fromTime;
    b.fromEnd = fromEnd;
    b.elapsed = 0;
    b.duration = seconds;
    b.started = false;
    needsEvaluate_ = true;
}

// Whether the animation visibly jumps from `before` to `after` (a loop restarting, the next clip starting),
// compared with how far it moves over the frame leading up to `before`. Seamless loops need no crossfade.
bool App::JumpsAt(const Model& m, double before, double after) const {
    const scene::Scene& sc = *m.scene;
    const double frame = sc.timeCodesPerSecond / std::max(1e-6, sc.framesPerSecond);
    scene::Pose prev, last, next;
    sc.SamplePose(before - frame, prev);
    sc.SamplePose(before, last);
    sc.SamplePose(after, next);
    const scene::PoseDifference step = scene::ComparePoses(prev, last), jump = scene::ComparePoses(last, next);
    constexpr double kMargin = 3;  // a jump of up to a few frames' worth of motion still reads as continuous
    return jump.angle > kMargin * step.angle + 0.02 || jump.weight > kMargin * step.weight + 0.02 ||
           jump.distance > kMargin * step.distance + 1e-3 * jump.extent;
}

bool App::AnyBlending() const {
    for (auto& m : tab_->models)
        if (m->blend.active) return true;
    return false;
}

void App::UpdateTimeline() {
    const double start = tab_->active ? tab_->active->rangeStart : 0, time = tab_->active ? tab_->active->time : 0;
    const int pos = int(std::lround((time - start) * tickScale_));
    if (pos != lastTimelinePos_ && !scrubbing_) {
        SendMessageW(timeline_, TBM_SETPOS, TRUE, pos);
        lastTimelinePos_ = pos;
    }
    std::wstring label;
    if (tab_->active && tab_->active->scene->animated) {
        wchar_t buf[96];
        std::swprintf(buf, 96, L"%s / %s   %.2f s", FormatTime(time - start).c_str(),
                      FormatTime(tab_->active->rangeEnd - start).c_str(), (time - start) / tab_->active->scene->timeCodesPerSecond);
        label = buf;
    } else if (tab_->active) {
        label = L"No animation";
    }
    wchar_t current[128] = {};
    GetWindowTextW(timeLabel_, current, 128);
    if (label != current) SetWindowTextW(timeLabel_, label.c_str());
}

// ---- camera ----------------------------------------------------------------------------------

scene::Bounds App::CurrentBounds() const {
    scene::Bounds b;
    for (auto& m : tab_->models)
        if (!m->scene->hidden) b.Add(m->scene->ComputeBounds());
    return b;
}

scene::Bounds App::FrameBoundsAll(const Tab& t) {
    scene::Bounds b;
    for (auto& m : t.models)
        if (!m->scene->hidden) b.Add(m->WorldFrameBounds());
    return b;
}

void App::FrameBounds(Tab& t, const scene::Bounds& b) {
    render::Camera& camera = t.camera;
    if (b.IsEmpty()) {
        camera.target = {0, 0, 0};
        camera.distance = 5;
    } else {
        const double r = std::max(b.Radius(), 1e-4);
        camera.target = b.Center();
        RECT rc;
        GetClientRect(viewport_, &rc);
        const double aspect = rc.bottom > 0 ? double(rc.right) / rc.bottom : 1.0;
        const double halfFov = std::min(camera.fovY / 2, std::atan(std::tan(camera.fovY / 2) * aspect));
        camera.distance = r / std::sin(halfFov) * 1.02;
    }
    needsRedraw_ = true;
}

void App::ResetView() {
    tab_->camera.yaw = 0.5;
    tab_->camera.pitch = 0.22;
    FrameBounds(*tab_, FrameBoundsAll(*tab_));
}

void App::Pick(int x, int y) {
    if (tab_->models.empty()) return;
    RECT rc;
    GetClientRect(viewport_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    const double aspect = double(rc.right) / rc.bottom;
    const double th = std::tan(tab_->camera.fovY / 2);
    const double nx = (2.0 * (x + 0.5) / rc.right - 1.0) * th * aspect;
    const double ny = (1.0 - 2.0 * (y + 0.5) / rc.bottom) * th;
    const scene::Vec3 origin = tab_->camera.Eye();
    const scene::Vec3 dir = (tab_->camera.Right() * nx + tab_->camera.Up() * ny - tab_->camera.Back()).Normalized();
    double best = 1e300;
    Model* bestModel = nullptr;
    const usd::Prim* bestPrim = nullptr;
    std::vector<float> pts;
    for (auto& model : tab_->models) {
        const scene::Scene& sc = *model->scene;
        for (const scene::Mesh& m : sc.meshes) {
            if (!m.visible || m.instancer >= 0) continue;
            sc.ComputeWorldPoints(m, pts);
            for (size_t c = 0; c + 2 < m.cornerPoint.size(); c += 3) {
                auto P = [&](uint32_t idx) { return scene::Vec3{pts[idx * 3], pts[idx * 3 + 1], pts[idx * 3 + 2]}; };
                const scene::Vec3 a = P(m.cornerPoint[c]), b = P(m.cornerPoint[c + 1]), d = P(m.cornerPoint[c + 2]);
                const scene::Vec3 e1 = b - a, e2 = d - a;
                const scene::Vec3 pv = dir.Cross(e2);
                const double det = e1.Dot(pv);
                if (std::fabs(det) < 1e-18) continue;
                const double inv = 1.0 / det;
                const scene::Vec3 tv = origin - a;
                const double u = tv.Dot(pv) * inv;
                if (u < 0 || u > 1) continue;
                const scene::Vec3 qv = tv.Cross(e1);
                const double v = dir.Dot(qv) * inv;
                if (v < 0 || u + v > 1) continue;
                const double t = e2.Dot(qv) * inv;
                if (t > 0 && t < best) {
                    best = t;
                    bestModel = model.get();
                    bestPrim = m.prim;
                }
            }
        }
    }
    if (!bestModel) {
        Command(ID_VIEW_CLEAR_SELECTION);
        return;
    }
    // Select the matching hierarchy row (which updates the selection through TVN_SELCHANGED).
    size_t entry = treeEntries_.size();
    for (size_t i = 0; i < treeEntries_.size(); ++i)
        if (treeEntries_[i].model == bestModel && treeEntries_[i].prim == bestPrim) entry = i;
    std::function<HTREEITEM(HTREEITEM)> find = [&](HTREEITEM item) -> HTREEITEM {
        for (; item; item = TreeView_GetNextSibling(tree_, item)) {
            TVITEMW tv = {};
            tv.mask = TVIF_PARAM;
            tv.hItem = item;
            TreeView_GetItem(tree_, &tv);
            if (size_t(tv.lParam) == entry) return item;
            if (HTREEITEM sub = find(TreeView_GetChild(tree_, item))) return sub;
        }
        return nullptr;
    };
    if (HTREEITEM item = entry < treeEntries_.size() ? find(TreeView_GetRoot(tree_)) : nullptr) {
        TreeView_SelectItem(tree_, item);
        TreeView_EnsureVisible(tree_, item);
    } else {
        SetSelection(bestModel, bestPrim);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::vector<std::wstring> files;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) files.push_back(argv[i]);
        LocalFree(argv);
    }
    App app;
    if (!app.Create(inst, show, files)) return 1;
    int code = app.Run();
    CoUninitialize();
    return code;
}
