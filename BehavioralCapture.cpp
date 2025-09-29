#include <windows.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

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
    long long timestamp;  // milliseconds since epoch
    EventType type;
    int x, y;            // mouse coordinates
    int keyCode;         // virtual key code for keyboard
    int wheelDelta;      // for mouse wheel
    long long timeSinceLast; // time since last event
};

class BehavioralCapture {
private:
    std::vector<BehavioralEvent> events;
    HHOOK mouseHook;
    HHOOK keyboardHook;
    std::ofstream dataFile;
    long long lastEventTime;
    POINT lastMousePos;

    static BehavioralCapture* instance;

    // Get current timestamp in milliseconds
    static long long getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

    // Mouse hook callback
    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance) {
            instance->processMouseEvent(wParam, lParam);
        }
        return CallNextHookEx(instance->mouseHook, nCode, wParam, lParam);
    }

    // Keyboard hook callback
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

        switch (wParam) {
        case WM_MOUSEMOVE:
            // Only record if mouse actually moved
            if (mouseStruct->pt.x != lastMousePos.x || mouseStruct->pt.y != lastMousePos.y) {
                event.type = MOUSE_MOVE;
                lastMousePos = mouseStruct->pt;
                addEvent(event);
            }
            break;
        case WM_LBUTTONDOWN:
            event.type = MOUSE_LEFT_DOWN;
            addEvent(event);
            break;
        case WM_LBUTTONUP:
            event.type = MOUSE_LEFT_UP;
            addEvent(event);
            break;
        case WM_RBUTTONDOWN:
            event.type = MOUSE_RIGHT_DOWN;
            addEvent(event);
            break;
        case WM_RBUTTONUP:
            event.type = MOUSE_RIGHT_UP;
            addEvent(event);
            break;
        case WM_MOUSEWHEEL:
            event.type = MOUSE_WHEEL;
            event.wheelDelta = GET_WHEEL_DELTA_WPARAM(mouseStruct->mouseData);
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
        events.push_back(event);
        lastEventTime = event.timestamp;

        // Write to file immediately for real-time analysis
        writeEventToFile(event);

        // Optional: limit memory usage
        if (events.size() > 100000) {
            events.erase(events.begin(), events.begin() + 50000);
        }
    }

    void writeEventToFile(const BehavioralEvent& event) {
        if (dataFile.is_open()) {
            dataFile << event.timestamp << ","
                << event.type << ","
                << event.x << ","
                << event.y << ","
                << event.keyCode << ","
                << event.wheelDelta << ","
                << event.timeSinceLast << std::endl;
        }
    }

    std::string getEventTypeName(EventType type) {
        switch (type) {
        case MOUSE_MOVE: return "MOUSE_MOVE";
        case MOUSE_LEFT_DOWN: return "MOUSE_LEFT_DOWN";
        case MOUSE_LEFT_UP: return "MOUSE_LEFT_UP";
        case MOUSE_RIGHT_DOWN: return "MOUSE_RIGHT_DOWN";
        case MOUSE_RIGHT_UP: return "MOUSE_RIGHT_UP";
        case MOUSE_WHEEL: return "MOUSE_WHEEL";
        case KEY_DOWN: return "KEY_DOWN";
        case KEY_UP: return "KEY_UP";
        default: return "UNKNOWN";
        }
    }

public:
    BehavioralCapture() : mouseHook(NULL), keyboardHook(NULL), lastEventTime(0) {
        instance = this;
        lastMousePos.x = 0;
        lastMousePos.y = 0;
    }

    ~BehavioralCapture() {
        stop();
        if (dataFile.is_open()) {
            dataFile.close();
        }
    }

    bool start(const std::string& filename = "behavioral_data.csv") {
        // Open file for writing
        dataFile.open(filename, std::ios::app);
        if (!dataFile.is_open()) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return false;
        }

        // Write header if file is empty
        dataFile.seekp(0, std::ios::end);
        if (dataFile.tellp() == 0) {
            dataFile << "timestamp,event_type,x,y,key_code,wheel_delta,time_since_last" << std::endl;
        }

        // Install mouse hook
        mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, NULL, 0);
        if (mouseHook == NULL) {
            std::cerr << "Failed to install mouse hook!" << std::endl;
            return false;
        }

        // Install keyboard hook
        keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookProc, NULL, 0);
        if (keyboardHook == NULL) {
            std::cerr << "Failed to install keyboard hook!" << std::endl;
            UnhookWindowsHookEx(mouseHook);
            return false;
        }

        lastEventTime = getCurrentTimestamp();
        std::cout << "Behavioral capture started. Data will be saved to: " << filename << std::endl;
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
        if (dataFile.is_open()) {
            dataFile.close();
        }
        std::cout << "Behavioral capture stopped." << std::endl;
    }

    void printStatistics() {
        std::cout << "\n=== Capture Statistics ===" << std::endl;
        std::cout << "Total events captured: " << events.size() << std::endl;

        // Count event types
        int mouseMoves = 0, mouseClicks = 0, keyPresses = 0;
        for (const auto& event : events) {
            if (event.type == MOUSE_MOVE) mouseMoves++;
            else if (event.type == MOUSE_LEFT_DOWN || event.type == MOUSE_RIGHT_DOWN) mouseClicks++;
            else if (event.type == KEY_DOWN) keyPresses++;
        }

        std::cout << "Mouse movements: " << mouseMoves << std::endl;
        std::cout << "Mouse clicks: " << mouseClicks << std::endl;
        std::cout << "Key presses: " << keyPresses << std::endl;
    }

    const std::vector<BehavioralEvent>& getEvents() const {
        return events;
    }
};

BehavioralCapture* BehavioralCapture::instance = nullptr;

int main() {
    std::cout << "=== Behavioral Biometric Capture System ===" << std::endl;
    std::cout << "This program captures mouse and keyboard behavior for user authentication." << std::endl;
    std::cout << "\nPress 'Q' to quit and see statistics.\n" << std::endl;

    BehavioralCapture capture;

    if (!capture.start("user_behavior_data.csv")) {
        std::cerr << "Failed to start capture system!" << std::endl;
        return 1;
    }

    // Message loop
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

        // Check for 'Q' key to quit
        if (GetAsyncKeyState('Q') & 0x8000) {
            std::cout << "\nQuitting..." << std::endl;
            running = false;
        }

        Sleep(10);  // Reduce CPU usage
    }

    capture.stop();
    capture.printStatistics();

    std::cout << "\nData saved to: user_behavior_data.csv" << std::endl;
    std::cout << "Press Enter to exit...";
    std::cin.get();

    return 0;
}