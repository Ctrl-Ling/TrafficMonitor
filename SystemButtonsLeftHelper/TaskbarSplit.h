#pragma once

// 在 explorer 的任务栏线程里安装/卸载“任务栏拆分布局（方案 B）”的那套挂钩。
// B 的做法：插件把 Windows 的对齐值写成靠左（TaskbarAl=0），系统按钮由系统自己钉在最左边；
// 助手只负责两件事——量出左边按钮块的右边界（给 TM 任务栏窗口摆位），以及把应用图标那一组整组居中。
// 这些函数必须在任务栏所在线程上调用（消息钩子第一次命中时）。

bool InstallTaskbarSplit();           //尝试挂钩 + 自检；成功返回 true
void RemoveTaskbarSplit();            //还原写进 explorer 的字节并清理状态
void RestoreDetourBytes();            //只还原代码字节（模块被卸载前必须调用）
void RestoreMarginsAsync();           //把我们加过的负边距还原（投递到 XAML 线程，不等结果）
void SetSplitEnabled(bool enabled);   //开关：Arrange 钩子里读的就是这个值

bool IsArrangeHookInstalled();        //跳转字节已经装上
bool IsTaskbarSplitWorking();         //已经真的在 Arrange 里看到过任务栏按钮
