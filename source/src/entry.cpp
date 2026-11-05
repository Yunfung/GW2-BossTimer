///----------------------------------------------------------------------------------------------------
/// GW2 BOSS 计时器 — Nexus 插件
/// 抓取星岬岛 BOSS 时刻表，游戏内生成按钮，按倒计时排序，点击把一行 BOSS 信息发到聊天频道。
///----------------------------------------------------------------------------------------------------

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nexus/Nexus.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "core.h"
#include "defaultdata.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")

/* proto */
void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void RenderMain();
void RenderOptions();
void OnInputBind(const char* aIdentifier, bool aIsRelease);

/* globals */
static AddonDefinition_t AddonDef = {};
static AddonAPI_t* APIDefs = nullptr;
static NexusLinkData_t* NexusLink = nullptr;
static HMODULE hSelf = nullptr;

static const char* kToggleId = "BOSS计时器-显示面板";
static const char* kLogTag = "BossTimer";

///----------------------------------------------------------------------------------------------------
/// 配置
///----------------------------------------------------------------------------------------------------
struct Config
{
    bool showPanel = true;
    bool autoHide = true;
    bool lockPos = false;
    bool hideChrome = false;
    int alphaPct = 92;
    int btnAlphaPct = 100;
    bool btnColorSet = false;
    float btnColor[3] = { 0.24f, 0.32f, 0.42f };
    bool textColorSet = false;
    float textColor[3] = { 1.0f, 1.0f, 1.0f };
    float btnW = 0.0f;
    float btnH = 0.0f;
    int graceMin = 15;
    std::string prefix = ""; // 空 = 发送到当前频道
    int refreshMin = 30;
    std::string dataUrl = "https://gw2.wishingstarmoye.com/la-assets/gw2timer/data/timerbox_bossdata.js";
    int openDelay = 180;
    int charGap = 2;
    int sendMode = 0;
    int sendKeyVk = 0x0D;
};

static Config gCfg;
static std::mutex gCfgMx;
static std::string gCfgPath;
static std::string gCachePath;
static std::string gAddonDir;

struct ChannelPreset
{
    const char* code;
    const char* name;
};

static const ChannelPreset kPresets[] =
{
    { "",   "当前频道" },
    { "m",  "地图" },
    { "d",  "团队" },
    { "p",  "队伍" },
    { "t",  "战队" },
    { "s",  "说" },
    { "g",  "当前公会" },
    { "g1", "公会1" },
    { "g2", "公会2" },
    { "g3", "公会3" },
    { "g4", "公会4" },
    { "g5", "公会5" },
    { "g6", "公会6" }
};
static const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

///----------------------------------------------------------------------------------------------------
/// 通用工具
///----------------------------------------------------------------------------------------------------
static void LogInfo(const std::string& msg)
{
    if (APIDefs && APIDefs->Log) APIDefs->Log(LOGL_INFO, kLogTag, msg.c_str());
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, char(0));
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, wchar_t(0));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void MakeDirUtf8(const std::string& p)
{
    if (p.empty()) return;
    CreateDirectoryW(Utf8ToWide(p).c_str(), nullptr);
}

static bool ReadFileUtf8(const std::string& path, std::string& out)
{
    std::ifstream in(Utf8ToWide(path).c_str(), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

static bool WriteFileUtf8(const std::string& path, const std::string& data)
{
    std::ofstream out(Utf8ToWide(path).c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return out.good();
}

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

///----------------------------------------------------------------------------------------------------
/// 配置读写
///----------------------------------------------------------------------------------------------------
static void InitPaths()
{
    const char* dir = nullptr;
    if (APIDefs && APIDefs->Paths_GetAddonDirectory)
        dir = APIDefs->Paths_GetAddonDirectory("BossTimer");

    if (dir && dir[0])
    {
        gAddonDir = dir;
    }
    else
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(hSelf, buf, MAX_PATH);
        std::string p(buf);
        size_t pos = p.find_last_of("\\/");
        gAddonDir = (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
    }

    MakeDirUtf8(gAddonDir);
    gCfgPath = gAddonDir + "\\BossTimer.ini";
    gCachePath = gAddonDir + "\\bossdata_cache.js";
}

static void SaveCfg()
{
    if (gCfgPath.empty()) return;

    char buf[1024];
    std::string s;
    s += "showPanel=" + std::to_string(gCfg.showPanel ? 1 : 0) + "\n";
    s += "autoHide=" + std::to_string(gCfg.autoHide ? 1 : 0) + "\n";
    s += "lockPos=" + std::to_string(gCfg.lockPos ? 1 : 0) + "\n";
    s += "hideChrome=" + std::to_string(gCfg.hideChrome ? 1 : 0) + "\n";
    s += "alphaPct=" + std::to_string(gCfg.alphaPct) + "\n";
    s += "btnAlphaPct=" + std::to_string(gCfg.btnAlphaPct) + "\n";
    s += "btnColorSet=" + std::to_string(gCfg.btnColorSet ? 1 : 0) + "\n";
    snprintf(buf, sizeof(buf), "btnColor=%.4f,%.4f,%.4f\n", gCfg.btnColor[0], gCfg.btnColor[1], gCfg.btnColor[2]); s += buf;
    s += "textColorSet=" + std::to_string(gCfg.textColorSet ? 1 : 0) + "\n";
    snprintf(buf, sizeof(buf), "textColor=%.4f,%.4f,%.4f\n", gCfg.textColor[0], gCfg.textColor[1], gCfg.textColor[2]); s += buf;
    snprintf(buf, sizeof(buf), "btnW=%.1f\nbtnH=%.1f\n", gCfg.btnW, gCfg.btnH); s += buf;
    s += "graceMin=" + std::to_string(gCfg.graceMin) + "\n";
    s += "prefix=" + gCfg.prefix + "\n";
    s += "refreshMin=" + std::to_string(gCfg.refreshMin) + "\n";
    s += "dataUrl=" + gCfg.dataUrl + "\n";
    s += "openDelay=" + std::to_string(gCfg.openDelay) + "\n";
    s += "charGap=" + std::to_string(gCfg.charGap) + "\n";
    s += "sendMode=" + std::to_string(gCfg.sendMode) + "\n";
    s += "sendKeyVk=" + std::to_string(gCfg.sendKeyVk) + "\n";

    WriteFileUtf8(gCfgPath, s);
}

static void ApplySetting(const std::string& key, const std::string& val)
{
    if (key == "showPanel") gCfg.showPanel = atoi(val.c_str()) != 0;
    else if (key == "autoHide") gCfg.autoHide = atoi(val.c_str()) != 0;
    else if (key == "lockPos") gCfg.lockPos = atoi(val.c_str()) != 0;
    else if (key == "hideChrome") gCfg.hideChrome = atoi(val.c_str()) != 0;
    else if (key == "alphaPct") gCfg.alphaPct = atoi(val.c_str());
    else if (key == "btnAlphaPct") gCfg.btnAlphaPct = atoi(val.c_str());
    else if (key == "btnColorSet") gCfg.btnColorSet = atoi(val.c_str()) != 0;
    else if (key == "textColorSet") gCfg.textColorSet = atoi(val.c_str()) != 0;
    else if (key == "btnW") gCfg.btnW = (float)atof(val.c_str());
    else if (key == "btnH") gCfg.btnH = (float)atof(val.c_str());
    else if (key == "graceMin") gCfg.graceMin = atoi(val.c_str());
    else if (key == "prefix")
    {
        gCfg.prefix = val;
        if (!gCfg.prefix.empty() && gCfg.prefix[0] == '/') gCfg.prefix = gCfg.prefix.substr(1);
    }
    else if (key == "refreshMin") gCfg.refreshMin = atoi(val.c_str());
    else if (key == "dataUrl" && !val.empty()) gCfg.dataUrl = val;
    else if (key == "openDelay") gCfg.openDelay = atoi(val.c_str());
    else if (key == "charGap") gCfg.charGap = atoi(val.c_str());
    else if (key == "sendMode") gCfg.sendMode = atoi(val.c_str());
    else if (key == "sendKeyVk") gCfg.sendKeyVk = atoi(val.c_str());
    else if (key == "btnColor" || key == "textColor")
    {
        float* dst = (key == "btnColor") ? gCfg.btnColor : gCfg.textColor;
        sscanf(val.c_str(), "%f,%f,%f", &dst[0], &dst[1], &dst[2]);
    }
}

static void LoadCfg()
{
    std::string text;
    if (!ReadFileUtf8(gCfgPath, text)) return;

    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = Trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        ApplySetting(Trim(line.substr(0, eq)), line.substr(eq + 1));
    }

    // 旧版本默认写的是 /map（国服无效），没有手动改过的配置回到“当前频道”
    if (gCfg.prefix == "map") gCfg.prefix.clear();
    if (gCfg.graceMin == 20) gCfg.graceMin = 15; // 旧默认值，改成和页面一致的 15 分钟
    if (gCfg.alphaPct < 0) gCfg.alphaPct = 0;
    if (gCfg.alphaPct > 100) gCfg.alphaPct = 100;
    if (gCfg.btnAlphaPct < 0) gCfg.btnAlphaPct = 0;
    if (gCfg.btnAlphaPct > 100) gCfg.btnAlphaPct = 100;
    if (gCfg.refreshMin < 5) gCfg.refreshMin = 5;
    if (gCfg.graceMin < 0) gCfg.graceMin = 0;
    if (gCfg.graceMin > 120) gCfg.graceMin = 120;
}

///----------------------------------------------------------------------------------------------------
/// HTTP 抓取
///----------------------------------------------------------------------------------------------------
static bool HttpGetText(const std::wstring& wurl, std::string& out, std::string& err)
{
    out.clear();

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
    {
        err = "网址解析失败";
        return false;
    }

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) GW2-BossTimer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        err = "WinHTTP 初始化失败";
        return false;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 8000, 10000);

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (connect)
    {
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (request)
        {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr))
            {
                DWORD status = 0;
                DWORD len = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

                if (status == 200)
                {
                    for (;;)
                    {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(request, &avail)) break;
                        if (avail == 0) { ok = true; break; }
                        if (avail > 64 * 1024) avail = 64 * 1024;
                        size_t old = out.size();
                        out.resize(old + avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, &out[old], avail, &read)) { ok = false; break; }
                        out.resize(old + read);
                        if (read == 0) { ok = true; break; }
                    }
                }
                else
                {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "服务器返回 HTTP %lu", (unsigned long)status);
                    err = buf;
                }
            }
            else
            {
                err = "连接超时或被拒绝";
            }
            WinHttpCloseHandle(request);
        }
        else
        {
            err = "请求创建失败";
        }
        WinHttpCloseHandle(connect);
    }
    else
    {
        err = "域名连接失败";
    }

    WinHttpCloseHandle(session);

    if (ok && out.empty())
    {
        ok = false;
        err = "返回内容为空";
    }
    return ok;
}

///----------------------------------------------------------------------------------------------------
/// 数据状态与后台抓取
///----------------------------------------------------------------------------------------------------
#include <chrono>

static std::vector<bosstimer::BossData> gBosses;
static std::mutex gBossMx;
static std::string gSource = "内置";
static std::string gUpdatedText = "未更新";
static std::string gLastError;
static std::atomic<bool> gFetching(false);
static std::atomic<bool> gStop(false);

static std::mutex gWakeMx;
static std::condition_variable gWakeCv;
static bool gWakeFlag = false;
static std::thread gFetchThread;
static std::thread gSendThread;

static void RequestRefresh()
{
    {
        std::lock_guard<std::mutex> lock(gWakeMx);
        gWakeFlag = true;
    }
    gWakeCv.notify_all();
}

static void MarkUpdated(const char* source)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d 更新", st.wHour, st.wMinute);
    gSource = source;
    gUpdatedText = buf;
}

static void FetchOnce()
{
    if (gFetching.exchange(true)) return;

    std::string url;
    {
        std::lock_guard<std::mutex> lock(gCfgMx);
        url = gCfg.dataUrl;
    }

    std::string body;
    std::string err;
    bool ok = HttpGetText(Utf8ToWide(url), body, err);

    std::vector<bosstimer::BossData> parsed;
    if (ok)
    {
        std::string perr;
        if (!bosstimer::ParseBossData(body, parsed, &perr))
        {
            ok = false;
            err = perr;
        }
    }

    if (ok)
    {
        {
            std::lock_guard<std::mutex> lock(gBossMx);
            gBosses.swap(parsed);
        }
        WriteFileUtf8(gCachePath, body);
        MarkUpdated("网络");
        gLastError.clear();

        char buf[128];
        snprintf(buf, sizeof(buf), "抓取成功：%d 条 BOSS 数据", (int)gBosses.size());
        LogInfo(buf);
    }
    else
    {
        gLastError = "抓取失败：" + err;
        LogInfo(gLastError);
    }

    gFetching = false;
}

static void FetchLoop()
{
    {
        std::unique_lock<std::mutex> lock(gWakeMx);
        gWakeCv.wait_for(lock, std::chrono::seconds(1), [] { return gStop.load(); });
    }

    while (!gStop.load())
    {
        FetchOnce();

        int minutes = 30;
        {
            std::lock_guard<std::mutex> lock(gCfgMx);
            minutes = gCfg.refreshMin;
        }
        if (minutes < 5) minutes = 5;

        std::unique_lock<std::mutex> lock(gWakeMx);
        gWakeCv.wait_for(lock, std::chrono::minutes(minutes),
            [] { return gStop.load() || gWakeFlag; });
        gWakeFlag = false;
    }
}

static void InitData()
{
    std::string body;
    std::vector<bosstimer::BossData> parsed;
    std::string perr;

    if (ReadFileUtf8(gCachePath, body) && bosstimer::ParseBossData(body, parsed, &perr))
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gBosses.swap(parsed);
        MarkUpdated("缓存");
        return;
    }

    if (bosstimer::ParseBossData(kDefaultData, parsed, &perr))
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gBosses.swap(parsed);
        gSource = "内置";
        gUpdatedText = "首次运行";
    }
}

///----------------------------------------------------------------------------------------------------
/// 发送（GW2 国服：只能 GBK 字节 + 消息投递）
///----------------------------------------------------------------------------------------------------
struct SendJob
{
    std::string body;
    bool overridePrefix = false; // 右键菜单指定的频道
    std::string prefix;
};

static std::deque<SendJob> gSendQ;
static std::mutex gSendMx;
static std::condition_variable gSendCv;
static std::string gStatus;
static DWORD gStatusUntil = 0;
static bool gReleaseFocus = false;

static void PostKeyToGame(HWND h, UINT msg, WPARAM vk, bool up)
{
    LONG scan = (LONG)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    LONG lp = 1 | scan | (up ? (LONG)0xC0000000 : 0);

    if (APIDefs && APIDefs->WndProc_SendToGameOnly)
        APIDefs->WndProc_SendToGameOnly(h, msg, vk, lp);
    else
        PostMessageW(h, msg, vk, lp);
}

static void PostCharToGame(HWND h, wchar_t ch)
{
    if (APIDefs && APIDefs->WndProc_SendToGameOnly)
        APIDefs->WndProc_SendToGameOnly(h, WM_CHAR, (WPARAM)ch, 1);
    else
        PostMessageW(h, WM_CHAR, (WPARAM)ch, 1);
}

// UTF-8 → GBK，再逐字节 WM_CHAR。UTF-8 或 WM_IME_CHAR 会让游戏断言闪退。
static void PostGbkText(HWND h, const std::string& utf8, int charGap)
{
    std::wstring w = Utf8ToWide(utf8);
    if (w.empty()) return;

    int n = WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;

    std::string bytes((size_t)n, char(0));
    WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), &bytes[0], n, nullptr, nullptr);

    int gap = charGap;
    if (gap < 1) gap = 1;
    for (size_t i = 0; i < bytes.size(); i++)
    {
        PostCharToGame(h, (wchar_t)(unsigned char)bytes[i]);
        Sleep(gap);
    }
}

static void PressChatAction(HWND h, int sendMode, int sendKeyVk)
{
    if (sendMode == 0 && APIDefs && APIDefs->GameBinds_InvokeAsync)
    {
        APIDefs->GameBinds_InvokeAsync(GB_UiChatFocus, 40);
        return;
    }
    int vk = (sendKeyVk >= 1 && sendKeyVk <= 255) ? sendKeyVk : VK_RETURN;
    PostKeyToGame(h, WM_KEYDOWN, (WPARAM)vk, false);
    PostKeyToGame(h, WM_KEYUP, (WPARAM)vk, true);
}

static void PressSendEnter(HWND h)
{
    PostKeyToGame(h, WM_KEYDOWN, VK_RETURN, false);
    PostKeyToGame(h, WM_KEYUP, VK_RETURN, true);
}

static void SendOne(const SendJob& job)
{
    HWND h = GetForegroundWindow();
    if (!h)
    {
        LogInfo("发送失败：没有前台窗口");
        return;
    }

    std::string prefix;
    int openDelay = 180;
    int charGap = 2;
    int sendMode = 0;
    int sendKeyVk = VK_RETURN;
    {
        std::lock_guard<std::mutex> lock(gCfgMx);
        prefix = gCfg.prefix;
        openDelay = gCfg.openDelay;
        charGap = gCfg.charGap;
        sendMode = gCfg.sendMode;
        sendKeyVk = gCfg.sendKeyVk;
    }
    if (job.overridePrefix) prefix = job.prefix;

    if (!prefix.empty() && prefix[0] == '/') prefix.erase(0, 1);
    std::string line = prefix.empty() ? job.body : ("/" + prefix + " " + job.body);

    PressChatAction(h, sendMode, sendKeyVk);
    Sleep(std::max(60, openDelay));

    PostGbkText(h, line, charGap);
    Sleep(120);

    PressSendEnter(h);
    Sleep(100);

    LogInfo("已发送：" + line);
}

static void SendLoop()
{
    for (;;)
    {
        SendJob job;
        {
            std::unique_lock<std::mutex> lock(gSendMx);
            gSendCv.wait(lock, [] { return gStop.load() || !gSendQ.empty(); });
            if (gStop.load()) return;
            job = gSendQ.front();
            gSendQ.pop_front();
        }
        SendOne(job);
    }
}

static std::string PrefixLabel();

static void QueueJob(const SendJob& job, const std::string& channelLabel)
{
    {
        std::lock_guard<std::mutex> lock(gSendMx);
        gSendQ.push_back(job);
    }
    gSendCv.notify_one();

    gStatus = "已发送 → " + channelLabel;
    gStatusUntil = GetTickCount() + 2500;
    gReleaseFocus = true;
    LogInfo("排队发送（" + channelLabel + "）：" + job.body);
}

static void QueueSend(const std::string& line)
{
    SendJob job;
    job.body = line;
    QueueJob(job, PrefixLabel());
}

static void QueueSendTo(const std::string& line, const std::string& prefix)
{
    SendJob job;
    job.body = line;
    job.overridePrefix = true;
    job.prefix = prefix;
    QueueJob(job, prefix.empty() ? "当前频道" : ("/" + prefix));
}

///----------------------------------------------------------------------------------------------------
/// 面板
///----------------------------------------------------------------------------------------------------
static std::vector<ImVec4> gHitRects;
static std::vector<ImVec4> gHitRectsPrev;
static ImVec4 gPanelRectPrev(0, 0, 0, 0);

static bool gMenuOpen = false;
static std::string gMenuMessage;
static ImVec2 gMenuPos(0, 0);
static ImVec4 gMenuRect(0, 0, 0, 0);
static char gMenuCustom[64] = {};
static bool gLBtnPrev = false;
static bool gRBtnPrev = false;
static bool gMenuClosedThisFrame = false;

static void OpenChannelMenu(const std::string& message, ImVec2 at)
{
    ImGuiIO& io = ImGui::GetIO();
    const float w = 230.0f;
    const float h = 470.0f;
    if (at.x + w > io.DisplaySize.x) at.x = io.DisplaySize.x - w;
    if (at.y + h > io.DisplaySize.y) at.y = io.DisplaySize.y - h;
    if (at.x < 0.0f) at.x = 0.0f;
    if (at.y < 0.0f) at.y = 0.0f;

    gMenuPos = at;
    gMenuMessage = message;
    gMenuCustom[0] = 0;
    gMenuOpen = true;
}

static std::string PrefixLabel()
{
    if (gCfg.prefix.empty()) return "当前频道";
    return "/" + gCfg.prefix;
}

static void KeepPanelOnScreen()
{
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x <= 0.0f || ds.y <= 0.0f) return;

    ImVec2 pos = ImGui::GetWindowPos();
    float maxX = ds.x - 40.0f;
    float maxY = ds.y - 40.0f;

    ImVec2 next(pos.x, pos.y);
    if (next.x < 0.0f) next.x = 0.0f;
    if (next.y < 0.0f) next.y = 0.0f;
    if (next.x > maxX) next.x = maxX;
    if (next.y > maxY) next.y = maxY;
    if (next.x != pos.x || next.y != pos.y) ImGui::SetWindowPos(next);
}

static void DrawChannelMenu()
{
    if (!gMenuOpen) return;

    ImGui::SetNextWindowPos(gMenuPos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.98f);
    ImGui::Begin("##bt_chmenu", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    ImGui::TextDisabled("发送到");
    ImGui::Separator();

    for (int i = 0; i < kPresetCount; i++)
    {
        std::string label = kPresets[i].name;
        if (kPresets[i].code[0]) label += std::string(" ( /") + kPresets[i].code + " )";
        if (ImGui::Selectable(label.c_str()))
        {
            QueueSendTo(gMenuMessage, kPresets[i].code);
            gMenuOpen = false;
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("自定义频道（回车发送）");
    ImGui::SetNextItemWidth(150.0f);
    bool enter = ImGui::InputText("##bt_custom", gMenuCustom, sizeof(gMenuCustom),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool clickedSend = ImGui::Button("发送");

    if (enter || clickedSend)
    {
        std::string prefix = Trim(gMenuCustom);
        if (!prefix.empty() && prefix[0] == '/') prefix.erase(0, 1);
        QueueSendTo(gMenuMessage, prefix);
        gMenuOpen = false;
        gMenuCustom[0] = 0;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) gMenuOpen = false;

    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 s0 = ImGui::GetWindowSize();
    gMenuRect = ImVec4(p0.x, p0.y, p0.x + s0.x, p0.y + s0.y);

    ImGui::End();
}

static void DrawBossRow(const bosstimer::BossRow& row, const bosstimer::BossData& boss,
                        int nowSec, float btnAlpha)
{
    ImGui::PushID(row.bossIndex);

    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float lineH = ImGui::GetTextLineHeight();
    float padX = 8.0f;
    float padY = 5.0f;
    float w = ImGui::GetContentRegionAvail().x;
    if (gCfg.btnW > 0.0f && gCfg.btnW < w) w = gCfg.btnW;
    float h = (gCfg.btnH > 0.0f) ? gCfg.btnH : (lineH * 2.0f + padY * 2.0f);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 b(p.x + w, p.y + h);

    ImGui::InvisibleButton("row", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImVec4 base = gCfg.btnColorSet
        ? ImVec4(gCfg.btnColor[0], gCfg.btnColor[1], gCfg.btnColor[2], 1.0f)
        : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    base.w = btnAlpha;

    ImVec4 bg = base;
    if (row.ongoing)
    {
        bg.x *= 0.55f;
        bg.y *= 0.55f;
        bg.z *= 0.55f;
    }
    if (hovered)
    {
        bg.x = std::min(1.0f, bg.x + 0.10f);
        bg.y = std::min(1.0f, bg.y + 0.10f);
        bg.z = std::min(1.0f, bg.z + 0.10f);
    }

    dl->AddRectFilled(p, b, ImGui::GetColorU32(bg), 4.0f);
    if (hovered)
    {
        ImVec4 border = base;
        border.x = std::min(1.0f, base.x + 0.25f);
        border.y = std::min(1.0f, base.y + 0.25f);
        border.z = std::min(1.0f, base.z + 0.25f);
        border.w = 0.9f;
        dl->AddRect(p, b, ImGui::GetColorU32(border), 4.0f);
    }

    ImU32 mainCol = ImGui::GetColorU32(gCfg.textColorSet
        ? ImVec4(gCfg.textColor[0], gCfg.textColor[1], gCfg.textColor[2], 1.0f)
        : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    ImU32 subCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    const int secondsToNext = row.nextSec - nowSec;
    const bool soon = !row.ongoing && secondsToNext <= 15 * 60;
    ImU32 cdCol = mainCol;
    if (row.ongoing) cdCol = subCol;
    else if (soon) cdCol = ImGui::GetColorU32(ImVec4(1.0f, 0.72f, 0.24f, 1.0f));

    std::string cdText = row.countdown;
    if (soon) cdText = "即将 " + cdText;

    float nameY = p.y + padY;
    dl->AddText(ImVec2(p.x + padX, nameY), mainCol, boss.name.c_str());

    const char* cd = row.countdown.c_str();
    float cdW = ImGui::CalcTextSize(cd).x;
    dl->AddText(ImVec2(b.x - padX - cdW, nameY), cdCol, cd);

    char sub[160];
    snprintf(sub, sizeof(sub), "上次 %s · 下次 %s · %s",
        bosstimer::FormatHm(row.lastSec).c_str(),
        bosstimer::FormatHm(row.nextSec).c_str(),
        boss.map.c_str());
    dl->AddText(ImVec2(p.x + padX, p.y + padY + lineH), subCol, sub);

    if (hovered)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(460.0f);
        ImGui::TextUnformatted(row.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::TextDisabled("左键发送到 %s · 右键选择频道", PrefixLabel().c_str());
        ImGui::EndTooltip();
    }

    if (clicked && !gMenuOpen && !gMenuClosedThisFrame)
    {
        QueueSend(row.message);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        OpenChannelMenu(row.message, ImGui::GetIO().MousePos);
    }

    gHitRects.push_back(ImVec4(p.x, p.y, b.x, b.y));
    ImGui::PopID();
    (void)style;
}

void RenderMain()
{
    if (gReleaseFocus)
    {
        ImGui::SetWindowFocus(nullptr);
        gReleaseFocus = false;
    }
    gHitRects.clear();

    if (!gCfg.showPanel || (gCfg.autoHide && NexusLink && !NexusLink->IsGameplay))
    {
        gMenuOpen = false;
        gHitRectsPrev.clear();
        return;
    }

    std::vector<bosstimer::BossData> bosses;
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        bosses = gBosses;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    const int nowSec = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;

    std::vector<bosstimer::BossRow> rows;
    bosstimer::BuildRows(bosses, nowSec, gCfg.graceMin, rows);

    ImVec2 mp = ImGui::GetIO().MousePos;

    gMenuClosedThisFrame = false;
    const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool lPressed = lDown && !gLBtnPrev;
    gLBtnPrev = lDown;
    const bool rDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool rPressed = rDown && !gRBtnPrev;
    gRBtnPrev = rDown;

    const bool mouseInMenu = gMenuOpen &&
        mp.x >= gMenuRect.x && mp.x <= gMenuRect.z &&
        mp.y >= gMenuRect.y && mp.y <= gMenuRect.w;

    // 菜单打开时点菜单外：关闭菜单；锁定位置时这一下继续穿透给游戏
    if (gMenuOpen && (lPressed || rPressed) && !mouseInMenu)
    {
        gMenuOpen = false;
        gMenuClosedThisFrame = true;
    }

    bool overHit = false;
    for (size_t i = 0; i < gHitRectsPrev.size(); i++)
    {
        const ImVec4& r = gHitRectsPrev[i];
        if (mp.x >= r.x && mp.x <= r.z && mp.y >= r.y && mp.y <= r.w) { overHit = true; break; }
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (gCfg.lockPos) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (gCfg.hideChrome) flags |= ImGuiWindowFlags_NoTitleBar;

    const bool passThrough = gCfg.lockPos && !overHit;
    if (passThrough) flags |= ImGuiWindowFlags_NoInputs;

    const float alpha = gCfg.alphaPct / 100.0f;
    const float btnAlpha = gCfg.btnAlphaPct / 100.0f;

    ImVec4 titleBg = ImGui::GetStyleColorVec4(ImGuiCol_TitleBg);
    ImVec4 titleBgActive = ImGui::GetStyleColorVec4(ImGuiCol_TitleBgActive);
    ImVec4 titleBgCollapsed = ImGui::GetStyleColorVec4(ImGuiCol_TitleBgCollapsed);
    titleBg.w = alpha;
    titleBgActive.w = alpha;
    titleBgCollapsed.w = alpha;
    ImGui::PushStyleColor(ImGuiCol_TitleBg, titleBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, titleBgActive);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, titleBgCollapsed);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::SetNextWindowSize(ImVec2(340, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(alpha);

    bool mouseInPanel = false;
    ImVec2 wp(0, 0), ws(0, 0);
    if (ImGui::Begin("BOSS计时器##bosstimer_main", gCfg.hideChrome ? nullptr : &gCfg.showPanel, flags))
    {
        KeepPanelOnScreen();

        wp = ImGui::GetWindowPos();
        ws = ImGui::GetWindowSize();
        mouseInPanel = (mp.x >= wp.x && mp.x <= wp.x + ws.x && mp.y >= wp.y && mp.y <= wp.y + ws.y);

        if (gCfg.hideChrome)
        {
            float pad = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f -
                        ImGui::GetStyle().ItemSpacing.y;
            if (pad > 0.0f) ImGui::Dummy(ImVec2(0, pad));
        }

        ImGui::TextDisabled("%d 项", (int)rows.size());
        ImGui::SameLine();
        ImGui::TextDisabled("· %s · %s", gSource.c_str(), gUpdatedText.c_str());

        const char* btnLabel = gFetching.load() ? "抓取中" : "刷新";
        float btnW = ImGui::CalcTextSize(btnLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - btnW - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button(btnLabel)) RequestRefresh();
        {
            ImVec2 a = ImGui::GetItemRectMin();
            ImVec2 b = ImGui::GetItemRectMax();
            gHitRects.push_back(ImVec4(a.x, a.y, b.x, b.y));
        }

        float statusH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##rows", ImVec2(0, -statusH), false);

        if (rows.empty())
        {
            ImGui::TextDisabled("还没有数据。点右上角“刷新”，或检查网络与数据地址。");
        }
        for (size_t i = 0; i < rows.size(); i++)
        {
            const bosstimer::BossRow& row = rows[i];
            if (row.bossIndex < 0 || row.bossIndex >= (int)bosses.size()) continue;
            DrawBossRow(row, bosses[(size_t)row.bossIndex], nowSec, btnAlpha);
        }

        ImGui::EndChild();

        if (!gStatus.empty() && GetTickCount() < gStatusUntil)
        {
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "%s", gStatus.c_str());
        }
        else
        {
            ImGui::TextDisabled("左键发送 %s · 右键选频道 · F10 隐藏", PrefixLabel().c_str());
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    DrawChannelMenu();

    const bool mouseInMenuNow = gMenuOpen &&
        mp.x >= gMenuRect.x && mp.x <= gMenuRect.z &&
        mp.y >= gMenuRect.y && mp.y <= gMenuRect.w;

    bool overOtherWindow = false;
    {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        ImGuiWindow* hw = ctx ? ctx->HoveredWindow : nullptr;
        if (hw && !mouseInPanel)
        {
            ImVec2 a = hw->Pos;
            ImVec2 b(hw->Pos.x + hw->Size.x, hw->Pos.y + hw->Size.y);
            overOtherWindow = (mp.x >= a.x && mp.x <= b.x && mp.y >= a.y && mp.y <= b.y);
        }
    }

    if (passThrough && !overOtherWindow && !mouseInMenuNow)
    {
        ImGui::GetIO().WantCaptureMouse = false;
        ImGui::GetIO().MouseDrawCursor = false;
    }

    gPanelRectPrev = ImVec4(wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
    gHitRectsPrev.swap(gHitRects);
    gHitRects.clear();
}

///----------------------------------------------------------------------------------------------------
/// 设置界面
///----------------------------------------------------------------------------------------------------
static int gCaptureKey = 0;

static std::string ComboLabel(int index)
{
    std::string name = kPresets[index].name;
    std::string code = kPresets[index].code;
    if (code.empty()) return name;
    return name + " (/" + code + ")";
}

static std::string KeyName(int vk)
{
    if (vk == VK_RETURN) return "Enter";
    if (vk >= VK_F1 && vk <= VK_F24) return std::string("F") + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk == VK_OEM_2) return "/";
    if (vk == VK_OEM_1) return ";";
    if (vk == VK_SPACE) return "Space";
    if (vk == VK_TAB) return "Tab";
    char buf[24];
    snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return std::string(buf);
}

void RenderOptions()
{
    bool changed = false;

    if (ImGui::CollapsingHeader("数据", ImGuiTreeNodeFlags_DefaultOpen))
    {
        char urlBuf[512];
        snprintf(urlBuf, sizeof(urlBuf), "%s", gCfg.dataUrl.c_str());
        if (ImGui::InputText("数据地址", urlBuf, sizeof(urlBuf)))
        {
            std::lock_guard<std::mutex> lock(gCfgMx);
            gCfg.dataUrl = Trim(urlBuf);
            changed = true;
        }

        if (ImGui::SliderInt("自动刷新（分钟）", &gCfg.refreshMin, 5, 180)) changed = true;

        if (ImGui::Button("立即刷新")) RequestRefresh();
        ImGui::SameLine();

        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(gBossMx);
            count = gBosses.size();
        }
        ImGui::TextDisabled("%s · %s · 共 %d 项", gSource.c_str(), gUpdatedText.c_str(), (int)count);

        if (!gLastError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", gLastError.c_str());

        ImGui::TextDisabled("数据来自星岬岛 BOSS 计时器页面，抓取成功后会缓存到插件目录。");
    }

    if (ImGui::CollapsingHeader("面板", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("显示/隐藏面板：F10（可在 Nexus「按键绑定」里改）");
        if (ImGui::Checkbox("非游戏场景自动隐藏（选人界面等）", &gCfg.autoHide)) changed = true;

        if (ImGui::SliderInt("背景透明度", &gCfg.alphaPct, 0, 100, "%d%%")) changed = true;
        if (ImGui::SliderInt("按钮透明度", &gCfg.btnAlphaPct, 0, 100, "%d%%")) changed = true;

        if (ImGui::ColorEdit3("按钮颜色", gCfg.btnColor)) { gCfg.btnColorSet = true; changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("跟随主题")) { gCfg.btnColorSet = false; changed = true; }

        if (ImGui::ColorEdit3("文字颜色", gCfg.textColor)) { gCfg.textColorSet = true; changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("文字跟随主题")) { gCfg.textColorSet = false; changed = true; }

        if (ImGui::SliderFloat("按钮宽度", &gCfg.btnW, 0.0f, 600.0f, "%.0f")) changed = true;
        if (ImGui::SliderFloat("按钮高度", &gCfg.btnH, 0.0f, 120.0f, "%.0f")) changed = true;
        ImGui::TextDisabled("宽度 0 = 铺满面板，高度 0 = 自动两行");

        if (ImGui::Checkbox("锁定面板位置（空白处鼠标穿透到游戏）", &gCfg.lockPos)) changed = true;
        if (ImGui::Checkbox("隐藏标题栏", &gCfg.hideChrome)) changed = true;
    }

    if (ImGui::CollapsingHeader("排序"))
    {
        if (ImGui::SliderInt("进行中持续时长（分钟）", &gCfg.graceMin, 1, 60)) changed = true;
        ImGui::TextDisabled("开始后这段时间内标记为“进行中”，排在最上面；其余按下次倒计时从近到远，已发生过的自然排在后面。");
        ImGui::TextDisabled("页面规则：开始前 15 分钟“即将开始”，开始后 15 分钟内“正在进行”。");
    }

    if (ImGui::CollapsingHeader("发送", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int cur = -1;
        for (int i = 0; i < kPresetCount; i++)
        {
            if (gCfg.prefix == kPresets[i].code) { cur = i; break; }
        }

        char preview[96];
        if (cur >= 0) snprintf(preview, sizeof(preview), "%s", ComboLabel(cur).c_str());
        else snprintf(preview, sizeof(preview), "自定义 (/%s)", gCfg.prefix.c_str());

        if (ImGui::BeginCombo("发送频道", preview))
        {
            for (int i = 0; i < kPresetCount; i++)
            {
                if (ImGui::Selectable(ComboLabel(i).c_str(), i == cur))
                {
                    gCfg.prefix = kPresets[i].code;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        char prefixBuf[128];
        snprintf(prefixBuf, sizeof(prefixBuf), "%s", gCfg.prefix.c_str());
        if (ImGui::InputText("自定义前缀（不带斜杠）", prefixBuf, sizeof(prefixBuf)))
        {
            gCfg.prefix = Trim(prefixBuf);
            changed = true;
        }
        if (gCfg.prefix.empty()) ImGui::TextDisabled("留空 = 发到当前频道");
        else ImGui::TextDisabled("实际发送：/%s  BOSS信息…", gCfg.prefix.c_str());

        const char* sendModes[] = { "游戏按键（GB_UiChatFocus）", "注入指定按键" };
        ImGui::Combo("打开聊天方式", &gCfg.sendMode, sendModes, 2);
        ImGui::TextDisabled("发送固定为回车（游戏里发送键不可改）");

        if (gCfg.sendMode == 1)
        {
            ImGui::Text("打开聊天的键：%s", KeyName(gCfg.sendKeyVk).c_str());
            ImGui::SameLine();
            if (ImGui::Button("读取按键")) gCaptureKey = 1;

            if (gCaptureKey)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "请按下要使用的按键（Esc 取消）");
                for (int vk = 0x08; vk <= 0xFE && gCaptureKey; vk++)
                {
                    if ((GetAsyncKeyState(vk) & 0x8000) == 0) continue;
                    if (vk == VK_ESCAPE) { gCaptureKey = 0; break; }
                    gCfg.sendKeyVk = vk;
                    gCaptureKey = 0;
                    changed = true;
                }
            }
            else
            {
                ImGui::TextDisabled("点“读取按键”后按一下游戏里的聊天键");
            }
        }

        if (ImGui::SliderInt("打开聊天延迟 (ms)", &gCfg.openDelay, 60, 1200)) changed = true;
        if (ImGui::SliderInt("逐字节间隔 (ms)", &gCfg.charGap, 1, 30)) changed = true;
        ImGui::TextDisabled("消息以 GBK 字节逐字投递（国服唯一可用方式）。");
    }

    if (ImGui::CollapsingHeader("关于"))
    {
        ImGui::TextDisabled("BOSS计时器 v1.0.0.0 · by 摸鱼哥");
        ImGui::TextDisabled("左键点击列表即发送；鼠标悬停可预览将要发送的一行文本。");
        ImGui::TextDisabled("数据来源：gw2.wishingstarmoye.com（星岬岛）");
    }

    if (changed) SaveCfg();
}

///----------------------------------------------------------------------------------------------------
/// 生命周期
///----------------------------------------------------------------------------------------------------
void OnInputBind(const char* aIdentifier, bool aIsRelease)
{
    if (!aIdentifier || aIsRelease) return;
    if (strcmp(aIdentifier, kToggleId) == 0)
    {
        gCfg.showPanel = !gCfg.showPanel;
        SaveCfg();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH: hSelf = hModule; break;
    default: break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    AddonDef.Signature = (uint32_t)-97532;
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "BOSS计时器";
    AddonDef.Version.Major = 1;
    AddonDef.Version.Minor = 0;
    AddonDef.Version.Build = 0;
    AddonDef.Version.Revision = 1;
    AddonDef.Author = "摸鱼哥";
    AddonDef.Description = "读取星岬岛 BOSS 时刻表，面板按倒计时排序，点击发送一行 BOSS 信息到聊天频道。";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;
    return &AddonDef;
}

void AddonLoad(AddonAPI_t* aApi)
{
    APIDefs = aApi;

    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions(
        (void* (*)(size_t, void*))APIDefs->ImguiMalloc,
        (void (*)(void*, void*))APIDefs->ImguiFree);

    if (APIDefs->DataLink_Get)
        NexusLink = (NexusLinkData_t*)APIDefs->DataLink_Get("DL_NEXUS_LINK");

    InitPaths();
    LoadCfg();
    InitData();

    APIDefs->GUI_Register(RT_Render, RenderMain);
    APIDefs->GUI_Register(RT_OptionsRender, RenderOptions);
    APIDefs->InputBinds_RegisterWithString(kToggleId, OnInputBind, "F10");

    gStop = false;
    gFetchThread = std::thread(FetchLoop);
    gSendThread = std::thread(SendLoop);

    LogInfo("BOSS计时器插件已加载。");
}

void AddonUnload()
{
    gStop = true;
    gWakeCv.notify_all();
    gSendCv.notify_all();

    if (gFetchThread.joinable()) gFetchThread.join();
    if (gSendThread.joinable()) gSendThread.join();

    SaveCfg();

    if (APIDefs)
    {
        APIDefs->GUI_Deregister(RenderMain);
        APIDefs->GUI_Deregister(RenderOptions);
        APIDefs->InputBinds_Deregister(kToggleId);
    }

    LogInfo("BOSS计时器插件已卸载。");
}
