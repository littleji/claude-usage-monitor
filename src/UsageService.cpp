#include "UsageService.h"

#include <algorithm>

namespace usage {
namespace {

// 用量接口限流较紧，而任务栏上的倒计时是本地根据 resets_at 推算的、
// 每秒都会刷新，并不依赖请求频率。所以默认 5 分钟拉一次就足够，
// 百分比的滞后最多 5 分钟，却能大幅降低撞上 HTTP 429 的概率。
const int kMinRefreshInterval = 60;
const int kMaxRefreshInterval = 3600;

// 失败后的退避：第一次等 30 秒，逐次翻倍，最多 10 分钟。
const int kFirstRetryDelay = 30;
const int kMaxRetryDelay = 600;

/** 读取上次请求的时间戳（可能来自上一个进程）；没有记录时返回 0 */
time_t ReadLastFetchAt(const std::wstring& config_path)
{
    wchar_t buffer[32]{};
    const DWORD length = ::GetPrivateProfileStringW(L"general", L"last_fetch_at", L"0",
                                                     buffer, ARRAYSIZE(buffer), config_path.c_str());
    if (length == 0)
        return 0;
    return static_cast<time_t>(::_wtoi64(buffer));
}

void WriteLastFetchAt(const std::wstring& config_path, time_t value)
{
    wchar_t buffer[32]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%lld", static_cast<long long>(value));
    ::WritePrivateProfileStringW(L"general", L"last_fetch_at", buffer, config_path.c_str());
}

}   // namespace

Service::Service()
{
    ::InitializeCriticalSection(&m_lock);
    m_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_refresh_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Service::~Service()
{
    Stop();
    if (m_stop_event != nullptr)
        ::CloseHandle(m_stop_event);
    if (m_refresh_event != nullptr)
        ::CloseHandle(m_refresh_event);
    ::DeleteCriticalSection(&m_lock);
}

Service& Service::Instance()
{
    static Service instance;
    return instance;
}

void Service::LoadConfig(const std::wstring& config_dir)
{
    if (config_dir.empty())
        return;

    std::wstring path = config_dir;
    if (path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    path += L"ClaudeUsage.ini";
    m_config_path = path;

    const int interval = ::GetPrivateProfileIntW(L"general", L"refresh_interval",
                                                 GetRefreshInterval(), path.c_str());
    const int clamped = std::max(kMinRefreshInterval, std::min(kMaxRefreshInterval, interval));
    ::InterlockedExchange(&m_refresh_interval, clamped);

    // 把生效的配置写回，方便用户看到有哪些可调项
    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%d", clamped);
    ::WritePrivateProfileStringW(L"general", L"refresh_interval", buffer, path.c_str());
}

void Service::Start()
{
    if (::InterlockedCompareExchange(&m_started, 1, 0) != 0)
        return;   // 已经启动过
    if (m_stop_event == nullptr || m_refresh_event == nullptr)
        return;

    m_thread = ::CreateThread(nullptr, 0, &Service::ThreadProc, this, 0, nullptr);
    if (m_thread == nullptr)
        ::InterlockedExchange(&m_started, 0);
}

void Service::Stop()
{
    if (m_thread == nullptr)
        return;
    ::SetEvent(m_stop_event);
    ::WaitForSingleObject(m_thread, 10000);
    ::CloseHandle(m_thread);
    m_thread = nullptr;
    ::InterlockedExchange(&m_started, 0);
}

void Service::RequestRefresh()
{
    if (m_refresh_event != nullptr)
        ::SetEvent(m_refresh_event);
}

Snapshot Service::GetSnapshot() const
{
    ::EnterCriticalSection(&m_lock);
    Snapshot copy = m_snapshot;
    ::LeaveCriticalSection(&m_lock);
    return copy;
}

DWORD WINAPI Service::ThreadProc(LPVOID param)
{
    static_cast<Service*>(param)->Run();
    return 0;
}

int Service::FetchOnce()
{
    ::EnterCriticalSection(&m_lock);
    m_snapshot.fetching = true;
    ::LeaveCriticalSection(&m_lock);

    Result result = Fetch();

    ::EnterCriticalSection(&m_lock);
    m_snapshot.fetching = false;
    m_snapshot.last_status = result.status;
    if (result.status == Status::Ok)
    {
        m_snapshot.data = result.data;
        m_snapshot.has_data = true;
        m_snapshot.updated_at = ::time(nullptr);
        m_snapshot.last_error.clear();
    }
    else
    {
        // 保留上一次成功的数据继续显示，只在提示里说明当前取数失败
        m_snapshot.last_error = result.message;
    }
    ::LeaveCriticalSection(&m_lock);
    return result.retry_after;
}

void Service::Run()
{
    HANDLE wait_handles[2] = { m_stop_event, m_refresh_event };
    int retry_delay = kFirstRetryDelay;

    // 上一次请求可能发生在上一个进程里——频繁重启 TrafficMonitor，或者调试时反复跑
    // HostTest/Probe，都会让新进程一启动就立刻再发一次请求，几秒内就能连续撞上
    // HTTP 429。这里只在启动时补一次这个等待，把请求间隔的下限（kMinRefreshInterval）
    // 也套用到"进程刚启动"这个时刻上；跑起来之后的节奏仍由下面的 interval/退避控制。
    if (!m_config_path.empty())
    {
        const time_t last_fetch_at = ReadLastFetchAt(m_config_path);
        if (last_fetch_at > 0)
        {
            const time_t now = ::time(nullptr);
            const int elapsed = static_cast<int>(std::max<time_t>(0, now - last_fetch_at));
            const int wait_needed = kMinRefreshInterval - elapsed;
            if (wait_needed > 0)
            {
                const DWORD waited = ::WaitForSingleObject(m_stop_event,
                                                            static_cast<DWORD>(wait_needed) * 1000);
                if (waited == WAIT_OBJECT_0)
                    return;   // 等待期间收到停止信号，直接退出
            }
        }
    }

    for (;;)
    {
        const int retry_after = FetchOnce();
        if (!m_config_path.empty())
            WriteLastFetchAt(m_config_path, ::time(nullptr));

        const int interval = GetRefreshInterval();
        int delay_seconds = interval;
        {
            ::EnterCriticalSection(&m_lock);
            const bool failed = (m_snapshot.last_status != Status::Ok);
            ::LeaveCriticalSection(&m_lock);

            if (failed)
            {
                // 服务端明确给了 Retry-After 就照做，否则用指数退避
                delay_seconds = (retry_after > 0)
                                    ? retry_after
                                    : std::min(retry_delay, interval);
                retry_delay = std::min(retry_delay * 2, kMaxRetryDelay);
            }
            else
            {
                retry_delay = kFirstRetryDelay;
            }
        }

        const DWORD waited = ::WaitForMultipleObjects(2, wait_handles, FALSE,
                                                      static_cast<DWORD>(delay_seconds) * 1000);
        if (waited == WAIT_OBJECT_0)
            break;   // 收到停止信号
        // WAIT_OBJECT_0 + 1（手动刷新）和 WAIT_TIMEOUT 都直接进入下一轮取数
    }
}

}   // namespace usage
