/*
 * Claude 终端状态：显示当前有多少个 Claude Code 终端在运行，以及每个终端的状态。
 *
 * 本插件自己看不到 Claude Code 的进程内部状态，需要 Claude Code 通过 hooks
 * 主动上报——每次 SessionStart / UserPromptSubmit / PreToolUse / Notification /
 * Stop / StopFailure / SessionEnd 事件发生时，由 hook 命令把状态写到
 *   <CLAUDE_CONFIG_DIR 或 ~/.claude>\status\<session_id>.json
 * 格式形如 {"session_id":"...","status":"thinking","updated_at":1755500000,"cwd":"..."}
 * （具体的 hook 配置见 tools/claude-hook-status.ps1 和 README）。
 *
 * 五种状态，任务栏上用插件自己 GDI 画的彩色圆点表示（不是彩色 emoji 字符——GDI 画
 * 文字不认字体自带的调色板，emoji 会被画成黑白轮廓，颜色信息全丢，所以圆点得自己画；
 * 见 ClaudeUsagePlugin.cpp 里 StateDotColor/FillDot 的注释）。emoji 字符本模块仍然
 * 保留，只用在鼠标提示文字和"关掉自绘"时的兜底文本里：
 *   idle     🔘 灰 空闲中         会话已打开，还没有第一条用户输入（SessionStart）
 *   thinking 🔵 蓝 正在思考       已提交请求，Claude 正在处理（UserPromptSubmit）
 *   waiting  🟡 黄 等待用户命令   Claude 主动需要用户关注，例如权限确认（Notification）
 *   done     🟢 绿 已完成        一轮回复正常结束（Stop）
 *   error    🔴 红 出错/中断     一轮回复因 API 错误异常终止（StopFailure，携带 error_type，
 *                               例如 rate_limit / overloaded / authentication_failed）
 *
 * 之所以要专门监听 StopFailure：Stop 事件只在正常结束时触发，本身不带错误信息；
 * 真正的失败信号（限流、服务端错误、鉴权失败等）只出现在 StopFailure 里。
 *
 * 会话正常退出（SessionEnd）时 hook 会删掉对应的状态文件。但终端被直接叉掉/
 * 强杀进程时根本来不及跑 SessionEnd，不能只靠它清理——所以状态文件里还带着
 * 拥有这个终端的进程 PID，扫描时用 OpenProcess/GetExitCodeProcess 验证这个
 * 进程是否还活着：不在了就直接删文件，不等超时，终端一关图标马上消失。
 * 只有 pid 缺失（旧格式文件）时才退回 updated_at 超时剔除这条兜底路径——
 * 超过 stale_seconds 未更新的文件视为死会话，扫描时直接忽略（但不主动删）。
 *
 * 另外，Task/Agent 工具调用出去的子代理不是用户看到的终端窗口，hook 脚本
 * 那边看到 payload 带 agent_id/agent_type 就直接不写文件；但实测发现同一个
 * 终端进程还会因为别的内部子调用（不带 agent_id 的那种）产生额外的 session_id，
 * 光靠这个字段挡不住。所以 Scan() 里还会按 pid 兜底去重——同一个 pid（同一个
 * 终端进程）只保留一条，这样不管内部产生了多少个 session_id，任务栏上的圆点数
 * 永远等于实际打开的终端（进程）数。挑哪条留下不是简单比 updated_at：内部子
 * 调用的 cwd 观察到的都是空的，可能比真正的终端会话更晚落盘，只比时间会把
 * 真会话顶掉；所以优先选有 cwd 的那条，都有/都没有 cwd 时才比 updated_at
 * （见 IsBetterForPid）。
 */
#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace terminals {

enum class State
{
    Idle,
    Thinking,
    Waiting,
    Done,
    Error,
};

struct Entry
{
    std::wstring session_id;
    State state{ State::Idle };
    time_t updated_at{ 0 };
    std::wstring cwd;
    std::wstring error_type;   /**< 仅 state==Error 时可能有值，来自 StopFailure 的 error_type */
    unsigned long pid{ 0 };    /**< 拥有这个终端的进程 PID；0 表示旧格式文件，没有记录 */
};

/** 状态文件所在目录：<CLAUDE_CONFIG_DIR 或 ~/.claude>\status */
std::wstring GetStatusDir();

/**
 * 扫描状态目录，剔除超过 stale_seconds 未更新的文件。
 * 结果按 session_id 排序，保证图标顺序稳定，不会因为状态变化而重新排列。
 */
std::vector<Entry> Scan(time_t now, int stale_seconds);

/** 状态对应的显示 emoji */
const wchar_t* StateEmoji(State state);

/** 状态的本地化文字说明，用于鼠标提示 */
const wchar_t* StateLabel(State state, bool zh);

}   // namespace terminals
