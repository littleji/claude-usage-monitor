/*
 * 后台取数服务。
 *
 * TrafficMonitor 每秒都会调用 ITMPlugin::DataRequired，绝不能在那里发网络请求，
 * 否则会卡住主程序的界面线程。这里用一个独立线程按固定间隔拉取，
 * DataRequired 只读取内存中的快照。
 */
#pragma once

#include "UsageApi.h"

#ifndef NOMINMAX
#define NOMINMAX   // 否则 windows.h 的 min/max 宏会破坏 std::min/std::max
#endif
#include <windows.h>

#include <string>

namespace usage {

/** 主线程读取的数据快照 */
struct Snapshot
{
    bool has_data{ false };         /**< 是否至少成功取到过一次数据 */
    Data data;
    time_t updated_at{ 0 };         /**< 最近一次成功的本地时间 */

    Status last_status{ Status::NetworkError };
    std::wstring last_error;        /**< 最近一次失败的描述；成功时为空 */
    bool fetching{ false };         /**< 是否有请求正在进行 */
};

class Service
{
public:
    static Service& Instance();

    /** 从 <config_dir>\ClaudeUsage.ini 读取配置。可在 Start 之前调用。 */
    void LoadConfig(const std::wstring& config_dir);

    /** 启动后台线程（重复调用无副作用） */
    void Start();

    /** 停止后台线程并等待其退出 */
    void Stop();

    /** 立即触发一次刷新（不阻塞） */
    void RequestRefresh();

    /** 取当前快照 */
    Snapshot GetSnapshot() const;

    int GetRefreshInterval() const
    {
        return static_cast<int>(::InterlockedCompareExchange(&m_refresh_interval, 0, 0));
    }
    std::wstring GetConfigPath() const { return m_config_path; }

private:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    static DWORD WINAPI ThreadProc(LPVOID param);
    void Run();
    /** 取一次数；返回服务端建议的等待秒数（0 表示未提供） */
    int FetchOnce();

    mutable CRITICAL_SECTION m_lock{};
    Snapshot m_snapshot;

    HANDLE m_thread{ nullptr };
    HANDLE m_stop_event{ nullptr };
    HANDLE m_refresh_event{ nullptr };
    LONG m_started{ 0 };

    // LoadConfig 在界面线程调用，取数线程会读它，因此用 Interlocked 访问。
    mutable volatile LONG m_refresh_interval{ 60 };   /**< 成功后的刷新间隔（秒） */
    std::wstring m_config_path;
};

}   // namespace usage
