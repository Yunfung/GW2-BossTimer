// 离线条目自测：真实页面数据 → 解析 → 计时 → 排序 → 一行文本
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core.h"
#include "defaultdata.h"

static int gFail = 0;

static void Check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "[OK]  " : "[FAIL]", what);
    if (!ok) gFail++;
}

static bool HasBoss(const std::vector<bosstimer::BossData>& bosses, const char* name)
{
    for (size_t i = 0; i < bosses.size(); i++)
        if (bosses[i].name == name) return true;
    return false;
}

int main(int argc, char** argv)
{
    std::vector<bosstimer::BossData> bosses;
    std::string err;

    Check(bosstimer::ParseBossData(kDefaultData, bosses, &err), "内置兜底数据可解析");
    printf("      内置条目：%d\n", (int)bosses.size());
    Check(bosses.size() == 24, "内置数据 24 条");
    if (!err.empty()) printf("      解析错误：%s\n", err.c_str());

    if (argc > 1)
    {
        std::ifstream in(argv[1], std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<bosstimer::BossData> web;
        std::string e2;
        bool ok = bosstimer::ParseBossData(text, web, &e2);
        Check(ok, "网页数据文件可解析");
        printf("      网页条目：%d\n", (int)web.size());
        if (!e2.empty()) printf("      解析错误：%s\n", e2.c_str());
        Check(web.size() == bosses.size(), "网页条目数与内置一致");
    }

    const int nowSec = 22 * 3600 + 5 * 60; // 22:05
    std::vector<bosstimer::BossRow> rows;
    bosstimer::BuildRows(bosses, nowSec, 15, rows);

    printf("      22:05 排序：\n");
    for (size_t i = 0; i < rows.size(); i++)
    {
        printf("        %2d. %-20s %-8s %s\n", (int)i + 1,
            bosses[(size_t)rows[i].bossIndex].name.c_str(),
            rows[i].countdown.c_str(),
            rows[i].ongoing ? "进行中" : "");
    }

    Check(rows.size() == bosses.size(), "全部条目进入排序");
    Check(rows.front().ongoing, "进行中排在最上面");
    Check(HasBoss(bosses, "淑雯"), "数据里有淑雯");

    bool seenUpcoming = false;
    bool groupOk = true;
    for (size_t i = 0; i < rows.size(); i++)
    {
        if (rows[i].ongoing)
        {
            if (seenUpcoming) groupOk = false;
        }
        else
        {
            seenUpcoming = true;
        }
    }
    Check(groupOk, "进行中的条目全部在最上面");

    const bosstimer::BossRow* firstUp = nullptr;
    for (size_t i = 0; i < rows.size(); i++)
    {
        if (!rows[i].ongoing) { firstUp = &rows[i]; break; }
    }
    if (firstUp)
    {
        printf("      进行中之后第一条：%s（%s）\n",
            bosses[(size_t)firstUp->bossIndex].name.c_str(), firstUp->countdown.c_str());
        Check(bosses[(size_t)firstUp->bossIndex].name == "苍翠夜晚", "进行中之后是最近要开始的 22:10");
        Check(firstUp->countdown == "5分", "22:10 的倒计时为 5 分");
    }
    else
    {
        Check(false, "找到进行中之后的条目");
    }

    long long prevKey = -1;
    bool asc = true;
    for (size_t i = 0; i < rows.size(); i++)
    {
        if (rows[i].ongoing) continue;
        long long k = rows[i].nextSec - nowSec;
        if (k < prevKey) asc = false;
        prevKey = k;
    }
    Check(asc, "其余条目按下次倒计时从近到远");

    const bosstimer::BossRow* soo = nullptr;
    for (size_t i = 0; i < rows.size(); i++)
        if (bosses[(size_t)rows[i].bossIndex].name == "淑雯") soo = &rows[i];

    if (!soo)
    {
        Check(false, "找到淑雯条目");
    }
    else
    {
        printf("      淑雯文本：[%s]\n", soo->message.c_str());
        Check(soo->message == "淑雯  上次: 21:00下次: 23:00  巨龙尽头 [&BKIMAAA=] ",
            "淑雯文本与用户给出的示例完全一致");
        Check(soo->countdown == "55分", "淑雯倒计时为 55 分");
    }

    std::vector<bosstimer::BossRow> rows2;
    bosstimer::BuildRows(bosses, 22 * 3600, 15, rows2); // 正好 22:00
    const bosstimer::BossRow& top2 = rows2.front();
    printf("      22:00 第一条：%s（%s）\n",
        bosses[(size_t)top2.bossIndex].name.c_str(), top2.countdown.c_str());
    Check(top2.ongoing, "整点刚刷新的 BOSS 视为进行中并排最上面");

    // 跨零点：00:05 时“上次”应是昨天最后一个时刻
    std::vector<bosstimer::BossRow> rows3;
    bosstimer::BuildRows(bosses, 5 * 60, 15, rows3);
    const bosstimer::BossRow* soo3 = nullptr;
    const bosstimer::BossRow* kai3 = nullptr;
    for (size_t i = 0; i < rows3.size(); i++)
    {
        const std::string& n = bosses[(size_t)rows3[i].bossIndex].name;
        if (n == "淑雯") soo3 = &rows3[i];
        if (n == "凯宁城断电") kai3 = &rows3[i];
    }
    if (soo3)
    {
        printf("      00:05 淑雯文本：[%s]\n", soo3->message.c_str());
        Check(soo3->message == "淑雯  上次: 23:00下次: 01:00  巨龙尽头 [&BKIMAAA=] ",
            "跨零点后淑雯上次仍是 23:00");
        Check(soo3->countdown == "55分", "跨零点后淑雯倒计时为 55 分");
    }
    else
    {
        Check(false, "跨零点找到淑雯条目");
    }
    if (kai3) Check(kai3->ongoing, "00:00 刷新的凯宁城断电仍算进行中");
    else Check(false, "跨零点找到凯宁城断电条目");
    if (kai3)
    {
        printf("      00:05 进行中文本：[%s]\n", kai3->message.c_str());
        Check(kai3->message == "凯宁城断电  上次: 22:00下次: 00:00  新凯宁城 [&BBkNAAA=] ",
            "进行中的条目与页面一致（上一轮 / 本轮）");
    }

    // 23:30：下次应跨到明天第一个时刻
    std::vector<bosstimer::BossRow> rows4;
    bosstimer::BuildRows(bosses, 23 * 3600 + 30 * 60, 15, rows4);
    const bosstimer::BossRow* soo4 = nullptr;
    for (size_t i = 0; i < rows4.size(); i++)
        if (bosses[(size_t)rows4[i].bossIndex].name == "淑雯") soo4 = &rows4[i];
    if (soo4)
    {
        printf("      23:30 淑雯文本：[%s]\n", soo4->message.c_str());
        Check(soo4->message == "淑雯  上次: 23:00下次: 01:00  巨龙尽头 [&BKIMAAA=] ",
            "23:30 时下次跨到明天 01:00");
        Check(soo4->countdown == "1时30分", "23:30 时倒计时为 1时30分");
    }
    else
    {
        Check(false, "23:30 找到淑雯条目");
    }

    // 长文本可放进一行：GW2 输入上限 199 字符
    size_t maxLen = 0;
    for (size_t i = 0; i < rows.size(); i++)
        maxLen = (std::max)(maxLen, rows[i].message.size());
    printf("      最长一行（UTF-8 字节）：%d\n", (int)maxLen);
    Check(maxLen < 199, "一行文本长度安全");

    // 事件时间表（可选：第二个参数是 gw2timer_new_data_*.js）
    if (argc > 2)
    {
        std::ifstream in2(argv[2], std::ios::binary);
        std::string t2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        std::vector<bosstimer::EventEntry> ev;
        std::string e3;
        Check(bosstimer::ParseEventData(t2, ev, &e3), "事件时间表可解析");
        printf("      事件条目：%d\n", (int)ev.size());
        if (!e3.empty()) printf("      解析错误：%s\n", e3.c_str());
        Check(ev.size() == 45, "事件条目 45 个");

        int withSeg = 0;
        for (size_t i = 0; i < ev.size(); i++)
            if (!ev[i].segments.empty()) withSeg++;
        Check(withSeg == (int)ev.size(), "每个事件都有阶段");

        for (size_t i = 0; i < ev.size(); i++)
        {
            if (ev[i].name != "世界BOSS") continue;
            std::vector<bosstimer::SeqSeg> seq;
            bosstimer::BuildEventSequence(ev[i], 0, 0, seq);
            bosstimer::EventRow er;
            bool ok = bosstimer::BuildEventRow(ev[i], seq, 12 * 3600, 480, er);
            Check(ok, "世界BOSS 12:00 UTC 可计算");
            printf("      12:00 UTC / 20:00 北京：%s\n", er.message.c_str());
            printf("      倒计时：%s\n", er.countdown.c_str());
            Check(er.countdown == "15分", "世界BOSS 倒计时 15 分");
            Check(er.lastLocalMin == 20 * 60 && er.nextLocalMin == 20 * 60 + 15,
                "世界BOSS 北京时间 20:00 / 20:15");
        }

        // 跳过空阶段：四风节在 00:05 UTC 处于“寻宝开始”(0-10)，下一个有名阶段是 00:30 的“准备”
        for (size_t i = 0; i < ev.size(); i++)
        {
            if (ev[i].name != "四风节-迷城峭壁") continue;
            std::vector<bosstimer::SeqSeg> seq;
            bosstimer::BuildEventSequence(ev[i], 0, 0, seq);

            bosstimer::EventRow er;
            Check(bosstimer::BuildEventRow(ev[i], seq, 5 * 60, 480, er), "四风节 00:05 可计算");
            printf("      00:05 UTC 四风节：%s\n", er.message.c_str());
            Check(er.lastLocalMin == 8 * 60, "上次是当前事件开始 08:00");
            Check(er.nextLocalMin == 8 * 60 + 30, "下次跳过空阶段，取下一个事件开始 08:30");
            Check(er.countdown == "25分", "倒计时 25 分（到事件开始，不是阶段结束）");

            bosstimer::EventRow er2;
            Check(bosstimer::BuildEventRow(ev[i], seq, 12 * 60, 480, er2), "四风节 00:12 可计算");
            printf("      00:12 UTC 四风节（空阶段中）：%s\n", er2.message.c_str());
            Check(er2.lastLocalMin == 8 * 60, "空阶段中上次仍是上一个事件开始 08:00");
            Check(er2.nextLocalMin == 8 * 60 + 30, "空阶段中下次仍是 08:30");
            Check(er2.countdown == "18分", "空阶段倒计时 18 分");
        }
    }

    // 日常采集（可选：第三个参数是 daily 页面 HTML）
    if (argc > 3)
    {
        std::ifstream in3(argv[3], std::ios::binary);
        std::string html((std::istreambuf_iterator<char>(in3)), std::istreambuf_iterator<char>());

        std::vector<bosstimer::DailyItem> items;
        std::vector<bosstimer::DailyList> lists;
        std::string derr;
        Check(bosstimer::ParseDailyHtml(html, items, lists, &derr), "日常页面可解析");
        printf("      日常按钮：%d 个，列表：%d 个\n", (int)items.size(), (int)lists.size());
        if (!derr.empty()) printf("      解析错误：%s\n", derr.c_str());

        const char* wanted[5] = { "充能石英晶体", "契约团商人", "供给官商人", "伊莉莎", "购买古老召唤石" };
        for (int w = 0; w < 5; w++)
        {
            bool found = false;
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].name == wanted[w]) found = true;
            printf("      包含 %s：%s\n", wanted[w], found ? "是" : "否");
            Check(found, "物品采集包含要求的按钮");
        }

        for (size_t i = 0; i < items.size(); i++)
        {
            std::string text = bosstimer::ResolveDailyText(items[i],
                2026, 11, 6, 5, 12, 0); // 2026-11-06 周五 12:00
            printf("      %s -> %s\n", items[i].name.c_str(), text.c_str());
            Check(!text.empty(), "日常按钮有发送文本");
            Check(text.find("[&") != std::string::npos, "日常文本含点位");
            if (items[i].weekly)
                Check(text.find("2026-11-05") != std::string::npos, "契约团商人取到最近的周四日期");
        }

        Check(lists.size() == 2, "日常有 2 个列表");
        for (size_t i = 0; i < lists.size(); i++)
        {
            std::string oneLine = bosstimer::DailyListLine(lists[i]);
            printf("      列表 %s：%d 条 -> %s\n", lists[i].name.c_str(),
                (int)lists[i].lines.size(), oneLine.c_str());
            Check(!lists[i].lines.empty(), "日常列表非空");
            Check(lists[i].name == "日常大型地下城赏金" || lists[i].name == "碎层", "列表名称正确");
            Check(oneLine.find(lists[i].name) == 0, "整合行以列表名开头");
            Check(oneLine.size() < 199, "整合成一行后长度安全");
        }
    }

    printf(gFail == 0 ? "\nALL PASSED\n" : "\nFAILED (%d)\n", gFail);
    return gFail == 0 ? 0 : 1;
}
