// 离线验证：用和插件相同的 WinHTTP 参数抓取数据源
#include <Windows.h>
#include <winhttp.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "winhttp.lib")

int main()
{
    const wchar_t* url = L"https://gw2.wishingstarmoye.com/la-assets/gw2timer/data/timerbox_bossdata.js";

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { printf("crack failed\n"); return 1; }

    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) GW2-BossTimer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { printf("open failed %lu\n", GetLastError()); return 1; }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 10000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) { printf("connect failed %lu\n", GetLastError()); return 1; }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { printf("request failed %lu\n", GetLastError()); return 1; }

    std::string body;
    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        printf("HTTP status: %lu\n", (unsigned long)status);

        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) break;
            if (avail == 0) { ok = true; break; }
            size_t old = body.size();
            body.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, &body[old], avail, &read)) break;
            body.resize(old + read);
        }
    }
    else
    {
        printf("request failed, winhttp error %lu\n", GetLastError());
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    printf("bytes: %d, ok: %d\n", (int)body.size(), ok ? 1 : 0);
    if (!body.empty())
    {
        size_t cut = body.size() > 120 ? 120 : body.size();
        printf("head: %.*s\n", (int)cut, body.c_str());
        printf("has_soo: %d\n", body.find("\"淑雯\"") != std::string::npos ? 1 : 0);
    }
    return (ok && body.find("\"淑雯\"") != std::string::npos) ? 0 : 1;
}
