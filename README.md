# GW2 BOSS 计时器（激战2 Nexus 插件）

在激战2游戏内显示 BOSS 时刻表面板，按倒计时自动排序，点一下就把一行 BOSS 信息发到聊天频道。

作者：摸鱼哥

## 功能

- 自动读取[星岬岛 BOSS 计时器](https://gw2.wishingstarmoye.com/index.php/gw2timerbox)的时刻表（24 个世界 BOSS），每 30 分钟抓取一次
- 同时读取[星岬岛事件时间表](https://gw2.wishingstarmoye.com/gw2timer)（45 个区域/事件，阶段时间轴），与 BOSS 合并成一个列表
- 面板顶部提供「全部 / BOSS / 事件 / 日常采集」过滤按钮
- **日常采集**页：读取[星岬岛日常](https://gw2.wishingstarmoye.com/daily)的「物品采集」，生成充能石英晶体、契约团商人、供给官商人、伊莉莎、购买古老召唤石等传送按钮（契约团商人按周自动取当前点位）
- 日常采集页另含「日常大型地下城赏金」「碎层」两个按钮，点击后按行依次发送当天列表
- **右键「事件」按钮**打开区域筛选：可按资料片整组勾选，也可逐条勾选；默认全部显示，可一键全选 / 全不选
- 面板每个条目一个按钮，**进行中的 BOSS 排最上面**，其余按「下次开始」的倒计时从近到远，每秒自动重排
- 点击按钮发送一行文本，例如：
  `淑雯  上次: 21:00下次: 23:00  巨龙尽头 [&BKIMAAA=] `
  事件条目同样格式：`世界BOSS  上次: 20:00下次: 20:15  斯瓦尼亚萨满 [&BMIDAAA=] `
- 左键按默认频道发送；**右键**弹出频道菜单：当前频道 / 地图 `/m` / 团队 `/d` / 队伍 `/p` / 战队 `/t` / 说 `/s` / 当前公会 `/g` / 公会1-6 `/g1`–`/g6`，也可以输入自定义频道
- BOSS 状态与页面一致：开始前 15 分钟显示「即将 X 分」，开始后 15 分钟内显示「进行中」
- 外观可调：面板透明度、按钮透明度、按钮颜色、文字颜色、按钮宽高
- 锁定位置后，鼠标在空白处可穿透操作游戏；可隐藏标题栏；选人等非游戏场景自动隐藏
- 显示/隐藏面板快捷键默认 `F10`（Nexus → 输入绑定 里可改）
- 抓取失败时使用上次缓存，首次运行 BOSS 使用内置数据

## 安装

前提：游戏已安装 [Nexus](https://github.com/RaidcoreGG) 插件管理工具（游戏根目录有 `addonLoader.dll`），否则 GW2 不会加载 `addons` 里的 DLL。

1. 关闭游戏
2. 双击 `release/BOSS计时器安装-双击运行-需要游戏安装Nexus插件管理工具.bat`
3. 选 `1`（安装 / 更新）
4. 填游戏目录（放 `Gw2-64.exe` 的文件夹，例如 `E:\Game\GuildWars2`）

命令行方式：

```powershell
powershell -ExecutionPolicy Bypass -File release\BOSS计时器安装.ps1 -Action 安装 -GameDir "E:\Game\GuildWars2"
powershell -ExecutionPolicy Bypass -File release\BOSS计时器安装.ps1 -Action 状态 -GameDir "E:\Game\GuildWars2"
powershell -ExecutionPolicy Bypass -File release\BOSS计时器安装.ps1 -Action 回退 -GameDir "E:\Game\GuildWars2"
```

- 安装只放 `addons\BossTimer.dll`，不动已有配置；旧版本会备份到脚本旁的 `backups\`
- 加 `-DryRun` 只预演，不改文件
- 卸载默认保留配置，要一起删加 `-PurgeConfig`
- 也可以手动把 `release/addons/BossTimer.dll` 复制到游戏的 `addons` 目录

## 使用

进入游戏后面板默认显示。

- 左键 BOSS 按钮：按默认频道发送
- 右键 BOSS 按钮：选择本次发送的频道
- 顶部「全部 / BOSS / 事件 / 日常采集」：过滤 / 切换页面
- 日常采集页：物品采集按点位发送；「日常大型地下城赏金」「碎层」会逐行发送当天列表
- 鼠标悬停：预览将要发送的完整文本
- `F10`：显示 / 隐藏面板
- 按住面板标题栏可拖动位置

设置入口：Nexus → 设置 → BOSS计时器。

## 配置与缓存

- 配置：`addons\BossTimer\BossTimer.ini`
- BOSS 数据缓存：`addons\BossTimer\bossdata_cache.js`
- 事件数据缓存：`addons\BossTimer\eventdata_cache.js`
- 日常页面缓存：`addons\BossTimer\dailydata_cache.html`

数据源为固定时刻表，不含实时服务器状态；「上次/下次」和「进行中」都在本地按页面规则计算。事件表按页面规则以 00:00 UTC 为起点、每 2 小时对齐，个别条目（约科进攻、迷雾武器商人）按当天星期/日期动态生成。

## 构建

需要 Visual Studio 2022 Build Tools（MSVC v143）。

```bat
source\build.cmd
```

离线自测（解析、跨零点、排序、文本格式）：

```bat
source\test\run_test.cmd
```

## 说明

- 国服聊天只能通过 Nexus 消息投递 + GBK 字节写入，UTF-8 或 IME 字节会触发游戏断言，因此插件固定使用 GBK 投递
- Nexus 的输入框无法使用中文输入法，自定义频道请填英文/数字（如 `d`、`g1`）

## License

MIT
