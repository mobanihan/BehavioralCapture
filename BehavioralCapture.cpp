#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>

#pragma comment(lib, "psapi.lib")

// Event types
enum EventType {
    MOUSE_MOVE,
    MOUSE_LEFT_DOWN,
    MOUSE_LEFT_UP,
    MOUSE_RIGHT_DOWN,
    MOUSE_RIGHT_UP,
    MOUSE_WHEEL,
    KEY_DOWN,
    KEY_UP
};

// Structure to store event data
struct BehavioralEvent {
    long long timestamp;
    EventType type;
    int x, y;
    int keyCode;
    int wheelDelta;
    long long timeSinceLast;
    std::string activeApp;
    int backgroundAppCount;
    double mouseSpeed;  // pixels per second
};

// Buffered writer for performance optimization
class BufferedWriter {
private:
    std::ofstream file;
    std::vector<std::string> buffer;
    std::mutex bufferMutex;
    const size_t BUFFER_SIZE = 100;  // Flush every 100 events

public:
    BufferedWriter() {}

    bool open(const std::string& filename) {
        file.open(filename, std::ios::app);
        if (!file.is_open()) return false;

        file.seekp(0, std::ios::end);
        if (file.tellp() == 0) {
            file << "timestamp,event_type,x,y,key_code,wheel_delta,time_since_last,"
                << "active_app,background_apps,mouse_speed_pxps" << std::endl;
        }
        return true;
    }

    void write(const std::string& data) {
        std::lock_guard<std::mutex> lock(bufferMutex);
        buffer.push_back(data);

        if (buffer.size() >= BUFFER_SIZE) {
            flush();
        }
    }

    void flush() {
        if (file.is_open() && !buffer.empty()) {
            for (const auto& line : buffer) {
                file << line << std::endl;
            }
            buffer.clear();
        }
    }

    void close() {
        flush();
        if (file.is_open()) {
            file.close();
        }
    }
};

class BehavioralCapture {
private:
    std::vector<BehavioralEvent> events;
    HHOOK mouseHook;
    HHOOK keyboardHook;
    BufferedWriter dataWriter;
    long long lastEventTime;
    POINT lastMousePos;
    long long lastMouseMoveTime;

    // Cached context info to reduce system calls
    std::string cachedActiveApp;
    int cachedBackgroundCount;
    std::chrono::steady_clock::time_point lastContextUpdate;
    const int CONTEXT_UPDATE_INTERVAL_MS = 500;  // Update context every 500ms
    std::mutex contextMutex;

    // Sampling for mouse movements (reduce overhead)
    int mouseMoveCounter;
    const int MOUSE_SAMPLE_RATE = 3;  // Sample every 3rd movement

    // Background thread for context updates
    std::atomic<bool> contextThreadRunning;
    std::thread contextThread;

    static BehavioralCapture* instance;

    static long long getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

    // Get active window application name
    std::string getActiveApplicationName() {
        HWND hwnd = GetForegroundWindow();
        if (hwnd == NULL) return "Unknown";

        DWORD processId;
        GetWindowThreadProcessId(hwnd, &processId);

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (hProcess == NULL) return "Unknown";

        char processName[MAX_PATH];
        if (GetModuleBaseNameA(hProcess, NULL, processName, MAX_PATH) == 0) {
            CloseHandle(hProcess);
            return "Unknown";
        }

        CloseHandle(hProcess);
        return std::string(processName);
    }

    // Count running processes (background applications)
    int countBackgroundProcesses() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32 processEntry;
        processEntry.dwSize = sizeof(PROCESSENTRY32);

        int count = 0;
        if (Process32First(snapshot, &processEntry)) {
            do {
                count++;
            } while (Process32Next(snapshot, &processEntry));
        }

        CloseHandle(snapshot);
        return count > 0 ? count - 1 : 0;  // Exclude current process
    }

    // Calculate mouse speed in pixels per second
    double calculateMouseSpeed(int x1, int y1, int x2, int y2, long long timeDelta) {
        if (timeDelta == 0) return 0.0;

        double distance = std::sqrt(std::pow(x2 - x1, 2) + std::pow(y2 - y1, 2));
        double timeInSeconds = timeDelta / 1000.0;
        return distance / timeInSeconds;
    }

    // Background thread to update context information periodically
    void contextUpdateThread() {
        while (contextThreadRunning) {
            {
                std::lock_guard<std::mutex> lock(contextMutex);
                cachedActiveApp = getActiveApplicationName();
                cachedBackgroundCount = countBackgroundProcesses();
                lastContextUpdate = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(CONTEXT_UPDATE_INTERVAL_MS));
        }
    }

    // Get cached context info (thread-safe)
    void getCachedContext(std::string& activeApp, int& bgCount) {
        std::lock_guard<std::mutex> lock(contextMutex);
        activeApp = cachedActiveApp;
        bgCount = cachedBackgroundCount;
    }

    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance) {
            instance->processMouseEvent(wParam, lParam);
        }
        return CallNextHookEx(instance->mouseHook, nCode, wParam, lParam);
    }

    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance) {
            instance->processKeyboardEvent(wParam, lParam);
        }
        return CallNextHookEx(instance->keyboardHook, nCode, wParam, lParam);
    }

    void processMouseEvent(WPARAM wParam, LPARAM lParam) {
        MSLLHOOKSTRUCT* mouseStruct = (MSLLHOOKSTRUCT*)lParam;
        BehavioralEvent event;
        event.timestamp = getCurrentTimestamp();
        event.timeSinceLast = event.timestamp - lastEventTime;
        event.x = mouseStruct->pt.x;
        event.y = mouseStruct->pt.y;
        event.keyCode = 0;
        event.wheelDelta = 0;

        // Get cached context
        getCachedContext(event.activeApp, event.backgroundAppCount);

        switch (wParam) {
        case WM_MOUSEMOVE:
            // Sample mouse movements to reduce overhead
            mouseMoveCounter++;
            if (mouseMoveCounter % MOUSE_SAMPLE_RATE != 0) {
                return;  // Skip this movement
            }

            if (mouseStruct->pt.x != lastMousePos.x || mouseStruct->pt.y != lastMousePos.y) {
                event.type = MOUSE_MOVE;
                event.mouseSpeed = calculateMouseSpeed(
                    lastMousePos.x, lastMousePos.y,
                    mouseStruct->pt.x, mouseStruct->pt.y,
                    event.timestamp - lastMouseMoveTime
                );
                lastMousePos = mouseStruct->pt;
                lastMouseMoveTime = event.timestamp;
                addEvent(event);
            }
            break;

        case WM_LBUTTONDOWN:
            event.type = MOUSE_LEFT_DOWN;
            event.mouseSpeed = 0.0;
            addEvent(event);
            break;

        case WM_LBUTTONUP:
            event.type = MOUSE_LEFT_UP;
            event.mouseSpeed = 0.0;
            addEvent(event);
            break;

        case WM_RBUTTONDOWN:
            event.type = MOUSE_RIGHT_DOWN;
            event.mouseSpeed = 0.0;
            addEvent(event);
            break;

        case WM_RBUTTONUP:
            event.type = MOUSE_RIGHT_UP;
            event.mouseSpeed = 0.0;
            addEvent(event);
            break;

        case WM_MOUSEWHEEL:
            event.type = MOUSE_WHEEL;
            event.wheelDelta = GET_WHEEL_DELTA_WPARAM(mouseStruct->mouseData);
            event.mouseSpeed = 0.0;
            addEvent(event);
            break;
        }
    }

    void processKeyboardEvent(WPARAM wParam, LPARAM lParam) {
        KBDLLHOOKSTRUCT* keyStruct = (KBDLLHOOKSTRUCT*)lParam;
        BehavioralEvent event;
        event.timestamp = getCurrentTimestamp();
        event.timeSinceLast = event.timestamp - lastEventTime;
        event.x = 0;
        event.y = 0;
        event.keyCode = keyStruct->vkCode;
        event.wheelDelta = 0;
        event.mouseSpeed = 0.0;

        // Get cached context
        getCachedContext(event.activeApp, event.backgroundAppCount);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            event.type = KEY_DOWN;
            addEvent(event);
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            event.type = KEY_UP;
            addEvent(event);
        }
    }

    void addEvent(const BehavioralEvent& event) {
        // Store in memory (with limit)
        events.push_back(event);
        if (events.size() > 50000) {  // Keep last 50k events
            events.erase(events.begin(), events.begin() + 25000);
        }

        lastEventTime = event.timestamp;

        // Write to buffered file (non-blocking)
        std::ostringstream oss;
        oss << event.timestamp << ","
            << event.type << ","
            << event.x << ","
            << event.y << ","
            << event.keyCode << ","
            << event.wheelDelta << ","
            << event.timeSinceLast << ","
            << event.activeApp << ","
            << event.backgroundAppCount << ","
            << std::fixed << std::setprecision(2) << event.mouseSpeed;

        dataWriter.write(oss.str());
    }

public:
    BehavioralCapture() :
        mouseHook(NULL),
        keyboardHook(NULL),
        lastEventTime(0),
        lastMouseMoveTime(0),
        mouseMoveCounter(0),
        contextThreadRunning(false),
        cachedBackgroundCount(0) {
        instance = this;
        lastMousePos.x = 0;
        lastMousePos.y = 0;
    }

    ~BehavioralCapture() {
        stop();
    }

    bool start(const std::string& filename = "behavioral_data.csv") {
        if (!dataWriter.open(filename)) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return false;
        }

        // Start context update thread
        contextThreadRunning = true;
        contextThread = std::thread(&BehavioralCapture::contextUpdateThread, this);

        // Install hooks
        mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, NULL, 0);
        if (mouseHook == NULL) {
            std::cerr << "Failed to install mouse hook!" << std::endl;
            contextThreadRunning = false;
            contextThread.join();
            return false;
        }

        keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookProc, NULL, 0);
        if (keyboardHook == NULL) {
            std::cerr << "Failed to install keyboard hook!" << std::endl;
            UnhookWindowsHookEx(mouseHook);
            contextThreadRunning = false;
            contextThread.join();
            return false;
        }

        lastEventTime = getCurrentTimestamp();
        lastMouseMoveTime = lastEventTime;

        std::cout << "Behavioral capture started (optimized mode)." << std::endl;
        std::cout << "- Mouse movement sampling: 1/" << MOUSE_SAMPLE_RATE << std::endl;
        std::cout << "- Context update interval: " << CONTEXT_UPDATE_INTERVAL_MS << "ms" << std::endl;
        std::cout << "- Buffered writing enabled" << std::endl;
        std::cout << "Data will be saved to: " << filename << std::endl;

        return true;
    }

    void stop() {
        if (mouseHook) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = NULL;
        }
        if (keyboardHook) {
            UnhookWindowsHookEx(keyboardHook);
            keyboardHook = NULL;
        }

        // Stop context thread
        if (contextThreadRunning) {
            contextThreadRunning = false;
            if (contextThread.joinable()) {
                contextThread.join();
            }
        }

        // Flush remaining data
        dataWriter.flush();
        dataWriter.close();

        std::cout << "Behavioral capture stopped." << std::endl;
    }

    void printStatistics() {
        std::cout << "\n=== Capture Statistics ===" << std::endl;
        std::cout << "Total events captured: " << events.size() << std::endl;

        int mouseMoves = 0, mouseClicks = 0, keyPresses = 0;
        double totalSpeed = 0.0;
        int speedCount = 0;

        for (const auto& event : events) {
            if (event.type == MOUSE_MOVE) {
                mouseMoves++;
                if (event.mouseSpeed > 0) {
                    totalSpeed += event.mouseSpeed;
                    speedCount++;
                }
            }
            else if (event.type == MOUSE_LEFT_DOWN || event.type == MOUSE_RIGHT_DOWN) {
                mouseClicks++;
            }
            else if (event.type == KEY_DOWN) {
                keyPresses++;
            }
        }

        std::cout << "Mouse movements: " << mouseMoves << std::endl;
        std::cout << "Mouse clicks: " << mouseClicks << std::endl;
        std::cout << "Key presses: " << keyPresses << std::endl;

        if (speedCount > 0) {
            std::cout << "Average mouse speed: " << std::fixed << std::setprecision(2)
                << (totalSpeed / speedCount) << " px/s" << std::endl;
        }

        if (!events.empty()) {
            std::cout << "Last active application: " << events.back().activeApp << std::endl;
            std::cout << "Background processes: " << events.back().backgroundAppCount << std::endl;
        }
    }

    const std::vector<BehavioralEvent>& getEvents() const {
        return events;
    }
};

BehavioralCapture* BehavioralCapture::instance = nullptr;

int main() {
    std::cout << "=== Optimized Behavioral Biometric Capture System ===" << std::endl;
    std::cout << "This program efficiently captures user behavior with minimal overhead." << std::endl;
    std::cout << "\nNew features:" << std::endl;
    std::cout << "  - Active application tracking" << std::endl;
    std::cout << "  - Background process counting" << std::endl;
    std::cout << "  - Mouse speed calculation" << std::endl;
    std::cout << "  - Optimized performance (buffering, sampling, threading)" << std::endl;
    std::cout << "\nPress 'Q' to quit and see statistics.\n" << std::endl;

    BehavioralCapture capture;

    if (!capture.start("user_behavior_data.csv")) {
        std::cerr << "Failed to start capture system!" << std::endl;
        return 1;
    }

    MSG msg;
    bool running = true;

    while (running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (GetAsyncKeyState('Q') & 0x8000) {
            std::cout << "\nQuitting..." << std::endl;
            running = false;
        }

        Sleep(10);
    }

    capture.stop();
    capture.printStatistics();

    std::cout << "\nData saved to: user_behavior_data.csv" << std::endl;
    std::cout << "Press Enter to exit...";
    std::cin.get();

    return 0;
}