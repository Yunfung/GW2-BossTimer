# v1.0.0.1

## 变更
- 安装方式与快捷喊话一致：新增 PowerShell 安装脚本和双击运行入口
  - 安装 / 更新：复制 `addons\BossTimer.dll`，旧版本先备份到 `backups\`
  - 回退 / 卸载：删除 DLL，默认保留配置（`-PurgeConfig` 可一起删除）
  - 状态：检查 Nexus、插件与配置位置
  - 支持 `-DryRun` 预演；游戏占用 DLL 时会提示先关闭游戏
- 插件版本号更新为 1.0.0.1，功能与 v1.0.0 相同

## 安装
双击 `BOSS计时器安装-双击运行-需要游戏安装Nexus插件管理工具.bat`，选 `1`，填写游戏目录即可。

命令行：

```powershell
powershell -ExecutionPolicy Bypass -File BOSS计时器安装.ps1 -Action 安装 -GameDir "E:\Game\GuildWars2"
```
