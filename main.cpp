#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <algorithm>
#include <atomic>
#include <d3d11.h>
#include <iostream>
#include <string>
#include <tchar.h>
#include <thread>
#include <vector>

#include "ConfigManager.h"
#include "Connection.h"
#include "OutputRouter.h"
// Must come after windows.h (pulled in via ConfigManager.h)
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include "resource.h"

namespace {
constexpr WORD MOUSE_WHEEL_UP = 0x1000;
constexpr WORD MOUSE_WHEEL_DOWN = 0x1001;
constexpr float kGuiScaleX1 = 1.0f;
constexpr float kGuiScaleX2 = 2.0f;
constexpr float kGuiScaleX3 = 3.0f;
}

static float g_guiScale = kGuiScaleX1;

static void ApplyGuiScale(float scale) {
  ImGuiIO &io = ImGui::GetIO();
  io.FontGlobalScale = scale;
  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(scale);
}

// Data
static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Tray Icon Constants
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_APP_ICON 1001
#define ID_TRAY_EXIT 5001
#define ID_TRAY_OPEN 5002

NOTIFYICONDATAW g_nid = {sizeof(g_nid)};
bool g_WindowVisible = true;
static HWND g_hwnd = nullptr;

void AddTrayIcon(HWND hWnd) {
  g_nid.cbSize = sizeof(NOTIFYICONDATAW);

  g_nid.hWnd = hWnd;
  g_nid.uID = ID_TRAY_APP_ICON;
  g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_nid.uCallbackMessage = WM_TRAYICON;
  g_nid.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDI_ICON1));
  lstrcpyW(g_nid.szTip, L"WiimoteKeyMapApp");
  Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

// How long (ms) each balloon notification stays visible before being cleared.
static constexpr int kNotificationDisplayMs = 3000;

// Generation counter — incremented on each new notification so that the
// dismissal timer for an older notification does not clear a newer one.
static std::atomic<int> g_notifGeneration{0};

// Show a balloon notification via the system tray icon.
// Dismisses any currently visible balloon before showing the new one.
// Automatically dismissed after kNotificationDisplayMs.
// Safe to call from any thread.
void ShowNotification(const wchar_t *title, const wchar_t *message) {
  // Dismiss any existing balloon first so the new one appears immediately.
  NOTIFYICONDATAW clearNid = g_nid;
  clearNid.uFlags          = NIF_INFO;
  clearNid.dwInfoFlags     = NIIF_INFO;
  clearNid.szInfoTitle[0]  = L'\0';
  clearNid.szInfo[0]       = L'\0';
  Shell_NotifyIconW(NIM_MODIFY, &clearNid);

  NOTIFYICONDATAW nid  = g_nid;
  nid.uFlags           = NIF_INFO;
  nid.dwInfoFlags      = NIIF_INFO;
  lstrcpyW(nid.szInfoTitle, title);
  lstrcpyW(nid.szInfo, message);
  Shell_NotifyIconW(NIM_MODIFY, &nid);

  // Schedule balloon dismissal.  The generation check ensures that if a
  // second notification fires before the timer expires, the first timer
  // does not wipe out the newer balloon.
  int gen = ++g_notifGeneration;
  std::thread([gen]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(kNotificationDisplayMs));
    if (g_notifGeneration.load() == gen) {
      NOTIFYICONDATAW clearNid = g_nid;
      clearNid.uFlags          = NIF_INFO;
      clearNid.dwInfoFlags     = NIIF_INFO;
      clearNid.szInfoTitle[0]  = L'\0';
      clearNid.szInfo[0]       = L'\0';
      Shell_NotifyIconW(NIM_MODIFY, &clearNid);
    }
  }).detach();
}

void ShowTrayContextMenu(HWND hWnd) {
  POINT pt;
  GetCursorPos(&pt);
  HMENU hMenu = CreatePopupMenu();
  InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_OPEN,
              L"Open WiimoteKeyMapApp");
  InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
  InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit");

  SetForegroundWindow(hWnd);
  TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
  DestroyMenu(hMenu);
}

// Returns the selected file path, or empty string if the user cancelled.
static std::string ShowOpenFileDialog() {
  char buf[MAX_PATH] = {};
  OPENFILENAMEA ofn   = {};
  ofn.lStructSize     = sizeof(ofn);
  ofn.hwndOwner       = g_hwnd;
  ofn.lpstrFilter     = "JSON Profile\0*.json\0All Files\0*.*\0";
  ofn.lpstrFile       = buf;
  ofn.nMaxFile        = MAX_PATH;
  ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  ofn.lpstrDefExt     = "json";
  return GetOpenFileNameA(&ofn) ? buf : "";
}

static std::string ShowSaveFileDialog(const std::string &defaultName) {
  char buf[MAX_PATH] = {};
  strncpy_s(buf, defaultName.c_str(), MAX_PATH - 1);
  OPENFILENAMEA ofn   = {};
  ofn.lStructSize     = sizeof(ofn);
  ofn.hwndOwner       = g_hwnd;
  ofn.lpstrFilter     = "JSON Profile\0*.json\0All Files\0*.*\0";
  ofn.lpstrFile       = buf;
  ofn.nMaxFile        = MAX_PATH;
  ofn.Flags           = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
  ofn.lpstrDefExt     = "json";
  return GetSaveFileNameA(&ofn) ? buf : "";
}

// App State
std::atomic<bool> g_appRunning{true};
std::atomic<bool> g_isSearching{false};
Connection g_wiimote;
OutputRouter g_router;

void BackgroundTask() {
  // Responsible only for input routing and passive reconnection.
  // BT discovery is handled by ScanTask.
  while (g_appRunning) {
    if (g_wiimote.IsConnected()) {
      const WiiRemoteState &state = g_wiimote.GetState();
      g_router.Route(state);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } else {
      g_router.Reset();
      // Poll the HID device list every 500 ms.  ScanTask enables HID service
      // when a remote is found; PassiveConnect picks it up here.
      g_wiimote.PassiveConnect();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
}

void ScanTask() {
  // Continuously scans for Wii Remotes via BT inquiry while not connected.
  // Each ScanOnce() call issues one ~2.56 s inquiry and returns immediately
  // after — giving truly gapless coverage without a fixed polling interval.
  bool stalePairingsRemoved = false;

  while (g_appRunning) {
    if (!g_wiimote.IsConnected()) {
      // On the first cycle after a disconnect, remove stale BT pairings so
      // a fresh SYNC press is visible to the inquiry as an unknown device.
      if (!stalePairingsRemoved) {
        g_wiimote.RemoveStalePairings();
        stalePairingsRemoved = true;
      }
      g_isSearching.store(true);
      g_wiimote.ScanOnce(); // ~2.56 s, then restart immediately
    } else {
      stalePairingsRemoved = false; // reset for the next disconnect
      g_isSearching.store(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  g_isSearching.store(false);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow) {

  int argc;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  bool startMinimized = false;
  if (argv != NULL) {
    for (int i = 1; i < argc; i++) {
      if (wcscmp(argv[i], L"--background") == 0 || wcscmp(argv[i], L"-b") == 0) {
        startMinimized = true;
      }
    }
    LocalFree(argv);
  }

  // Enable Windows DPI awareness for HiDPI compatibility.
  SetProcessDPIAware();

  // ConfigManager Load (Loads the last active session)
  ConfigManager::GetInstance().LoadActiveConfig();

  // Create application window
  HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON1));
  WNDCLASSEXW wc = {sizeof(wc),          CS_CLASSDC, WndProc,    0L,         0L,
                    hInstance,           hAppIcon,   nullptr,    nullptr,    nullptr,
                    L"WiimoteKeyMap UI", hAppIcon};
  ::RegisterClassExW(&wc);
  HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"WiimoteKeyMapApp",
                              WS_OVERLAPPEDWINDOW, 100, 100, 620, 770, nullptr,
                              nullptr, wc.hInstance, nullptr);
  g_hwnd = hwnd;

  if (!CreateDeviceD3D(hwnd)) {
    CleanupDeviceD3D();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  if (startMinimized) {
    g_WindowVisible = false;
    ::ShowWindow(hwnd, SW_HIDE);
  } else {
    g_WindowVisible = true;
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  }

  ::UpdateWindow(hwnd);

  // Initialize System Tray
  AddTrayIcon(hwnd);

  // Register connect/disconnect notifications.
  // Callbacks are set after AddTrayIcon so g_nid is ready before they fire.
  g_wiimote.SetOnConnected([]() {
    ShowNotification(L"Wii Remote Connected", L"Wii Remote Connected.");
  });
  g_wiimote.SetOnDisconnected([]() {
    ShowNotification(L"Wii Remote Disconnected", L"Wii Remote Disconnected.");
  });

  // bgThread: input routing + passive HID reconnection
  // scanThread: continuous BT inquiry (ScanOnce loop)
  std::thread bgThread(BackgroundTask);
  std::thread scanThread(ScanTask);

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // Setup Dear ImGui style (Premium Dark Mode)
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 8.0f;
  style.FrameRounding = 5.0f;
  style.GrabRounding = 5.0f;
  style.PopupRounding = 5.0f;
  style.WindowBorderSize = 0.0f;
  style.ItemSpacing = ImVec2(10, 8);
  style.FramePadding = ImVec2(10, 6);

  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.35f, 1.0f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.35f, 0.50f, 1.0f);
  style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.25f, 0.45f, 1.0f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.35f, 0.65f, 1.0f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.20f, 0.40f, 1.0f);
  style.Colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);

  ApplyGuiScale(g_guiScale);

  // Setup Platform/Renderer backends
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  // Main loop
  bool done = false;
  while (!done) {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        done = true;
    }
    if (done)
      break;

    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
      CleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                  DXGI_FORMAT_UNKNOWN, 0);
      g_ResizeWidth = g_ResizeHeight = 0;
      CreateRenderTarget();
    }

    // Start the Dear ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 0. Top Menu Bar
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("Settings")) {
        if (ImGui::BeginMenu("GUI Scale")) {
          if (ImGui::MenuItem("x1", NULL, g_guiScale == kGuiScaleX1)) {
            g_guiScale = kGuiScaleX1;
            ApplyGuiScale(g_guiScale);
          }
          if (ImGui::MenuItem("x2", NULL, g_guiScale == kGuiScaleX2)) {
            g_guiScale = kGuiScaleX2;
            ApplyGuiScale(g_guiScale);
          }
          if (ImGui::MenuItem("x3", NULL, g_guiScale == kGuiScaleX3)) {
            g_guiScale = kGuiScaleX3;
            ApplyGuiScale(g_guiScale);
          }
          ImGui::EndMenu();
        }

        bool tray = ConfigManager::GetInstance().GetMinimizeToTray();
        if (ImGui::MenuItem("Minimize to System Tray", NULL, &tray))
          ConfigManager::GetInstance().SetMinimizeToTray(tray);
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    // 1. Connection Panel
    ImGui::SetNextWindowPos(ImVec2(10, 30));
    ImGui::SetNextWindowSize(ImVec2(580, 140));
    ImGui::Begin("Device Connection", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);

    ImGui::TextDisabled("Version: %s", APP_VERSION);
    ImGui::Separator();

    bool isConnected = g_wiimote.IsConnected();
    ImVec4 statusColor = isConnected ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                     : ImVec4(1.0f, 0.8f, 0.0f, 1.0f);

    // Status LED
    ImGui::TextUnformatted("Status:");
    ImGui::SameLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + 8, p.y + 8), 6.0f,
        ImGui::ColorConvertFloat4ToU32(statusColor));
    ImGui::Dummy(ImVec2(20, 16));
    ImGui::SameLine();

    if (isConnected) {
      ImGui::TextColored(statusColor, "CONNECTED");
    } else {
      ImGui::TextColored(statusColor, "SCANNING... (Hold SYNC on Wiimote)");
    }

    ImGui::Separator();

    if (ImGui::Button("Search & Connect", ImVec2(-1, 30))) {
      if (!isConnected && !g_isSearching) {
        // Detached thread for non-blocking UI
        std::thread([]() {
          g_isSearching.store(true);
          g_wiimote.ActiveSearchConnect();
          g_isSearching.store(false);
        }).detach();
      }
    }
    ImGui::End();

    // 2. Mapping Panel
    ImGui::SetNextWindowPos(ImVec2(10, 180));
    ImGui::SetNextWindowSize(ImVec2(580, 570));
    ImGui::Begin("Keyboard Mappings", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);

    ImGui::SeparatorText("Profile Management");

    static char newProfileName[64] = "";
    auto profiles = ConfigManager::GetInstance().ListProfiles();
    std::string currentProfile =
        ConfigManager::GetInstance().GetCurrentProfileName();

    int currentIdx = 0;
    for (int i = 0; i < (int)profiles.size(); ++i) {
      if (profiles[i] == currentProfile) { currentIdx = i; break; }
    }

    // Row 1: dropdown  |  Quick Save  |  Delete
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Profile:");
    ImGui::SameLine();
    ImGui::PushItemWidth(190);
    if (ImGui::BeginCombo("##profiles", profiles[currentIdx].c_str())) {
      for (int n = 0; n < (int)profiles.size(); n++) {
        const bool is_selected = (currentIdx == n);
        if (ImGui::Selectable(profiles[n].c_str(), is_selected))
          ConfigManager::GetInstance().LoadProfile(profiles[n]);
        if (is_selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Quick Save")) {
      ConfigManager::GetInstance().SaveCurrentProfile();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Overwrite \"%s\" with current mappings", currentProfile.c_str());

    ImGui::SameLine();
    ImGui::BeginDisabled(currentProfile == "default");
    if (ImGui::Button("Delete")) {
      ConfigManager::GetInstance().DeleteProfile(currentProfile);
      ConfigManager::GetInstance().LoadProfile("default");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(currentProfile == "default"
                            ? "Cannot delete the default profile"
                            : "Delete \"%s\"", currentProfile.c_str());
    ImGui::EndDisabled();

    // Row 2: Import from file  |  Export to file
    if (ImGui::Button("Import from File...")) {
      std::string path = ShowOpenFileDialog();
      if (!path.empty())
        ConfigManager::GetInstance().LoadProfileFromPath(path);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Load a profile JSON from anywhere on disk.\n"
                        "It is copied into the profiles folder and becomes active.");

    ImGui::SameLine();
    if (ImGui::Button("Export to File...")) {
      std::string def = currentProfile + ".json";
      std::string path = ShowSaveFileDialog(def);
      if (!path.empty())
        ConfigManager::GetInstance().SaveProfileToPath(path);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Save current mappings to a JSON file of your choice.");

    // Row 3: Save as new profile inside profiles/ dir
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Save as New:");
    ImGui::SameLine();
    ImGui::PushItemWidth(175);
    ImGui::InputText("##newname", newProfileName, IM_ARRAYSIZE(newProfileName));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
      if (strlen(newProfileName) > 0) {
        ConfigManager::GetInstance().SaveProfile(newProfileName);
        newProfileName[0] = '\0';
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Save current mappings as a new profile in the profiles folder.");

    ImGui::SeparatorText("Button Mappings");

    static std::string recordingButton = "";
    static std::vector<WORD> recordedKeys;
    static std::chrono::steady_clock::time_point recordingStartTime;
    static bool wasAnyKeyDown = false;
    static bool isReleaseBufferActive = false;
    static std::vector<WORD> bufferedKeys;
    static std::chrono::steady_clock::time_point releaseStartTime;
    static std::vector<WORD> initialHeldKeys;

    if (ImGui::BeginTable("MappingTable", 4,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_WidthFixed, 30.0f);
      ImGui::TableSetupColumn("Wii Button", ImGuiTableColumnFlags_WidthFixed,
                              100.0f);
      ImGui::TableSetupColumn("Assignment (Keyboard Combinations)",
                              ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Action Mode", ImGuiTableColumnFlags_WidthFixed,
                              100.0f);
      ImGui::TableHeadersRow();

      auto &mappings = ConfigManager::GetInstance().GetMappings();
      static std::vector<std::string> buttonOrder = {
          "UP",   "DOWN",  "LEFT", "RIGHT", "A",  "B",
          "PLUS", "MINUS", "HOME", "ONE",   "TWO"};

      for (const std::string &button : buttonOrder) {
        auto it = mappings.find(button);
        if (it == mappings.end())
          continue;
        auto &vtKeys = it->second;

        ImGui::TableNextRow(ImGuiTableRowFlags_None, 28.0f);
        ImGui::PushID(button.c_str());

        // Column 0: Clear
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button("X", ImVec2(24, 22))) {
          ConfigManager::GetInstance().SetMappedKeys(button, {});
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Clear mapping");

        // Column 1: Label
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(button.c_str());

        // Column 2: Mapping
        ImGui::TableSetColumnIndex(2);
        std::string mappingText = "";
        bool isThisRecording = (recordingButton == button);
        if (isThisRecording) {
          if (recordedKeys.empty()) {
            mappingText = "[ Recording... ]";
          } else {
            mappingText = "";
            for (size_t i = 0; i < recordedKeys.size(); ++i) {
              if (i > 0)
                mappingText += "+";
              mappingText +=
                  ConfigManager::GetInstance().VKToString(recordedKeys[i]);
            }
          }
          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        } else {
          for (size_t i = 0; i < vtKeys.size(); ++i) {
            if (i > 0)
              mappingText += " + ";
            mappingText += ConfigManager::GetInstance().VKToString(vtKeys[i]);
          }
          if (mappingText.empty())
            mappingText = "(None)";
        }

        if (ImGui::Button(mappingText.c_str(), ImVec2(-1, 0))) {
          if (isThisRecording) {
            recordingButton = "";
          } else {
            recordingButton = button;
            recordedKeys.clear();
            wasAnyKeyDown = false;
            isReleaseBufferActive = false;
            recordingStartTime = std::chrono::steady_clock::now();
            initialHeldKeys.clear();
            for (int i = 1; i < 256; ++i) {
              if (GetAsyncKeyState(i) & 0x8000)
                initialHeldKeys.push_back((WORD)i);
            }
          }
        }
        if (isThisRecording)
          ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(
              "Click to start/stop recording. Mouse click cancels.");

        // Column 3: Mode
        ImGui::TableSetColumnIndex(3);
        const char *mouseActions[] = {"Kbd", "M-Left", "M-Right", "M-Mid",
                                      "M-Wheel Up", "M-Wheel Down"};
        int cmdIdx = 0;
        if (!vtKeys.empty()) {
          if (vtKeys[0] == VK_LBUTTON)
            cmdIdx = 1;
          else if (vtKeys[0] == VK_RBUTTON)
            cmdIdx = 2;
          else if (vtKeys[0] == VK_MBUTTON)
            cmdIdx = 3;
          else if (vtKeys[0] == MOUSE_WHEEL_UP)
            cmdIdx = 4;
          else if (vtKeys[0] == MOUSE_WHEEL_DOWN)
            cmdIdx = 5;
        }

        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##Mouse", &cmdIdx, mouseActions,
                         IM_ARRAYSIZE(mouseActions))) {
          if (cmdIdx == 0)
            ConfigManager::GetInstance().SetMappedKeys(button, {});
          else {
            WORD mouseVK = (cmdIdx == 1)
                               ? VK_LBUTTON
                               : (cmdIdx == 2 ? VK_RBUTTON
                                              : (cmdIdx == 3 ? VK_MBUTTON
                                                             : (cmdIdx == 4 ? MOUSE_WHEEL_UP
                                                                            : MOUSE_WHEEL_DOWN)));
            ConfigManager::GetInstance().SetMappedKeys(button, {mouseVK});
          }
        }

        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    // Global key recording logic (Native Win32 Listener)
    if (!recordingButton.empty()) {
      std::vector<WORD> currentlyHeld;
      for (int i = 1; i < 256; ++i) {
        // Exclude mouse buttons
        if (i == VK_LBUTTON || i == VK_RBUTTON || i == VK_MBUTTON ||
            i == VK_XBUTTON1 || i == VK_XBUTTON2)
          continue;

        if (GetAsyncKeyState(i) & 0x8000) {
          // Ignore keys that were already down when recording started until
          // they are released
          auto it = std::find(initialHeldKeys.begin(), initialHeldKeys.end(),
                              (WORD)i);
          if (it != initialHeldKeys.end())
            continue;

          // Skip generic modifiers to avoid duplicates and 'phantom' stuck
          // states. We exclusively use L/R versions (LSHIFT, RSHIFT, etc.)
          if (i == VK_SHIFT || i == VK_CONTROL || i == VK_MENU)
            continue;

          currentlyHeld.push_back((WORD)i);
        } else {
          // Once released, remove from initial skip list
          auto it = std::find(initialHeldKeys.begin(), initialHeldKeys.end(),
                              (WORD)i);
          if (it != initialHeldKeys.end())
            initialHeldKeys.erase(it);
        }
      }

      auto now = std::chrono::steady_clock::now();
      bool isAnyKeyDownNow = !currentlyHeld.empty();

      // Cancel on Mouse Click during Keyboard Recording
      bool cancelledByMouse = false;
      // Only cancel if clicked OUTSIDE any ImGui window or specific mapping
      // buttons Here, we'll simplify: if clicking, check if we were clicking
      // the button we just started/stopped
      if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) {
        // Ensure we don't cancel if we just clicked the recording button to
        // start/stop
        if (!wasAnyKeyDown &&
            (std::chrono::duration_cast<std::chrono::milliseconds>(
                 now - recordingStartTime)
                 .count() > 300)) {
          recordingButton = "";
          recordedKeys.clear();
          wasAnyKeyDown = false;
          isReleaseBufferActive = false;
          cancelledByMouse = true;
        }
      }

      if (isAnyKeyDownNow) {
        wasAnyKeyDown = true;
        if (currentlyHeld.size() < recordedKeys.size()) {
          // A key was released. Start buffer to see if it's a staggered combo
          // release.
          if (!isReleaseBufferActive) {
            isReleaseBufferActive = true;
            releaseStartTime = now;
            bufferedKeys = recordedKeys;
          } else if (std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - releaseStartTime)
                         .count() > 150) {
            // Buffer expired, this is an intentional release to a smaller set
            recordedKeys = currentlyHeld;
            isReleaseBufferActive = false;
          }
        } else {
          // Keys were added or remained the same size - reset buffer
          recordedKeys = currentlyHeld;
          isReleaseBufferActive = false;
        }
      } else if (wasAnyKeyDown && !cancelledByMouse) {
        // Transition to ALL RELEASED - Save the last state seen
        if (!recordedKeys.empty()) {
          ConfigManager::GetInstance().SetMappedKeys(recordingButton,
                                                     recordedKeys);
          // SaveActiveConfig is called inside SetMappedKeys
        }
        recordingButton = "";
        recordedKeys.clear();
        bufferedKeys.clear();
        wasAnyKeyDown = false;
        isReleaseBufferActive = false;
      }
    }

    ImGui::End();

    // Rendering
    ImGui::Render();
    const float clear_color_with_alpha[4] = {0.05f, 0.05f, 0.05f, 1.00f};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                            nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                               clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0); // Present with vsync
  }

  // Cleanup
  g_appRunning = false;
  bgThread.join();
  scanThread.join();

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  RemoveTrayIcon();

  CleanupDeviceD3D();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

  return 0;
}

// DX11 Boilerplate
bool CreateDeviceD3D(HWND hWnd) {
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };
  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED)
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res != S_OK)
    return false;

  CreateRenderTarget();
  return true;
}

void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

void CreateRenderTarget() {
  ID3D11Texture2D *pBackBuffer;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
  g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                       &g_mainRenderTargetView);
  pBackBuffer->Release();
}

void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {
  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED) {
      g_ResizeWidth = (UINT)LOWORD(lParam);
      g_ResizeHeight = (UINT)HIWORD(lParam);
    }
    return 0;
  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU)
      return 0; // Disable ALT app menu
    if ((wParam & 0xfff0) == SC_MINIMIZE &&
        ConfigManager::GetInstance().GetMinimizeToTray()) {
      ::ShowWindow(hWnd, SW_HIDE);
      g_WindowVisible = false;
      ShowNotification(L"WiimoteKeyMapApp",
                       L"Running in background. "
                       L"Double-click the tray icon to restore.");
      return 0;
    }
    break;
  case WM_TRAYICON:
    if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
      ::ShowWindow(hWnd, SW_SHOW);
      ::ShowWindow(hWnd, SW_RESTORE);
      ::SetForegroundWindow(hWnd);
      g_WindowVisible = true;
    } else if (LOWORD(lParam) == WM_RBUTTONUP) {
      ShowTrayContextMenu(hWnd);
    }
    break;
  case WM_COMMAND:
    if (LOWORD(wParam) == ID_TRAY_OPEN) {
      ::ShowWindow(hWnd, SW_SHOW);
      ::ShowWindow(hWnd, SW_RESTORE);
      ::SetForegroundWindow(hWnd);
      g_WindowVisible = true;
    } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
      ::PostQuitMessage(0);
    }
    break;
  case WM_CLOSE:
    if (ConfigManager::GetInstance().GetMinimizeToTray()) {
      ::ShowWindow(hWnd, SW_HIDE);
      g_WindowVisible = false;
      ShowNotification(L"WiimoteKeyMapApp",
                       L"Running in the background. "
                       L"Double-click the tray icon to restore.");
    } else {
      ::PostQuitMessage(0);
    }
    return 0;
  case WM_DESTROY:
    ::PostQuitMessage(0);
    return 0;
  }
  return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
