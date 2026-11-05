#pragma once

// 在 explorer 的任务栏线程里安装/卸载“把系统按钮钉在左侧”的那套挂钩。
// 这些函数必须在任务栏所在线程上调用（消息钩子第一次命中时）。

bool InstallTaskbarSplit();           //尝试挂钩 + 自检；成功返回 true
void RemoveTaskbarSplit();            //还原写进 explorer 的字节并清理状态
void RestoreDetourBytes();            //只还原代码字节（模块被卸载前必须调用）
void RestoreMarginsAsync();           //把我们加过的负边距还原（投递到 XAML 线程，不等结果）
void SetSplitEnabled(bool enabled);   //开关：Arrange 钩子里读的就是这个值

bool IsArrangeHookInstalled();        //跳转字节已经装上
bool IsTaskbarSplitWorking();         //已经真的在 Arrange 里看到过任务栏按钮
