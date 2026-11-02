// BOSS 计时器核心：解析数据、计算上次/下次、排序、生成喊话文本。
// 不依赖 Win32/ImGui，便于离线自测。
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bosstimer
{

struct BossData
{
    std::string name;
    std::string map;
    std::string point;
    std::vector<int> times; // 当天分钟数 0..1439，升序
};

struct BossRow
{
    int bossIndex = -1;
    int lastSec = 0;        // 上次开始（当天秒数，可为负=昨天）
    int nextSec = 0;        // 下次开始（当天秒数，可 >86400=明天）
    bool ongoing = false;   // 刚开始不久，排到最下面
    long long sortKey = 0;
    std::string message;    // 点击后发送的一行文本
    std::string countdown;  // 面板右侧倒计时
};

// ---------- JSON 读取（只覆盖页面数据用到的子集） ----------
class JsonReader
{
public:
    JsonReader(const char* a, const char* b) : p(a), end(b) {}

    void SkipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    }

    bool Peek(char c)
    {
        SkipWs();
        if (p < end && *p == c) { p++; return true; }
        return false;
    }

    bool ParseString(std::string& out)
    {
        SkipWs();
        if (p >= end || *p != '"') return false;
        p++;
        out.clear();
        while (p < end)
        {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') return true;
            if (c != '\\') { out += (char)c; continue; }
            if (p >= end) return false;
            char esc = *p++;
            switch (esc)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                unsigned cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && p[0] == '\\' && p[1] == 'u')
                {
                    p += 2;
                    unsigned low = 0;
                    if (!Hex4(low)) return false;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool SkipValue(int depth = 0)
    {
        if (depth > 32) return false;
        SkipWs();
        if (p >= end) return false;

        if (*p == '"') { std::string tmp; return ParseString(tmp); }
        if (*p == '{')
        {
            p++;
            if (Peek('}')) return true;
            for (;;)
            {
                std::string key;
                if (!ParseString(key)) return false;
                if (!Peek(':')) return false;
                if (!SkipValue(depth + 1)) return false;
                SkipWs();
                if (Peek(',')) continue;
                return Peek('}');
            }
        }
        if (*p == '[')
        {
            p++;
            if (Peek(']')) return true;
            for (;;)
            {
                if (!SkipValue(depth + 1)) return false;
                SkipWs();
                if (Peek(',')) continue;
                return Peek(']');
            }
        }
        const char* start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        return p > start;
    }

private:
    bool Hex4(unsigned& out)
    {
        if (p + 4 > end) return false;
        out = 0;
        for (int i = 0; i < 4; i++)
        {
            char c = p[i];
            unsigned v;
            if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (unsigned)(c - 'A' + 10);
            else return false;
            out = (out << 4) | v;
        }
        p += 4;
        return true;
    }

    static void AppendUtf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) { s += (char)cp; return; }
        if (cp < 0x800)
        {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
            return;
        }
        if (cp < 0x10000)
        {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
            return;
        }
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }

    const char* p;
    const char* end;
};

inline bool ParseClock(const std::string& s, int& minutes)
{
    int h = 0, m = 0;
    if (sscanf_s(s.c_str(), "%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    minutes = h * 60 + m;
    return true;
}

// 解析 "var bossdata = [...];" 或纯 JSON 数组
inline bool ParseBossData(const std::string& text, std::vector<BossData>& out, std::string* err)
{
    out.clear();

    const char* begin = text.c_str();
    const char* finish = begin + text.size();
    const char* arr = nullptr;

    const char* tag = strstr(begin, "bossdata");
    if (tag) arr = strchr(tag, '[');
    if (!arr) arr = strchr(begin, '[');
    if (!arr)
    {
        if (err) *err = "数据里找不到数组";
        return false;
    }

    JsonReader r(arr, finish);
    if (!r.Peek('['))
    {
        if (err) *err = "数组起始符错误";
        return false;
    }

    for (;;)
    {
        r.SkipWs();
        if (r.Peek(']')) break;

        if (!r.Peek('{'))
        {
            if (err) *err = "条目不是对象";
            return false;
        }

        BossData boss;
        for (;;)
        {
            r.SkipWs();
            if (r.Peek('}')) break;

            std::string key;
            if (!r.ParseString(key)) { if (err) *err = "字段名解析失败"; return false; }
            if (!r.Peek(':')) { if (err) *err = "缺少冒号"; return false; }

            if (key == "name" || key == "map" || key == "point")
            {
                std::string val;
                if (!r.ParseString(val)) { if (err) *err = "字段值解析失败"; return false; }
                if (key == "name") boss.name = val;
                else if (key == "map") boss.map = val;
                else boss.point = val;
            }
            else if (key == "time")
            {
                if (!r.Peek('[')) { if (err) *err = "time 不是数组"; return false; }
                for (;;)
                {
                    r.SkipWs();
                    if (r.Peek(']')) break;
                    std::string ts;
                    if (!r.ParseString(ts)) { if (err) *err = "时刻解析失败"; return false; }
                    int m = 0;
                    if (ParseClock(ts, m)) boss.times.push_back(m);
                    r.SkipWs();
                    if (r.Peek(',')) continue;
                    if (!r.Peek(']')) { if (err) *err = "time 数组未闭合"; return false; }
                    break;
                }
            }
            else
            {
                if (!r.SkipValue(1)) { if (err) *err = "字段跳过失败"; return false; }
            }

            r.SkipWs();
            if (r.Peek(',')) continue;
            if (!r.Peek('}')) { if (err) *err = "对象未闭合"; return false; }
            break;
        }

        if (!boss.name.empty() && !boss.times.empty())
        {
            std::sort(boss.times.begin(), boss.times.end());
            boss.times.erase(std::unique(boss.times.begin(), boss.times.end()), boss.times.end());
            out.push_back(boss);
        }

        r.SkipWs();
        if (r.Peek(',')) continue;
        break;
    }

    if (out.empty())
    {
        if (err) *err = "没有有效条目";
        return false;
    }
    return true;
}

inline std::string FormatHm(int secOfDay)
{
    int s = secOfDay % 86400;
    if (s < 0) s += 86400;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", s / 3600, (s % 3600) / 60);
    return buf;
}

inline std::string BuildMessage(const BossData& boss, int lastSec, int nextSec)
{
    return boss.name + "  上次: " + FormatHm(lastSec) + "下次: " + FormatHm(nextSec) +
           "  " + boss.map + " " + boss.point + " ";
}

inline std::string CountdownText(int seconds)
{
    if (seconds < 60) return "即将开始";
    if (seconds >= 3600)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d时%02d分", seconds / 3600, (seconds % 3600) / 60);
        return buf;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d分", seconds / 60);
    return buf;
}

// nowSec：当天秒数；graceMin：刚结束多久内的 BOSS 算“已发生”并排到最后
inline void BuildRows(const std::vector<BossData>& bosses, int nowSec, int graceMin,
                      std::vector<BossRow>& out)
{
    out.clear();
    const int graceSec = (graceMin > 0 ? graceMin : 0) * 60;

    for (size_t i = 0; i < bosses.size(); i++)
    {
        const BossData& boss = bosses[i];
        if (boss.times.empty()) continue;

        // 跨零点：当天还没到第一个时刻时，“上次”应是昨天最后一个时刻
        int lastSec = boss.times.back() * 60 - 86400;
        int nextSec = boss.times.front() * 60 + 86400;
        for (size_t k = 0; k < boss.times.size(); k++)
        {
            int t = boss.times[k] * 60;
            if (t <= nowSec) lastSec = t;
            else { nextSec = t; break; }
        }

        BossRow row;
        row.bossIndex = (int)i;
        row.ongoing = (nowSec - lastSec) < graceSec;
        // 进行中排最上面（按开始先后）；其余按“下次开始”的倒计时从近到远
        row.sortKey = row.ongoing ? (long long)(lastSec - nowSec) : (long long)(nextSec - nowSec);

        // 和页面口径一致：进行中的卡片显示“上一轮 / 本轮”，其余显示“上次 / 下次”
        int dispLast = lastSec;
        int dispNext = nextSec;
        if (row.ongoing)
        {
            int lastMod = ((lastSec % 86400) + 86400) % 86400;
            int idx = -1;
            for (size_t k = 0; k < boss.times.size(); k++)
            {
                if (boss.times[k] * 60 == lastMod) { idx = (int)k; break; }
            }
            if (idx >= 0)
            {
                int n = (int)boss.times.size();
                int prev = (idx - 1 + n) % n;
                dispLast = boss.times[(size_t)prev] * 60;
                dispNext = lastSec;
            }
        }

        row.lastSec = dispLast;
        row.nextSec = dispNext;
        row.message = BuildMessage(boss, dispLast, dispNext);
        row.countdown = row.ongoing ? "进行中" : CountdownText(nextSec - nowSec);
        out.push_back(row);
    }

    std::stable_sort(out.begin(), out.end(),
        [](const BossRow& a, const BossRow& b) {
            if (a.sortKey != b.sortKey) return a.sortKey < b.sortKey;
            return a.bossIndex < b.bossIndex;
        });
}

} // namespace bosstimer
