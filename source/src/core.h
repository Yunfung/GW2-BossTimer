// BOSS 计时器核心：解析数据、计算上次/下次、排序、生成喊话文本。
// 不依赖 Win32/ImGui，便于离线自测。
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
        for (;;)
        {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p + 1 < end && p[0] == '/' && p[1] == '/')
            {
                while (p < end && *p != '\n') p++;
                continue;
            }
            if (p + 1 < end && p[0] == '/' && p[1] == '*')
            {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
                if (p + 1 < end) p += 2;
                continue;
            }
            break;
        }
    }

    bool At(char c)
    {
        SkipWs();
        return p < end && *p == c;
    }

    bool ParseInt(int& out)
    {
        SkipWs();
        bool neg = false;
        if (p < end && *p == '-') { neg = true; p++; }
        if (p >= end || *p < '0' || *p > '9') return false;
        long v = 0;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        out = (int)(neg ? -v : v);
        return true;
    }

    bool ParseIdent(std::string& out)
    {
        SkipWs();
        if (p >= end) return false;
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) return false;
        const char* s = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                           (*p >= '0' && *p <= '9') || *p == '_')) p++;
        out.assign(s, p);
        return true;
    }

    void SkipParens()
    {
        SkipWs();
        if (p >= end || *p != '(') return;
        int depth = 0;
        while (p < end)
        {
            if (*p == '(') depth++;
            else if (*p == ')')
            {
                depth--;
                if (depth == 0) { p++; return; }
            }
            p++;
        }
    }

    // 跳过动态表达式残留，直到顶层遇到 ',' 或 '}'
    void SkipUntilDelta()
    {
        while (p < end && *p != ',' && *p != '}') p++;
    }

    bool ParseKeyName(std::string& out)
    {
        if (At('"')) return ParseString(out);
        if (p < end && *p >= '0' && *p <= '9')
        {
            int v = 0;
            if (!ParseInt(v)) return false;
            out = std::to_string(v);
            return true;
        }
        return ParseIdent(out);
    }

    const char* Mark()
    {
        SkipWs();
        return p;
    }

    void Reset(const char* mark)
    {
        p = mark;
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
            case '\'': out += '\''; break;
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
                if (!ParseKeyName(key)) return false;
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
            if (!r.ParseKeyName(key)) { if (err) *err = "字段名解析失败"; return false; }
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

// ---------- 事件时间表（星岬岛 gw2timer 页面） ----------
struct EventSegment
{
    std::string name;
    std::string chatlink;
    std::string copy;
};

struct EventStep
{
    int r = 0; // segments 下标
    int d = 0; // 持续分钟
};

struct EventEntry
{
    std::string category;
    std::string name;
    std::map<int, EventSegment> segments;
    std::vector<EventStep> partial;
    std::vector<EventStep> pattern;
    std::vector<std::vector<EventStep>> dayPartial; // 按星期（周一=0）
    bool zeghai = false;                            // partial 由 calcZeghai() 动态生成
};

struct SeqSeg
{
    int r = 0;
    int s = 0; // 距 00:00 UTC 的分钟
    int e = 0;
};

struct EventRow
{
    int entryIndex = -1;
    int lastLocalMin = 0;
    int nextLocalMin = 0;
    long long secondsToNext = 0;
    std::string countdown;
    std::string label;
    std::string chatlink;
    std::string message;
};

inline std::string StripHtml(const std::string& s)
{
    std::string out;
    bool inTag = false;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    // 去掉首尾空白
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = out.find_last_not_of(" \t\r\n");
    return out.substr(a, b - a + 1);
}

inline bool ParseKey(JsonReader& r, std::string& out)
{
    if (r.At('"')) return r.ParseString(out);
    return r.ParseIdent(out);
}

inline bool ParseStepArray(JsonReader& r, std::vector<EventStep>& out)
{
    if (!r.Peek('[')) return false;
    for (;;)
    {
        r.SkipWs();
        if (r.Peek(']')) break;
        if (!r.Peek('{')) return false;
        EventStep st;
        for (;;)
        {
            r.SkipWs();
            if (r.Peek('}')) break;
            std::string k;
            if (!r.ParseKeyName(k)) return false;
            if (!r.Peek(':')) return false;
            if (k == "r") { if (!r.ParseInt(st.r)) return false; }
            else if (k == "d") { if (!r.ParseInt(st.d)) return false; }
            else if (!r.SkipValue(1)) return false;
            r.SkipWs();
            if (r.Peek(',')) continue;
            if (!r.Peek('}')) return false;
            break;
        }
        out.push_back(st);
        r.SkipWs();
        if (r.Peek(',')) continue;
        if (!r.Peek(']')) return false;
        break;
    }
    return true;
}

inline bool ParseStepArrays(JsonReader& r, std::vector<std::vector<EventStep>>& out)
{
    if (!r.Peek('[')) return false;
    for (;;)
    {
        r.SkipWs();
        if (r.Peek(']')) break;
        std::vector<EventStep> day;
        if (!ParseStepArray(r, day)) return false;
        out.push_back(day);
        r.SkipWs();
        if (r.Peek(',')) continue;
        if (!r.Peek(']')) return false;
        break;
    }
    return true;
}

inline bool ParseSegments(JsonReader& r, std::map<int, EventSegment>& out)
{
    if (!r.Peek('{')) return false;
    for (;;)
    {
        r.SkipWs();
        if (r.Peek('}')) break;

        int idx = -1;
        if (r.At('"'))
        {
            std::string key;
            if (!r.ParseKeyName(key)) return false;
            idx = atoi(key.c_str());
        }
        else if (!r.ParseInt(idx))
        {
            return false;
        }

        if (!r.Peek(':')) return false;
        if (!r.Peek('{')) return false;

        EventSegment seg;
        std::string tname;
        for (;;)
        {
            r.SkipWs();
            if (r.Peek('}')) break;
            std::string key;
            if (!r.ParseKeyName(key)) return false;
            if (!r.Peek(':')) return false;

            if (key == "name" || key == "chatlink" || key == "copy" || key == "tname")
            {
                std::string val;
                if (!r.ParseString(val)) return false;
                if (key == "name") seg.name = val;
                else if (key == "chatlink") seg.chatlink = val;
                else if (key == "copy") seg.copy = val;
                else tname = val;
            }
            else if (!r.SkipValue(1))
            {
                return false;
            }

            r.SkipWs();
            if (r.Peek(',')) continue;
            if (!r.Peek('}')) return false;
            break;
        }

        if (!tname.empty() && (seg.name.empty() || seg.name[0] == '<')) seg.name = tname;
        seg.name = StripHtml(seg.name);
        out[idx] = seg;

        r.SkipWs();
        if (r.Peek(',')) continue;
        if (!r.Peek('}')) return false;
        break;
    }
    return true;
}

inline bool ParseSequences(JsonReader& r, EventEntry& entry)
{
    if (!r.Peek('{')) return false;
    for (;;)
    {
        r.SkipWs();
        if (r.Peek('}')) break;
        std::string key;
        if (!r.ParseKeyName(key)) return false;
        if (!r.Peek(':')) return false;

        if (key == "partial")
        {
            bool paren = r.Peek('(');
            const char* mark = r.Mark();
            r.Peek('[');
            if (r.At('['))
            {
                r.Reset(mark);
                if (!ParseStepArrays(r, entry.dayPartial)) return false;
                if (paren) r.Peek(')');
                r.SkipUntilDelta(); // 形如 ])[(new Date()...)] 的星期选择表达式
            }
            else
            {
                r.Reset(mark);
                if (r.At('['))
                {
                    r.Reset(mark);
                    if (!ParseStepArray(r, entry.partial)) return false;
                }
                else
                {
                    std::string id;
                    if (!r.ParseIdent(id)) return false;
                    if (id == "calcZeghai")
                    {
                        entry.zeghai = true;
                        r.SkipParens();
                    }
                    r.SkipUntilDelta();
                }
                if (paren) r.Peek(')');
                r.SkipUntilDelta();
            }
        }
        else if (key == "pattern")
        {
            if (!ParseStepArray(r, entry.pattern)) return false;
        }
        else
        {
            if (!r.SkipValue(1)) return false;
        }

        r.SkipWs();
        if (r.Peek(',')) continue;
        if (!r.Peek('}')) return false;
        break;
    }
    return true;
}

inline bool ParseEventData(const std::string& text, std::vector<EventEntry>& out, std::string* err)
{
    out.clear();

    const char* begin = text.c_str();
    const char* finish = begin + text.size();
    const char* tag = strstr(begin, "eventData");
    const char* obj = tag ? strchr(tag, '{') : strchr(begin, '{');
    if (!obj)
    {
        if (err) *err = "数据里找不到 eventData";
        return false;
    }

    JsonReader r(obj, finish);
    if (!r.Peek('{'))
    {
        if (err) *err = "eventData 起始符错误";
        return false;
    }

    for (;;)
    {
        r.SkipWs();
        if (r.Peek('}')) break;

        std::string key;
        if (!r.ParseKeyName(key)) { if (err) *err = "顶层键解析失败"; return false; }
        if (!r.Peek(':')) { if (err) *err = "缺少冒号"; return false; }

        if (key == "t" || key == "zones")
        {
            if (!r.SkipValue(1)) { if (err) *err = "跳过字段失败"; return false; }
        }
        else
        {
            EventEntry entry;
            if (!r.Peek('{')) { if (err) *err = "条目不是对象"; return false; }

            for (;;)
            {
                r.SkipWs();
                if (r.Peek('}')) break;
                std::string k;
                if (!r.ParseKeyName(k)) { if (err) *err = "字段名解析失败"; return false; }
                if (!r.Peek(':')) { if (err) *err = "缺少冒号"; return false; }

                if (k == "category")
                {
                    if (!r.ParseString(entry.category)) { if (err) *err = "category 解析失败"; return false; }
                }
                else if (k == "name")
                {
                    if (!r.ParseString(entry.name)) { if (err) *err = "name 解析失败"; return false; }
                }
                else if (k == "segments")
                {
                    if (!ParseSegments(r, entry.segments)) { if (err) *err = "segments 解析失败"; return false; }
                }
                else if (k == "sequences")
                {
                    if (!ParseSequences(r, entry)) { if (err) *err = "sequences 解析失败"; return false; }
                }
                else if (!r.SkipValue(1))
                {
                    if (err) *err = "字段跳过失败";
                    return false;
                }

                r.SkipWs();
                if (r.Peek(',')) continue;
                if (!r.Peek('}')) { if (err) *err = "条目未闭合"; return false; }
                break;
            }

            if (!entry.name.empty() && !entry.segments.empty()) out.push_back(entry);
        }

        r.SkipWs();
        if (r.Peek(',')) continue;
        break;
    }

    if (out.empty())
    {
        if (err) *err = "没有事件条目";
        return false;
    }
    return true;
}

inline int StepsDuration(const std::vector<EventStep>& steps)
{
    int total = 0;
    for (size_t i = 0; i < steps.size(); i++) total += steps[i].d;
    return total;
}

// 页面规则：每天 00:00 UTC 从 partial 开始，之后重复 pattern，铺满 25 小时 15 分
inline void BuildEventSequence(const EventEntry& entry, int weekdayMon0, int zeghaiOffset,
                               std::vector<SeqSeg>& out)
{
    out.clear();

    std::vector<EventStep> steps;
    if (entry.zeghai)
    {
        steps.push_back({ 0, 15 });
        for (int i = 1; i <= 51; i++)
        {
            int k = ((zeghaiOffset + i) % 15 + 15) % 15;
            steps.push_back({ k + 1, 25 });
            steps.push_back({ 0, 35 });
        }
    }
    else if (!entry.dayPartial.empty())
    {
        int n = (int)entry.dayPartial.size();
        int idx = ((weekdayMon0 % n) + n) % n;
        steps = entry.dayPartial[(size_t)idx];
    }
    else
    {
        steps = entry.partial;
    }

    const int fill = 1515;
    int total = StepsDuration(steps);
    if (total < fill && !entry.pattern.empty())
    {
        int patternD = StepsDuration(entry.pattern);
        while (patternD > 0 && total < fill)
        {
            steps.insert(steps.end(), entry.pattern.begin(), entry.pattern.end());
            total += patternD;
        }
    }

    int s = 0;
    for (size_t i = 0; i < steps.size(); i++)
    {
        SeqSeg seg;
        seg.r = steps[i].r;
        seg.s = s;
        seg.e = s + steps[i].d;
        s += steps[i].d;
        out.push_back(seg);
    }
}

inline bool BuildEventRow(const EventEntry& entry, const std::vector<SeqSeg>& seq,
                          int nowUtcSec, int tzMin, EventRow& row)
{
    if (seq.empty()) return false;

    const double pos = nowUtcSec / 60.0;
    int cur = -1;
    for (size_t i = 0; i < seq.size(); i++)
    {
        if (seq[i].s <= pos && pos < seq[i].e) { cur = (int)i; break; }
    }
    if (cur < 0)
    {
        for (int i = (int)seq.size() - 1; i >= 0; i--)
        {
            if (seq[i].s <= pos) { cur = i; break; }
        }
        if (cur < 0) cur = 0;
    }

    int nxt = cur + 1;
    int dayAdd = 0;
    if (nxt >= (int)seq.size())
    {
        nxt = 0;
        dayAdd = 1440;
    }

    double seconds = ((seq[(size_t)nxt].s + dayAdd) - pos) * 60.0;
    if (seconds < 0.0) seconds += 86400.0;
    if (seconds < 0.0) seconds = 0.0;

    int lastLocal = ((seq[(size_t)cur].s + tzMin) % 1440 + 1440) % 1440;
    int nextLocal = ((seq[(size_t)nxt].s + dayAdd + tzMin) % 1440 + 1440) % 1440;

    const EventSegment* curSeg = nullptr;
    const EventSegment* nextSeg = nullptr;
    std::map<int, EventSegment>::const_iterator it = entry.segments.find(seq[(size_t)cur].r);
    if (it != entry.segments.end()) curSeg = &it->second;
    it = entry.segments.find(seq[(size_t)nxt].r);
    if (it != entry.segments.end()) nextSeg = &it->second;

    std::string label = (nextSeg && !nextSeg->name.empty()) ? nextSeg->name
                      : (curSeg ? curSeg->name : std::string());
    std::string link = (nextSeg && !nextSeg->chatlink.empty()) ? nextSeg->chatlink
                      : (curSeg ? curSeg->chatlink : std::string());

    row.entryIndex = -1;
    row.lastLocalMin = lastLocal;
    row.nextLocalMin = nextLocal;
    row.secondsToNext = (long long)(seconds + 0.5);
    row.countdown = CountdownText((int)row.secondsToNext);
    row.label = label;
    row.chatlink = link;
    row.message = entry.name + "  上次: " + FormatHm(lastLocal * 60) +
                  "下次: " + FormatHm(nextLocal * 60) +
                  "  " + label + " " + link + " ";
    return true;
}

} // namespace bosstimer
