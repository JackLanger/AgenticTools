#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <iostream>
#include <utility>
#include <vector>
#include <fstream>
#include <numeric>
#include <ranges>
#include <windows.h>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <queue>
#include <optional>
#include <algorithm>
#include <cctype>
#include <tlhelp32.h>
#include <io.h>
#include <cstdio>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef _DEBUG
#define DEBUG_LOG(msg) do { std::clog << "[DEBUG] " << msg << std::endl; } while (0)
#else
#define DEBUG_LOG(msg) do { } while (0)
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif


namespace input {
    static std::pair<INPUT, INPUT> key_up(const INPUT &key) {
        INPUT up = key;
        up.ki.dwFlags |= KEYEVENTF_KEYUP; // key up
        return std::make_pair(key, up);
    }

    static INPUT key_down(const WORD w) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = w;
        down.ki.dwFlags = KEYEVENTF_UNICODE; // key down (unicode)
        return down;
    }

    static std::pair<INPUT, INPUT> press(const WORD w) {
        return key_up(key_down(w));
    }

    static void write_text(const std::wstring &text) {
        DEBUG_LOG("write_text called with length=" << text.size());
        std::vector<INPUT> inputs;
        inputs.reserve(text.size() * 2);

        for (const auto ch: text) {
            auto [down, up] = press(ch);
            inputs.push_back(down);
            inputs.push_back(up);
        }

        const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        DEBUG_LOG("write_text injected events=" << sent << "/" << inputs.size());
    }
}



/**
* Abstraction layer for mouse interactions, allowing to move the mouse, click buttons, and perform other actions. The class uses the Windows API to simulate mouse events.
*/
namespace mouse {
    static constexpr DWORD click_duration = 30; // milliseconds
    static constexpr int move_steps = 10; // number of steps for smooth movement
    static constexpr DWORD move_sleep_duration = 10; // milliseconds to sleep between steps

    namespace {
        enum MouseButton {
            left = MOUSEEVENTF_LEFTDOWN, right = MOUSEEVENTF_RIGHTDOWN
        };
    }

    static DWORD down_flag(const MouseButton button) {
        return button == left ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    }

    static DWORD up_flag(const MouseButton button) {
        return button == left ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
    }

    /**
     * Button up event for the specified mouse button.
     * @param button
     */
    static void up(const MouseButton button) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = up_flag(button);
        const UINT sent = SendInput(1, &input, sizeof(INPUT));
        DEBUG_LOG("mouse::up sent=" << sent << " button=" << (button == left ? "left" : "right"));
    }

    /**
     * Button down event for the specified mouse button.
     * @param button
     */
    static void down(const MouseButton button) {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = down_flag(button);
        const UINT sent = SendInput(1, &in, sizeof(INPUT));
        DEBUG_LOG("mouse::down sent=" << sent << " button=" << (button == left ? "left" : "right"));
    }

    /**
     * Button click event for the specified mouse button. It simulates a button down followed by a button up event with a short delay in between.
     * @param button
     */
    static void click(const MouseButton button) {
        DEBUG_LOG("mouse::click start button=" << (button == left ? "left" : "right"));
        down(button);
        Sleep(click_duration);
        up(button);
        DEBUG_LOG("mouse::click done");
    }

    /**
     * Moves the mouse cursor to the specified screen coordinates. The function uses the Windows API to set the cursor position directly.
     * @param p
     */
    static void move_to(const POINT p) {
        DEBUG_LOG("mouse::move_to x=" << p.x << " y=" << p.y);
        SetCursorPos(p.x, -p.y);
    }

    /**
     * Moves the mouse cursor by the specified offset from its current position. The function retrieves the current cursor position, calculates the target position, and moves the cursor in steps for a smooth transition.
     * @param p
     */
    static void move_by(const POINT p) {
        POINT cur{};
        if (!GetCursorPos(&cur)) {
            DEBUG_LOG("mouse::move_by failed to get cursor position");
            return;
        }

        const POINT target{.x = cur.x + p.x, .y = cur.y - p.y};
        DEBUG_LOG("mouse::move_by dx=" << p.x << " dy=" << p.y << " target_x=" << target.x << " target_y=" << target.y);
        if (move_steps <= 1) {
            SetCursorPos(target.x, target.y);
            return;
        }

        for (int i = 1; i <= move_steps; ++i) {
            const int x = cur.x + (target.x - cur.x) * i / move_steps;
            const int y = cur.y + (target.y - cur.y) * i / move_steps;
            SetCursorPos(x, y);
            Sleep(move_sleep_duration);
        }
    }

    static void MouseScrollVertical(const long notches) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(notches * WHEEL_DELTA); // 120 per notch
        SendInput(1, &in, sizeof(INPUT));
    }

    static void MouseScrollHorizontal(const long notches) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = static_cast<DWORD>(notches * WHEEL_DELTA); // +right, -left
        SendInput(1, &in, sizeof(INPUT));
    }
};

static std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}


namespace Window {
    struct FindWindowByPidData {
        DWORD pid;
        HWND hwnd = nullptr;
    };

    class Window {
        HWND _hwnd;
        std::string name;

        static BOOL CALLBACK EnumWindowsProc(const HWND hwnd, const LPARAM lParam) {
            auto *data = reinterpret_cast<FindWindowByPidData *>(lParam);
            DWORD dwPID = 0;
            GetWindowThreadProcessId(hwnd, &dwPID);

            if (dwPID == data->pid) {
                DEBUG_LOG("Window::EnumWindowsProc found hwnd for pid=" << dwPID);
                if (IsIconic(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                if (!SetForegroundWindow(hwnd)) {
                    printf("Failed to set foreground window.\n");
                    DEBUG_LOG("Window::EnumWindowsProc SetForegroundWindow failed");
                }
                data->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        }

        static HWND HwndFromPid(const DWORD pid) {
            FindWindowByPidData data{.pid = pid};
            EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
            return data.hwnd;
        }

    public:
        Window(const int pid, std::string name) : _hwnd(HwndFromPid(static_cast<DWORD>(pid))),
                                                  name(std::move(name)) {
        }

        // nullptr hwnd targets the whole desktop
        Window(const HWND hwnd, std::string name) : _hwnd(hwnd), name(std::move(name)) {
        }

        [[nodiscard]] bool valid() const {
            return _hwnd != nullptr && IsWindow(_hwnd);
        }

        [[nodiscard]] DWORD process_id() const {
            DWORD pid = 0;
            if (_hwnd != nullptr) {
                GetWindowThreadProcessId(_hwnd, &pid);
            }
            return pid;
        }

        cv::Mat capture() const {
            HWND target = _hwnd;
            if (target == nullptr || !IsWindow(target)) {
                target = GetDesktopWindow();
            }

            DEBUG_LOG("Window::capture start hwnd=" << target << " name=" << name);
            const auto hwindowDC = GetDC(target);
            const auto hwindowCompatibleDC = CreateCompatibleDC(hwindowDC);

            const int width = GetSystemMetrics(SM_CXSCREEN);
            const int height = GetSystemMetrics(SM_CYSCREEN);

            const auto hb_window = CreateCompatibleBitmap(hwindowDC, width, height);
            SelectObject(hwindowCompatibleDC, hb_window);

            BitBlt(hwindowCompatibleDC, 0, 0, width, height, hwindowDC, 0, 0, SRCCOPY);

            cv::Mat src(height, width, CV_8UC4);
            BITMAPINFOHEADER bi = {0};
            bi.biSize = sizeof(BITMAPINFOHEADER);
            bi.biWidth = width;
            bi.biHeight = -height;
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;

            GetDIBits(hwindowCompatibleDC, hb_window, 0, height, src.data, reinterpret_cast<BITMAPINFO *>(&bi),
                      DIB_RGB_COLORS);

            DeleteObject(hb_window);
            DeleteDC(hwindowCompatibleDC);
            ReleaseDC(nullptr, hwindowDC);

            DEBUG_LOG("Window::capture done width=" << width << " height=" << height);
            return src;
        }

        void close() const {
            if (_hwnd == nullptr) {
                std::cerr << "Invalid PID. Cannot close window." << std::endl;
                DEBUG_LOG("Window::close aborted invalid pid");
                return;
            }
            DEBUG_LOG("Window::close pid=" << _hwnd << " name=" << name);
            // CloseWindow() only minimizes; WM_CLOSE asks the window to close
            PostMessage(_hwnd, WM_CLOSE, 0, 0);
        }

        void minimize() const {
            if (_hwnd == nullptr) {
                std::cerr << "Invalid PID. Cannot minimize window." << std::endl;
                DEBUG_LOG("Window::minimize aborted invalid pid");
                return;
            }
            DEBUG_LOG("Window::minimize pid=" << _hwnd << " name=" << name);
            ShowWindow(_hwnd, SW_MINIMIZE);
        }

        void focus() {
            if (_hwnd == nullptr) {
                std::cerr << "Invalid PID. Cannot focus window." << std::endl;
                DEBUG_LOG("Window::focus aborted invalid pid");
                return;
            }
            const auto hwnd = _hwnd;
            DEBUG_LOG("Window::focus pid=" << _hwnd << " name=" << name);
            SetForegroundWindow(hwnd);
        }
    };


    class LiveCapture {
        std::atomic<bool> running{false};
        std::thread worker;

    public:
        void start(Window &window, const std::string &outPath, int intervalMs = 100) {
            if (running.exchange(true)) return; // already running
            worker = std::thread([this, &window, outPath, intervalMs]() {
                while (running.load()) {
                    cv::Mat frame = window.capture();
                    std::vector<uchar> buf;
                    if (cv::imencode(".png", frame, buf)) {
                        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
                        out.write(reinterpret_cast<const char *>(buf.data()),
                                  static_cast<std::streamsize>(buf.size()));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                }
            });
        }

        void stop() {
            if (!running.exchange(false)) return;
            if (worker.joinable()) worker.join();
        }

        ~LiveCapture() { stop(); }
    };
}


namespace commands {
    namespace {
        mouse::MouseButton parse_button(const std::string &button) {
            if (button == "left") return mouse::left;
            if (button == "right") return mouse::right;
            throw std::invalid_argument("Unknown button: " + button + " (expected left|right)");
        }

        std::wstring utf8_to_wide(const std::string &s) {
            if (s.empty()) return {};
            const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
            return w;
        }

        bool is_number(const std::string &s) {
            return !s.empty() && std::ranges::all_of(s, [](const unsigned char c) { return std::isdigit(c); });
        }

        /**
         * Resolves a window identifier: a number is treated as a process ID, anything else as an exact window title.
         */
        Window::Window resolve_window(const std::string &id) {
            Window::Window window = is_number(id)
                                        ? Window::Window(std::stoi(id), id)
                                        : Window::Window(FindWindowA(nullptr, id.c_str()), id);
            if (!window.valid()) {
                throw std::invalid_argument("No window found for: " + id);
            }
            DEBUG_LOG("Opened window: id=" << id << " pid=" << window.process_id());
            return window;
        }

        void write_png(const cv::Mat &frame, const std::string &path) {
            std::vector<uchar> buf;
            if (!cv::imencode(".png", frame, buf)) {
                throw std::runtime_error("Failed to encode PNG");
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("Cannot open output file: " + path);
            }
            out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
        }
    }

    class Command {
    public:
        virtual ~Command() = default;

        [[nodiscard]] virtual std::string name() const = 0;

        [[nodiscard]] virtual std::string usage() const = 0;

        virtual void execute(const std::vector<std::string> &args) const = 0;
    };

    // ---- mouse ----

    class MoveCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "move";
        }

        [[nodiscard]] std::string usage() const override {
            return "move (to|by) <x> <y>      Move the cursor to a screen position or by an offset";
        }

        void execute(const std::vector<std::string> &args) const override {
#ifdef _DEBUG
            std::stringbuf sbuf;
            for (const auto &arg: args) {
                sbuf.sputc(' ');
                sbuf.sputn(arg.data(), static_cast<std::streamsize>(arg.size()));
            }
            DEBUG_LOG("move command received with args size=" << args.size() << " args=" << sbuf.str());
#endif

            if (args.size() < 3) {
                throw std::invalid_argument("move requires 3 arguments: (to|by) dx dy");
            }

            const int dx = std::stoi(args[1]);
            const int dy = std::stoi(args[2]);
            if (args[0] == "to") {
                mouse::move_to({.x = dx, .y = dy});
            } else if (args[0] == "by") {
                mouse::move_by({.x = dx, .y = dy});
            } else {
                throw std::invalid_argument("move requires first argument to be 'to' or 'by'");
            }
        }
    };

    class ClickCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "click";
        }

        [[nodiscard]] std::string usage() const override {
            return "click (left|right)        Click a mouse button at the current position";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("click requires 1 argument: (left|right)");
            }
            mouse::click(parse_button(args[0]));
        }
    };

    class MouseDownCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "down";
        }

        [[nodiscard]] std::string usage() const override {
            return "down (left|right)         Press and hold a mouse button";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("down requires 1 argument: (left|right)");
            }
            mouse::down(parse_button(args[0]));
        }
    };

    class MouseUpCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "up";
        }

        [[nodiscard]] std::string usage() const override {
            return "up (left|right)           Release a mouse button";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("up requires 1 argument: (left|right)");
            }
            mouse::up(parse_button(args[0]));
        }
    };

    class ScrollCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "scroll";
        }

        [[nodiscard]] std::string usage() const override {
            return "scroll <notches>          Scroll vertically (+up, -down)";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("scroll requires 1 argument: notches");
            }
            mouse::MouseScrollVertical(std::stol(args[0]));
        }
    };

    class HScrollCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "hscroll";
        }

        [[nodiscard]] std::string usage() const override {
            return "hscroll <notches>         Scroll horizontally (+right, -left)";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("hscroll requires 1 argument: notches");
            }
            mouse::MouseScrollHorizontal(std::stol(args[0]));
        }
    };

    // ---- input ----

    class WriteCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "write";
        }

        [[nodiscard]] std::string usage() const override {
            return "write <text...>           Type text (UTF-8) into the focused window";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("write requires text");
            }
            std::string text = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                text += ' ';
                text += args[i];
            }
            input::write_text(utf8_to_wide(text));
        }
    };

    // ---- window ----

    class CaptureCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "capture";
        }

        [[nodiscard]] std::string usage() const override {
            return "capture <out.png> [window] Save a screenshot; window = pid or title, default whole screen";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("capture requires 1 argument: <out.png> [window]");
            }
            const Window::Window window = args.size() > 1
                                              ? resolve_window(args[1])
                                              : Window::Window(static_cast<HWND>(nullptr), "desktop");
            write_png(window.capture(), args[0]);
        }
    };

    class CloseCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "close";
        }

        [[nodiscard]] std::string usage() const override {
            return "close <window>            Close a window (pid or title)";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("close requires 1 argument: window");
            }
            resolve_window(args[0]).close();
        }
    };

    class MinimizeCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "minimize";
        }

        [[nodiscard]] std::string usage() const override {
            return "minimize <window>         Minimize a window (pid or title)";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("minimize requires 1 argument: window");
            }
            resolve_window(args[0]).minimize();
        }
    };

    class FocusCommand final : public Command {
    public:
        [[nodiscard]] std::string name() const override {
            return "focus";
        }

        [[nodiscard]] std::string usage() const override {
            return "focus <window>            Bring a window to the foreground (pid or title)";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("focus requires 1 argument: window");
            }
            resolve_window(args[0]).focus();
        }
    };

    class LiveCaptureCommand final : public Command {
        // window must outlive the capture thread, so it is declared first (destroyed last)
        mutable std::optional<Window::Window> window;
        mutable Window::LiveCapture liveCapture;

    public:
        [[nodiscard]] std::string name() const override {
            return "livecapture";
        }

        [[nodiscard]] std::string usage() const override {
            return "livecapture start <out.png> [window] [intervalMs] | livecapture stop\n"
                   "                          Continuously overwrite a screenshot file";
        }

        void execute(const std::vector<std::string> &args) const override {
            if (args.empty()) {
                throw std::invalid_argument("livecapture requires (start|stop)");
            }

            if (args[0] == "stop") {
                liveCapture.stop();
                return;
            }

            if (args[0] != "start" || args.size() < 2) {
                throw std::invalid_argument("livecapture start <out.png> [window] [intervalMs]");
            }

            liveCapture.stop();
            if (args.size() > 2) {
                window.emplace(resolve_window(args[2]));
            } else {
                window.emplace(static_cast<HWND>(nullptr), "desktop");
            }
            const int intervalMs = args.size() > 3 ? std::stoi(args[3]) : 100;
            liveCapture.start(*window, args[1], intervalMs);
        }
    };
}


void clear( std::queue<std::function<void()>> &q )
{
   std::queue<std::function<void()>> empty;
   std::swap( q, empty );
}

DWORD GetParentPid() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe{ .dwSize = sizeof(pe) };
    DWORD parent = 0;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (pe.th32ProcessID == self) { parent = pe.th32ParentProcessID; break; }
    }
    CloseHandle(snap);
    return parent;
}

int main(const int argc, char *argv[]) {
    std::string command;

    std::vector<std::unique_ptr<commands::Command> > commands;
    commands.emplace_back(std::make_unique<commands::MoveCommand>());
    commands.emplace_back(std::make_unique<commands::ClickCommand>());
    commands.emplace_back(std::make_unique<commands::MouseDownCommand>());
    commands.emplace_back(std::make_unique<commands::MouseUpCommand>());
    commands.emplace_back(std::make_unique<commands::ScrollCommand>());
    commands.emplace_back(std::make_unique<commands::HScrollCommand>());
    commands.emplace_back(std::make_unique<commands::WriteCommand>());
    commands.emplace_back(std::make_unique<commands::CaptureCommand>());
    commands.emplace_back(std::make_unique<commands::CloseCommand>());
    commands.emplace_back(std::make_unique<commands::MinimizeCommand>());
    commands.emplace_back(std::make_unique<commands::FocusCommand>());
    commands.emplace_back(std::make_unique<commands::LiveCaptureCommand>());

    // if help requested print help
    if (argc > 1) {
        if (const std::string arg(argv[1]); arg == "--help" || arg == "-h" || arg == "?") {
            std::cout << "Usage: " << argv[0] << "   (reads one command per line from stdin)" << std::endl;
            std::cout << "Commands are queued and run when 'execute' is sent." << std::endl;
            std::cout << "Commands:" << std::endl;
            for (const auto &cmd: commands) {
                std::cout << "  " << cmd->usage() << std::endl;
            }
            std::cout << "  execute                   Run all queued commands, then refocus the terminal" << std::endl;
            std::cout << "  cancel, exit, quit        Exit the program" << std::endl;
            return 0;
        }
    }

    std::unordered_map<std::string, std::function<void(const std::vector<std::string> &)> > command_map;
    for (const auto &cmd: commands) {
        command_map.emplace(cmd->name(), [command = cmd.get()](const std::vector<std::string> &args) {
            command->execute(args);
        });
    }

    DEBUG_LOG("Command map initialized with " << command_map.size() << " commands");
#ifdef _DEBUG
    for (const auto &cmd: command_map) {
        DEBUG_LOG("Registered command: " << cmd.first);
    }
#endif



    Window::Window terminalWindow(static_cast<int>(GetParentPid()), "terminal");

    std::queue<std::function<void()>> commandQueue;

    EnumWindows([](const HWND hwnd, const LPARAM) -> BOOL {
        const int titleLength = GetWindowTextLengthA(hwnd);
        std::string title(static_cast<size_t>(titleLength) + 1, '\0');
        const int copied = GetWindowTextA(hwnd, title.data(), titleLength + 1);
        title.resize(static_cast<size_t>(copied));
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        DEBUG_LOG("Opened window hwnd=" << hwnd << " pid=" << pid << " title=" << title);
        return TRUE;
    }, 0);

    // welcome message and prompt only for humans; piped input (agents) stays quiet
    const bool interactive = _isatty(_fileno(stdin)) != 0;
    if (interactive) {
        std::cout << "WindowsInputBridge " << APP_VERSION << " ready.\n"
                  << "Commands are queued and run with 'execute'. Type 'exit' to quit.\n"
                  << "Start with --help for the list of commands." << std::endl;
    }

    //    command interpreter loop
    while (true) {
        try {
            if (interactive) {
                std::cout << ">> " << std::flush;
            }
            std::getline(std::cin, command);
            if (!std::cin) {
                if (interactive) {
                    std::cout << std::endl;
                }
                break;
            }

            if (command.empty()) {
                continue;
            }

            if (command == "cancel" || command == "exit" || command == "quit") {
                break;
            }

            if(command == "execute") {
                while (!commandQueue.empty()) {
                    auto cmd = commandQueue.front();
                    commandQueue.pop();
                    try {
                        cmd();
                    } catch (...) {
                        clear(commandQueue);
                        throw;
                    }
                }
                terminalWindow.focus();
                continue;
            }
            
            auto parts = split(command, ' ');

            if (parts.empty()) {
                continue;
            }

            DEBUG_LOG("parsing command: " << parts[0]);
            if (auto it = command_map.find(parts[0]); it != command_map.end()) {
                std::vector<std::string> tail(parts.begin() + 1, parts.end());

                commandQueue.emplace([func = it->second, tail = std::move(tail)]() {
                    func(tail);
                });

            } else {
                std::cerr << "Unknown command: " << command << std::endl;
            }
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
}
