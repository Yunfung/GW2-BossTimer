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
    int filterMode = 0; // 0=全部 1=BOSS 2=事件
    std::vector<std::string> hiddenEvents; // 区域筛选：被隐藏的事件名，空=全部显示
    std::string prefix = ""; // 空 = 发送到当前频道
    int refreshMin = 30;
    std::string dataUrl = "https://gw2.wishingstarmoye.com/la-assets/gw2timer/data/timerbox_bossdata.js";
    std::string eventsPage = "https://gw2.wishingstarmoye.com/gw2timer";
    std::string dailyUrl = "https://gw2.wishingstarmoye.com/daily";
    int openDelay = 180;
    int charGap = 2;
    int sendMode = 0;
    int sendKeyVk = 0x0D;
};

static Config gCfg;
static std::mutex gCfgMx;
static std::string gCfgPath;
static std::string gCachePath;
static std::string gEventCachePath;
static std::string gDailyCachePath;
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
    gEventCachePath = gAddonDir + "\\eventdata_cache.js";
    gDailyCachePath = gAddonDir + "\\dailydata_cache.html";
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
    s += "filterMode=" + std::to_string(gCfg.filterMode) + "\n";
    {
        std::string hidden;
        for (size_t i = 0; i < gCfg.hiddenEvents.size(); i++)
        {
            if (!hidden.empty()) hidden += ";";
            hidden += gCfg.hiddenEvents[i];
        }
        s += "hiddenEvents=" + hidden + "\n";
    }
    s += "prefix=" + gCfg.prefix + "\n";
    s += "refreshMin=" + std::to_string(gCfg.refreshMin) + "\n";
    s += "dataUrl=" + gCfg.dataUrl + "\n";
    s += "eventsPage=" + gCfg.eventsPage + "\n";
    s += "dailyUrl=" + gCfg.dailyUrl + "\n";
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
    else if (key == "filterMode") gCfg.filterMode = atoi(val.c_str());
    else if (key == "hiddenEvents")
    {
        gCfg.hiddenEvents.clear();
        size_t pos = 0;
        while (pos <= val.size())
        {
            size_t next = val.find(';', pos);
            if (next == std::string::npos) next = val.size();
            std::string item = Trim(val.substr(pos, next - pos));
            if (!item.empty()) gCfg.hiddenEvents.push_back(item);
            if (next >= val.size()) break;
            pos = next + 1;
        }
    }
    else if (key == "eventsPage" && !val.empty()) gCfg.eventsPage = val;
    else if (key == "dailyUrl" && !val.empty()) gCfg.dailyUrl = val;
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
    if (gCfg.filterMode < 0 || gCfg.filterMode > 3) gCfg.filterMode = 0;
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
static std::vector<bosstimer::EventEntry> gEvents;
static std::vector<bosstimer::DailyItem> gDailyItems;
static std::vector<bosstimer::DailyList> gDailyLists;
static std::mutex gBossMx;
static std::atomic<int> gBossVer(0);
static std::atomic<int> gEventVer(0);
static std::atomic<int> gDailyVer(0);
static std::string gSource = "内置";
static std::string gUpdatedText = "未更新";
static std::string gEventSource = "未加载";
static std::string gEventUpdatedText;
static std::string gDailySource = "未加载";
static std::string gDailyUpdatedText;
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

static std::string ClockText()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d 更新", st.wHour, st.wMinute);
    return buf;
}

static void MarkUpdated(const char* source)
{
    gSource = source;
    gUpdatedText = ClockText();
}

static void MarkEventUpdated(const char* source)
{
    gEventSource = source;
    gEventUpdatedText = ClockText();
}

static void MarkDailyUpdated(const char* source)
{
    gDailySource = source;
    gDailyUpdatedText = ClockText();
}

static void FetchDaily(std::string& errors)
{
    std::string url;
    {
        std::lock_guard<std::mutex> lock(gCfgMx);
        url = gCfg.dailyUrl;
    }

    std::string html;
    std::string err;
    if (!HttpGetText(Utf8ToWide(url), html, err))
    {
        errors += "日常页面抓取失败：" + err + "\n";
        return;
    }

    std::vector<bosstimer::DailyItem> items;
    std::vector<bosstimer::DailyList> lists;
    std::string perr;
    if (!bosstimer::ParseDailyHtml(html, items, lists, &perr))
    {
        errors += "日常页面解析失败：" + perr + "\n";
        return;
    }

    int itemCount = (int)items.size();
    int listCount = (int)lists.size();
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gDailyItems.swap(items);
        gDailyLists.swap(lists);
        gDailyVer++;
    }
    WriteFileUtf8(gDailyCachePath, html);
    MarkDailyUpdated("网络");

    char buf[128];
    snprintf(buf, sizeof(buf), "日常采集抓取成功：%d 项 + %d 个列表", itemCount, listCount);
    LogInfo(buf);
}

// 页面里引用的数据文件名带日期版本，从 HTML 里找当前地址
static std::string FindEventDataUrl(const std::string& html)
{
    const char* key = "gw2timer_new_data_";
    size_t p = html.find(key);
    if (p == std::string::npos) return std::string();

    size_t start = html.rfind("http", p);
    size_t end = html.find(".js", p);
    if (start == std::string::npos || end == std::string::npos || start > p) return std::string();
    end += 3;
    return html.substr(start, end - start);
}

static void FetchEvents(std::string& errors)
{
    std::string pageUrl;
    {
        std::lock_guard<std::mutex> lock(gCfgMx);
        pageUrl = gCfg.eventsPage;
    }

    std::string html;
    std::string err;
    if (!HttpGetText(Utf8ToWide(pageUrl), html, err))
    {
        errors += "事件页面抓取失败：" + err + "\n";
        return;
    }

    std::string dataUrl = FindEventDataUrl(html);
    if (dataUrl.empty())
    {
        errors += "事件页面里找不到数据文件\n";
        return;
    }

    std::string body;
    if (!HttpGetText(Utf8ToWide(dataUrl), body, err))
    {
        errors += "事件数据抓取失败：" + err + "\n";
        return;
    }

    std::vector<bosstimer::EventEntry> parsed;
    std::string perr;
    if (!bosstimer::ParseEventData(body, parsed, &perr))
    {
        errors += "事件数据解析失败：" + perr + "\n";
        return;
    }

    int count = (int)parsed.size();
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gEvents.swap(parsed); gEventVer++;
    }
    WriteFileUtf8(gEventCachePath, body);
    MarkEventUpdated("网络");

    char buf[96];
    snprintf(buf, sizeof(buf), "事件时间表抓取成功：%d 项", count);
    LogInfo(buf);
}

static void FetchOnce()
{
    if (gFetching.exchange(true)) return;

    std::string errors;

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
            gBosses.swap(parsed); gBossVer++;
        }
        WriteFileUtf8(gCachePath, body);
        MarkUpdated("网络");

        char buf[128];
        snprintf(buf, sizeof(buf), "抓取成功：%d 条 BOSS 数据", (int)gBosses.size());
        LogInfo(buf);
    }
    else
    {
        errors += "BOSS 抓取失败：" + err + "\n";
    }

    FetchEvents(errors);
    FetchDaily(errors);

    gLastError = Trim(errors);
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
        gBosses.swap(parsed); gBossVer++;
        MarkUpdated("缓存");
        return;
    }

    if (bosstimer::ParseBossData(kDefaultData, parsed, &perr))
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gBosses.swap(parsed); gBossVer++;
        gSource = "内置";
        gUpdatedText = "首次运行";
    }

    std::string ebody;
    std::vector<bosstimer::EventEntry> events;
    std::string eerr;
    if (ReadFileUtf8(gEventCachePath, ebody) && bosstimer::ParseEventData(ebody, events, &eerr))
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gEvents.swap(events); gEventVer++;
        MarkEventUpdated("缓存");
    }

    std::string dbody;
    std::vector<bosstimer::DailyItem> ditem;
    std::vector<bosstimer::DailyList> dlists;
    std::string derr;
    if (ReadFileUtf8(gDailyCachePath, dbody) && bosstimer::ParseDailyHtml(dbody, ditem, dlists, &derr))
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        gDailyItems.swap(ditem);
        gDailyLists.swap(dlists);
        gDailyVer++;
        MarkDailyUpdated("缓存");
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

static void QueueSendLines(const std::vector<std::string>& lines, bool overridePrefix = false,
                           const std::string& prefix = std::string())
{
    if (lines.empty()) return;

    std::string channelLabel = overridePrefix
        ? (prefix.empty() ? "当前频道" : ("/" + prefix))
        : PrefixLabel();

    {
        std::lock_guard<std::mutex> lock(gSendMx);
        for (size_t i = 0; i < lines.size(); i++)
        {
            if (lines[i].empty()) continue;
            SendJob job;
            job.body = lines[i];
            job.overridePrefix = overridePrefix;
            job.prefix = prefix;
            gSendQ.push_back(job);
        }
    }
    gSendCv.notify_one();

    gStatus = "已发送 " + std::to_string(lines.size()) + " 条 → " + channelLabel;
    gStatusUntil = GetTickCount() + 2800;
    gReleaseFocus = true;

    for (size_t i = 0; i < lines.size(); i++)
        LogInfo("排队发送（" + channelLabel + "）：" + lines[i]);
}

static void QueueSend(const std::string& line)
{
    QueueSendLines(std::vector<std::string>(1, line));
}

static void QueueSendTo(const std::string& line, const std::string& prefix)
{
    QueueSendLines(std::vector<std::string>(1, line), true, prefix);
}

///----------------------------------------------------------------------------------------------------
/// 面板
///----------------------------------------------------------------------------------------------------
static std::vector<ImVec4> gHitRects;
static std::vector<ImVec4> gHitRectsPrev;
static ImVec4 gPanelRectPrev(0, 0, 0, 0);

static bool gMenuOpen = false;
static std::string gMenuMessage;
static std::vector<std::string> gMenuLines;
static ImVec2 gMenuPos(0, 0);
static ImVec4 gMenuRect(0, 0, 0, 0);
static char gMenuCustom[64] = {};
static bool gLBtnPrev = false;
static bool gRBtnPrev = false;
static bool gMenuClosedThisFrame = false;

static bool gRegionOpen = false;
static ImVec2 gRegionPos(0, 0);
static ImVec4 gRegionRect(0, 0, 0, 0);

static bool EventHidden(const std::string& name)
{
    return std::find(gCfg.hiddenEvents.begin(), gCfg.hiddenEvents.end(), name) != gCfg.hiddenEvents.end();
}

static void SetEventHidden(const std::string& name, bool hidden)
{
    std::vector<std::string>::iterator it =
        std::find(gCfg.hiddenEvents.begin(), gCfg.hiddenEvents.end(), name);
    if (hidden)
    {
        if (it == gCfg.hiddenEvents.end()) gCfg.hiddenEvents.push_back(name);
    }
    else if (it != gCfg.hiddenEvents.end())
    {
        gCfg.hiddenEvents.erase(it);
    }
}

static void OpenRegionMenu(ImVec2 at)
{
    ImGuiIO& io = ImGui::GetIO();
    const float w = 320.0f;
    const float h = 430.0f;
    if (at.x + w > io.DisplaySize.x) at.x = io.DisplaySize.x - w;
    if (at.y + h > io.DisplaySize.y) at.y = io.DisplaySize.y - h;
    if (at.x < 0.0f) at.x = 0.0f;
    if (at.y < 0.0f) at.y = 0.0f;

    gRegionPos = at;
    gRegionOpen = true;
}

static void OpenChannelMenu(const std::vector<std::string>& lines, ImVec2 at)
{
    if (lines.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float w = 230.0f;
    const float h = 470.0f;
    if (at.x + w > io.DisplaySize.x) at.x = io.DisplaySize.x - w;
    if (at.y + h > io.DisplaySize.y) at.y = io.DisplaySize.y - h;
    if (at.x < 0.0f) at.x = 0.0f;
    if (at.y < 0.0f) at.y = 0.0f;

    gMenuPos = at;
    gMenuLines = lines;
    gMenuMessage.clear();
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (i > 0) gMenuMessage += "\n";
        gMenuMessage += lines[i];
    }
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
            QueueSendLines(gMenuLines, true, kPresets[i].code);
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
        QueueSendLines(gMenuLines, true, prefix);
        gMenuOpen = false;
        gMenuCustom[0] = 0;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) gMenuOpen = false;

    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 s0 = ImGui::GetWindowSize();
    gMenuRect = ImVec4(p0.x, p0.y, p0.x + s0.x, p0.y + s0.y);

    ImGui::End();
}

static void DrawRegionMenu(const std::vector<bosstimer::EventEntry>& events)
{
    if (!gRegionOpen) return;

    ImGui::SetNextWindowPos(gRegionPos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.98f);
    ImGui::Begin("##bt_regionmenu", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    ImGui::TextDisabled("区域筛选（勾选显示）");
    ImGui::Separator();

    if (ImGui::Button("全选"))
    {
        gCfg.hiddenEvents.clear();
        SaveCfg();
    }
    ImGui::SameLine();
    if (ImGui::Button("全不选"))
    {
        gCfg.hiddenEvents.clear();
        for (size_t i = 0; i < events.size(); i++) gCfg.hiddenEvents.push_back(events[i].name);
        SaveCfg();
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) gRegionOpen = false;

    ImGui::BeginChild("##regions", ImVec2(300.0f, 330.0f), false);

    struct RegionGroup
    {
        std::string category;
        std::vector<int> items;
    };
    std::vector<RegionGroup> groups;
    for (size_t i = 0; i < events.size(); i++)
    {
        std::string cat = bosstimer::StripHtml(events[i].category);
        size_t g = 0;
        for (; g < groups.size(); g++)
        {
            if (groups[g].category == cat) break;
        }
        if (g >= groups.size())
        {
            RegionGroup ng;
            ng.category = cat;
            groups.push_back(ng);
        }
        groups[g].items.push_back((int)i);
    }

    for (size_t g = 0; g < groups.size(); g++)
    {
        RegionGroup& group = groups[g];
        if (g > 0) ImGui::Spacing();

        int visible = 0;
        for (size_t k = 0; k < group.items.size(); k++)
        {
            if (!EventHidden(events[(size_t)group.items[k]].name)) visible++;
        }
        bool allShown = (visible == (int)group.items.size());

        ImGui::PushID(group.category.c_str());
        if (ImGui::Checkbox("##group", &allShown))
        {
            for (size_t k = 0; k < group.items.size(); k++)
                SetEventHidden(events[(size_t)group.items[k]].name, !allShown);
            SaveCfg();
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextDisabled("%s（%d/%d）", group.category.c_str(), visible, (int)group.items.size());

        ImGui::Indent(18.0f);
        for (size_t k = 0; k < group.items.size(); k++)
        {
            const std::string& name = events[(size_t)group.items[k]].name;
            bool shown = !EventHidden(name);
            if (ImGui::Checkbox(name.c_str(), &shown))
            {
                SetEventHidden(name, !shown);
                SaveCfg();
            }
        }
        ImGui::Unindent(18.0f);
    }

    ImGui::EndChild();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) gRegionOpen = false;

    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 s0 = ImGui::GetWindowSize();
    gRegionRect = ImVec4(p0.x, p0.y, p0.x + s0.x, p0.y + s0.y);

    ImGui::End();
}

struct UiRow
{
    std::string name;
    std::string sub;
    std::string countdown;
    std::string message;
    std::vector<std::string> lines;
    bool ongoing = false;
    bool soon = false;
    long long key = 0;
    bool isEvent = false;
};

static std::vector<std::vector<bosstimer::SeqSeg>> gEventSeqs;
static int gSeqLocalDay = -1;
static int gSeqUtcDay = -1;

static int ZeghaiOffset(const SYSTEMTIME& ut)
{
    SYSTEMTIME start = {};
    start.wYear = 2018;
    start.wMonth = 5;
    start.wDay = 10;

    SYSTEMTIME stop = ut;
    stop.wHour = 0;
    stop.wMinute = 15;
    stop.wSecond = 0;
    stop.wMilliseconds = 0;

    FILETIME f1 = {}, f2 = {};
    if (!SystemTimeToFileTime(&start, &f1) || !SystemTimeToFileTime(&stop, &f2)) return 0;

    ULARGE_INTEGER u1 = {}, u2 = {};
    u1.LowPart = f1.dwLowDateTime;
    u1.HighPart = f1.dwHighDateTime;
    u2.LowPart = f2.dwLowDateTime;
    u2.HighPart = f2.dwHighDateTime;

    long long hours = ((long long)u2.QuadPart - (long long)u1.QuadPart) / 36000000LL;
    if (hours < 0) hours = -hours;
    return (int)(hours / 2) * 2;
}

// 事件时刻表按天生成：星期决定约科防守的表格，UTC 日期决定迷雾商人的路线
static void EnsureEventSeqs(const std::vector<bosstimer::EventEntry>& events,
                            const SYSTEMTIME& lt, const SYSTEMTIME& ut)
{
    int ld = lt.wYear * 10000 + lt.wMonth * 100 + lt.wDay;
    int ud = ut.wYear * 10000 + ut.wMonth * 100 + ut.wDay;
    if (ld == gSeqLocalDay && ud == gSeqUtcDay && gEventSeqs.size() == events.size()) return;

    int weekdayMon0 = (lt.wDayOfWeek + 6) % 7; // 周日=6，周一=0
    int zoff = ZeghaiOffset(ut);

    gEventSeqs.assign(events.size(), std::vector<bosstimer::SeqSeg>());
    for (size_t i = 0; i < events.size(); i++)
        bosstimer::BuildEventSequence(events[i], weekdayMon0, zoff, gEventSeqs[i]);

    gSeqLocalDay = ld;
    gSeqUtcDay = ud;
}

static void DrawRow(const UiRow& row, int id, float btnAlpha)
{
    ImGui::PushID(id);

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

    ImU32 cdCol = mainCol;
    if (row.ongoing) cdCol = subCol;
    else if (row.soon) cdCol = ImGui::GetColorU32(ImVec4(1.0f, 0.72f, 0.24f, 1.0f));

    std::string cdText = row.countdown;
    if (row.soon) cdText = "即将 " + cdText;

    float nameY = p.y + padY;
    dl->AddText(ImVec2(p.x + padX, nameY), mainCol, row.name.c_str());

    const char* cd = cdText.c_str();
    float cdW = ImGui::CalcTextSize(cd).x;
    dl->AddText(ImVec2(b.x - padX - cdW, nameY), cdCol, cd);

    dl->AddText(ImVec2(p.x + padX, p.y + padY + lineH), subCol, row.sub.c_str());

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

    if (clicked && !gMenuOpen && !gRegionOpen && !gMenuClosedThisFrame)
    {
        if (row.lines.empty()) QueueSend(row.message);
        else QueueSendLines(row.lines);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        if (row.lines.empty()) OpenChannelMenu(std::vector<std::string>(1, row.message), ImGui::GetIO().MousePos);
        else OpenChannelMenu(row.lines, ImGui::GetIO().MousePos);
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
        gRegionOpen = false;
        gHitRectsPrev.clear();
        return;
    }

    static std::vector<bosstimer::BossData> cachedBosses;
    static std::vector<bosstimer::EventEntry> cachedEvents;
    static std::vector<bosstimer::DailyItem> cachedDailyItems;
    static std::vector<bosstimer::DailyList> cachedDailyLists;
    static int cachedBossVer = -1;
    static int cachedEventVer = -1;
    static int cachedDailyVer = -1;

    if (cachedBossVer != gBossVer.load() || cachedEventVer != gEventVer.load() ||
        cachedDailyVer != gDailyVer.load())
    {
        std::lock_guard<std::mutex> lock(gBossMx);
        cachedBosses = gBosses;
        cachedEvents = gEvents;
        cachedDailyItems = gDailyItems;
        cachedDailyLists = gDailyLists;
        cachedBossVer = gBossVer.load();
        cachedEventVer = gEventVer.load();
        cachedDailyVer = gDailyVer.load();
    }

    const std::vector<bosstimer::BossData>& bosses = cachedBosses;
    const std::vector<bosstimer::EventEntry>& events = cachedEvents;
    const std::vector<bosstimer::DailyItem>& dailyItems = cachedDailyItems;
    const std::vector<bosstimer::DailyList>& dailyLists = cachedDailyLists;

    SYSTEMTIME lt = {};
    SYSTEMTIME ut = {};
    GetLocalTime(&lt);
    GetSystemTime(&ut);

    const int nowSec = lt.wHour * 3600 + lt.wMinute * 60 + lt.wSecond;
    const int nowUtcSec = ut.wHour * 3600 + ut.wMinute * 60 + ut.wSecond;

    int tzMin = (lt.wHour * 60 + lt.wMinute) - (ut.wHour * 60 + ut.wMinute);
    if (tzMin < -720) tzMin += 1440;
    if (tzMin > 720) tzMin -= 1440;

    EnsureEventSeqs(events, lt, ut);

    std::vector<UiRow> rows;

    if (gCfg.filterMode == 3)
    {
        for (size_t i = 0; i < dailyItems.size(); i++)
        {
            std::string text = bosstimer::ResolveDailyText(dailyItems[i], lt.wYear, lt.wMonth,
                lt.wDay, lt.wDayOfWeek, lt.wHour, lt.wMinute);
            if (text.empty()) continue;

            UiRow u;
            u.name = dailyItems[i].name;
            u.sub = text;
            u.message = text;
            u.lines.push_back(text);
            u.key = (long long)i;
            rows.push_back(u);
        }

        for (size_t i = 0; i < dailyLists.size(); i++)
        {
            const bosstimer::DailyList& list = dailyLists[i];
            std::string oneLine = bosstimer::DailyListLine(list);

            UiRow u;
            u.name = list.name;
            u.sub = oneLine;
            u.message = oneLine;
            u.lines.push_back(oneLine);
            u.key = 1000 + (long long)i;
            rows.push_back(u);
        }
    }
    else
    {
        if (gCfg.filterMode != 2)
        {
            std::vector<bosstimer::BossRow> bossRows;
            bosstimer::BuildRows(bosses, nowSec, gCfg.graceMin, bossRows);

            for (size_t i = 0; i < bossRows.size(); i++)
            {
                const bosstimer::BossRow& br = bossRows[i];
                if (br.bossIndex < 0 || br.bossIndex >= (int)bosses.size()) continue;
                const bosstimer::BossData& bd = bosses[(size_t)br.bossIndex];

                UiRow u;
                u.name = bd.name;
                u.sub = "上次 " + bosstimer::FormatHm(br.lastSec) + " · 下次 " +
                        bosstimer::FormatHm(br.nextSec) + " · " + bd.map;
                u.countdown = br.countdown;
                u.message = br.message;
                u.ongoing = br.ongoing;
                u.soon = !br.ongoing && (br.nextSec - nowSec) <= 15 * 60;
                u.key = br.sortKey;
                rows.push_back(u);
            }
        }

        if (gCfg.filterMode != 1)
        {
            for (size_t i = 0; i < events.size() && i < gEventSeqs.size(); i++)
            {
                if (EventHidden(events[i].name)) continue;

                bosstimer::EventRow er;
                if (!bosstimer::BuildEventRow(events[i], gEventSeqs[i], nowUtcSec, tzMin, er)) continue;

                UiRow u;
                u.name = events[i].name;
                u.sub = "上次 " + bosstimer::FormatHm(er.lastLocalMin * 60) + " · 下次 " +
                        bosstimer::FormatHm(er.nextLocalMin * 60);
                if (!er.label.empty()) u.sub += " · " + er.label;
                u.countdown = er.countdown;
                u.message = er.message;
                u.ongoing = er.ongoing;
                u.soon = !er.ongoing && er.secondsToNext <= 15 * 60;
                u.key = er.sortKey;
                u.isEvent = true;
                rows.push_back(u);
            }
        }
    }

    std::stable_sort(rows.begin(), rows.end(),
        [](const UiRow& a, const UiRow& b) { return a.key < b.key; });

    ImVec2 mp = ImGui::GetIO().MousePos;

    gMenuClosedThisFrame = false;
    const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool lPressed = lDown && !gLBtnPrev;
    gLBtnPrev = lDown;
    const bool rDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool rPressed = rDown && !gRBtnPrev;
    gRBtnPrev = rDown;

    const bool mouseInChannelMenu = gMenuOpen &&
        mp.x >= gMenuRect.x && mp.x <= gMenuRect.z &&
        mp.y >= gMenuRect.y && mp.y <= gMenuRect.w;
    const bool mouseInRegionMenu = gRegionOpen &&
        mp.x >= gRegionRect.x && mp.x <= gRegionRect.z &&
        mp.y >= gRegionRect.y && mp.y <= gRegionRect.w;
    const bool mouseInAnyMenu = mouseInChannelMenu || mouseInRegionMenu;

    // 菜单打开时点菜单外：关闭菜单；锁定位置时这一下继续穿透给游戏
    if ((gMenuOpen || gRegionOpen) && (lPressed || rPressed) && !mouseInAnyMenu)
    {
        gMenuOpen = false;
        gRegionOpen = false;
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

        if (gCfg.filterMode == 3)
            ImGui::TextDisabled("日常采集 %d 项 · 列表 %d", (int)dailyItems.size(), (int)dailyLists.size());
        else
            ImGui::TextDisabled("BOSS %d · 事件 %d", (int)bosses.size(), (int)events.size());

        const char* btnLabel = gFetching.load() ? "抓取中" : "刷新";
        float btnW = ImGui::CalcTextSize(btnLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - btnW - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button(btnLabel)) RequestRefresh();
        {
            ImVec2 a = ImGui::GetItemRectMin();
            ImVec2 b = ImGui::GetItemRectMax();
            gHitRects.push_back(ImVec4(a.x, a.y, b.x, b.y));
        }

        const char* filterNames[4] = { "全部", "BOSS", "事件", "日常采集" };
        for (int i = 0; i < 4; i++)
        {
            if (i > 0) ImGui::SameLine();
            bool active = (gCfg.filterMode == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.62f, 0.15f, 1.0f));
            if (ImGui::Button(filterNames[i]))
            {
                gCfg.filterMode = i;
                gRegionOpen = false;
                SaveCfg();
            }
            if (i == 2 && ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                OpenRegionMenu(ImGui::GetIO().MousePos);
            }
            if (active) ImGui::PopStyleColor();
            {
                ImVec2 a = ImGui::GetItemRectMin();
                ImVec2 b = ImGui::GetItemRectMax();
                gHitRects.push_back(ImVec4(a.x, a.y, b.x, b.y));
            }
        }

        float statusH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##rows", ImVec2(0, -statusH), false);

        if (rows.empty())
        {
            ImGui::TextDisabled("还没有数据。点右上角“刷新”，或检查网络与数据地址。");
        }
        for (size_t i = 0; i < rows.size(); i++)
        {
            DrawRow(rows[i], (int)i, btnAlpha);
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
    DrawRegionMenu(events);

    const bool mouseInMenuNow =
        (gMenuOpen && mp.x >= gMenuRect.x && mp.x <= gMenuRect.z &&
         mp.y >= gMenuRect.y && mp.y <= gMenuRect.w) ||
        (gRegionOpen && mp.x >= gRegionRect.x && mp.x <= gRegionRect.z &&
         mp.y >= gRegionRect.y && mp.y <= gRegionRect.w);

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
        if (ImGui::InputText("BOSS 数据地址", urlBuf, sizeof(urlBuf)))
        {
            std::lock_guard<std::mutex> lock(gCfgMx);
            gCfg.dataUrl = Trim(urlBuf);
            changed = true;
        }

        char pageBuf[512];
        snprintf(pageBuf, sizeof(pageBuf), "%s", gCfg.eventsPage.c_str());
        if (ImGui::InputText("事件页面地址", pageBuf, sizeof(pageBuf)))
        {
            std::lock_guard<std::mutex> lock(gCfgMx);
            gCfg.eventsPage = Trim(pageBuf);
            changed = true;
        }

        char dailyBuf[512];
        snprintf(dailyBuf, sizeof(dailyBuf), "%s", gCfg.dailyUrl.c_str());
        if (ImGui::InputText("日常页面地址", dailyBuf, sizeof(dailyBuf)))
        {
            std::lock_guard<std::mutex> lock(gCfgMx);
            gCfg.dailyUrl = Trim(dailyBuf);
            changed = true;
        }

        if (ImGui::SliderInt("自动刷新（分钟）", &gCfg.refreshMin, 5, 180)) changed = true;

        if (ImGui::Button("立即刷新")) RequestRefresh();
        ImGui::SameLine();
        if (gFetching.load()) ImGui::TextDisabled("抓取中…");
        else ImGui::TextDisabled("会同时更新 BOSS、事件与日常数据");

        size_t bossCount = 0;
        size_t eventCount = 0;
        size_t dailyCount = 0;
        size_t dailyListCount = 0;
        {
            std::lock_guard<std::mutex> lock(gBossMx);
            bossCount = gBosses.size();
            eventCount = gEvents.size();
            dailyCount = gDailyItems.size();
            dailyListCount = gDailyLists.size();
        }
        ImGui::TextDisabled("BOSS：%s · %s · 共 %d 项", gSource.c_str(), gUpdatedText.c_str(), (int)bossCount);
        ImGui::TextDisabled("事件：%s · %s · 共 %d 项", gEventSource.c_str(),
            gEventUpdatedText.empty() ? "未更新" : gEventUpdatedText.c_str(), (int)eventCount);
        ImGui::TextDisabled("日常：%s · %s · %d 项 + %d 个列表", gDailySource.c_str(),
            gDailyUpdatedText.empty() ? "未更新" : gDailyUpdatedText.c_str(), (int)dailyCount, (int)dailyListCount);

        if (!gLastError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", gLastError.c_str());

        ImGui::TextDisabled("数据来自星岬岛 BOSS 计时器与事件时间表，抓取成功后会缓存到插件目录。");
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
    AddonDef.Version.Revision = 8;
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
