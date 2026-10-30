#include "sifa/desktop.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>

#include "sifa/android_runtime.hpp"
#include "sifa/game_input.hpp"
#include "sifa/server.hpp"
#include "sifa/settings.hpp"
#include "third_party/json.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sifa {
namespace {

constexpr UINT work_completed = WM_APP + 25;
constexpr UINT_PTR poll_timer = 1;
constexpr wchar_t window_class[] = L"SifaAndroidDesktop";

enum Control : int {
    sdk_path = 101, browse_sdk, refresh_devices, device_list, cpu_list,
    memory_list, gpu_list, start_android, stop_android, open_play,
    game_page, start_game, install_sdk, log_output, input_settings, resolution_list,
};

enum class Operation { inspect, start, poll, stop, play, game_store, game };

struct Resolution {
    unsigned width;
    unsigned height;
    const wchar_t* label;
};

constexpr std::array<Resolution, 4> resolutions{{
    {1280, 720, L"1280 × 720 (минимум)"},
    {1600, 900, L"1600 × 900"},
    {1920, 1080, L"1920 × 1080"},
    {0, 0, L"По настройкам AVD"},
}};

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"Не удалось прочитать сообщение UTF-8";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring text_of(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::wstring status_text(const android::Status& status) {
    if (!status.running) return L"Android остановлен";
    if (!status.boot_completed) return L"Android загружается…";
    return L"Android работает";
}

struct Result {
    Operation operation = Operation::inspect;
    bool ok = false;
    android::Sdk sdk;
    std::vector<android::Avd> devices;
    android::Status status;
    std::string error;
};

class Desktop {
public:
    Desktop() : data_directory_(default_data_dir()),
        runtime_(data_directory_ / "android-logs"), host_(detect_host()) {
        HDC screen = GetDC(nullptr);
        dpi_ = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
        if (screen) ReleaseDC(nullptr, screen);
        load_preferences();
    }

    ~Desktop() {
        if (worker_.joinable()) worker_.join();
        game_input_.reset();
        if (font_) DeleteObject(font_);
        if (title_font_) DeleteObject(title_font_);
        if (bold_font_) DeleteObject(bold_font_);
        if (background_) DeleteObject(background_);
    }

    int run(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.hInstance = instance;
        type.lpfnWndProc = window_proc;
        type.lpszClassName = window_class;
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        type.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        background_ = CreateSolidBrush(RGB(246, 248, 250));
        type.hbrBackground = background_;
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return 1;

        RECT area{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        const int width = std::min(px(1000), static_cast<int>(area.right - area.left));
        const int height = std::min(px(790), static_cast<int>(area.bottom - area.top));
        window_ = CreateWindowExW(0, window_class, L"Sifa — Android для Windows",
            WS_OVERLAPPEDWINDOW, area.left + (area.right - area.left - width) / 2,
            area.top + (area.bottom - area.top - height) / 2, width, height,
            nullptr, nullptr, instance, this);
        if (!window_) return 1;
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        begin(Operation::inspect);
        SetTimer(window_, poll_timer, 3000, nullptr);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<int>(message.wParam);
    }

private:
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT bold_font_ = nullptr;
    HBRUSH background_ = nullptr;
    int dpi_ = 96;
    std::filesystem::path data_directory_;
    android::Runtime runtime_;
    std::unique_ptr<GameInput> game_input_;
    HostInfo host_;
    android::LaunchOptions saved_;
    android::Sdk sdk_;
    android::Status status_;
    std::vector<android::Avd> devices_;
    std::vector<unsigned> cpu_values_;
    std::vector<unsigned> memory_values_;
    std::thread worker_;
    std::mutex result_mutex_;
    std::unique_ptr<Result> result_;
    bool busy_ = false;
    bool closing_ = false;
    bool close_stop_attempted_ = false;
    bool setting_sdk_path_ = false;
    std::wstring last_report_;
    std::wstring activity_;

    HWND heading_ = nullptr, subtitle_ = nullptr, sdk_label_ = nullptr;
    HWND sdk_hint_ = nullptr, device_label_ = nullptr;
    HWND cpu_label_ = nullptr, memory_label_ = nullptr, gpu_label_ = nullptr;
    HWND resolution_label_ = nullptr;
    HWND status_heading_ = nullptr, status_detail_ = nullptr;
    HWND log_label_ = nullptr, limitation_ = nullptr;

    int px(int value) const { return MulDiv(value, dpi_, 96); }
    HWND control(int id) const { return GetDlgItem(window_, id); }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Desktop* app = reinterpret_cast<Desktop*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<Desktop*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) return app->message(message, wparam, lparam);
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HWND make(const wchar_t* type, const wchar_t* title, DWORD style, int id = 0) {
        const DWORD extended = std::wstring(type) == L"EDIT" ? WS_EX_CLIENTEDGE : 0;
        HWND result = CreateWindowExW(extended, type, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return result;
    }

    void create_controls() {
        font_ = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        title_font_ = CreateFontW(-px(30), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        bold_font_ = CreateFontW(-px(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        heading_ = make(L"STATIC", L"Sifa", SS_LEFT);
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        subtitle_ = make(L"STATIC", L"Запуск Android в отдельном окне · техническая версия", SS_LEFT);
        sdk_label_ = make(L"STATIC", L"Папка Android SDK", SS_LEFT);
        make(L"EDIT", saved_.sdk_root.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, sdk_path);
        make(L"BUTTON", L"Выбрать…", WS_TABSTOP | BS_PUSHBUTTON, browse_sdk);
        sdk_hint_ = make(L"STATIC",
            L"Сначала установите Android Emulator и создайте устройство с Google Play\nв Android Studio → Device Manager. Sifa использует эти локальные файлы.", SS_LEFT);
        make(L"BUTTON", L"Скачать Android Studio", WS_TABSTOP | BS_PUSHBUTTON, install_sdk);
        device_label_ = make(L"STATIC", L"Устройство Android (AVD)", SS_LEFT);
        make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, device_list);
        make(L"BUTTON", L"Обновить список", WS_TABSTOP | BS_PUSHBUTTON, refresh_devices);

        cpu_label_ = make(L"STATIC", L"Ядра CPU", SS_LEFT);
        memory_label_ = make(L"STATIC", L"Память Android", SS_LEFT);
        gpu_label_ = make(L"STATIC", L"Графика", SS_LEFT);
        resolution_label_ = make(L"STATIC", L"Разрешение Android", SS_LEFT);
        make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, cpu_list);
        make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, memory_list);
        make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, gpu_list);
        make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, resolution_list);

        const unsigned cpu_limit = std::max(1U, std::min(16U,
            host_.logical_cpus > 1 ? host_.logical_cpus - 1 : 1));
        for (unsigned value : {1U, 2U, 3U, 4U, 6U, 8U, 12U, 16U}) {
            if (value > cpu_limit) continue;
            cpu_values_.push_back(value);
            const auto label = std::to_wstring(value);
            SendMessageW(control(cpu_list), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        const auto memory_limit = host_.memory_mb == 0 ? 8192ULL :
            std::max<std::uint64_t>(1536, host_.memory_mb > 2048 ? host_.memory_mb - 2048 : 1536);
        for (unsigned value : {1536U, 2048U, 3072U, 4096U, 6144U, 8192U, 12288U, 16384U}) {
            if (value > memory_limit) continue;
            memory_values_.push_back(value);
            const auto label = std::to_wstring(value) + L" МБ";
            SendMessageW(control(memory_list), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        select_value(cpu_list, cpu_values_, saved_.cores);
        select_value(memory_list, memory_values_, saved_.memory_mb);
        for (const wchar_t* label : {L"Авто (рекомендуется)", L"GPU компьютера", L"Программная (CPU)"})
            SendMessageW(control(gpu_list), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(control(gpu_list), CB_SETCURSEL,
            saved_.gpu == "host" ? 1 : saved_.gpu == "software" ? 2 : 0, 0);
        std::size_t resolution_index = 0;
        for (std::size_t i = 0; i < resolutions.size(); ++i) {
            const auto& resolution = resolutions[i];
            SendMessageW(control(resolution_list), CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(resolution.label));
            if (saved_.width == resolution.width && saved_.height == resolution.height)
                resolution_index = i;
        }
        SendMessageW(control(resolution_list), CB_SETCURSEL, resolution_index, 0);

        make(L"BUTTON", L"Запуск Android", WS_TABSTOP | BS_DEFPUSHBUTTON, start_android);
        make(L"BUTTON", L"Остановить", WS_TABSTOP | BS_PUSHBUTTON, stop_android);
        make(L"BUTTON", L"Google Play", WS_TABSTOP | BS_PUSHBUTTON, open_play);
        make(L"BUTTON", L"Игра в Play", WS_TABSTOP | BS_PUSHBUTTON, game_page);
        make(L"BUTTON", L"Запустить игру", WS_TABSTOP | BS_PUSHBUTTON, start_game);
        make(L"BUTTON", L"Управление", WS_TABSTOP | BS_PUSHBUTTON, input_settings);
        status_heading_ = make(L"STATIC", L"Проверка Android SDK…", SS_LEFT);
        SendMessageW(status_heading_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_font_), TRUE);
        status_detail_ = make(L"STATIC", L"", SS_LEFT);
        log_label_ = make(L"STATIC", L"Состояние и сообщения", SS_LEFT);
        make(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
             log_output);
        SendMessageW(control(log_output), EM_SETLIMITTEXT, 65536, 0);
        limitation_ = make(L"STATIC",
            L"Экспериментальное управление: кнопка «Управление». Захват — F8, выход — Esc.\n"
            L"Совместимость и FPS Standoff 2 не подтверждены. Google-аккаунт вводится внутри Android.", SS_LEFT);
        layout();
        update_controls();
    }

    void select_value(int id, const std::vector<unsigned>& values, unsigned desired) {
        std::size_t selected = 0;
        for (std::size_t i = 0; i < values.size(); ++i)
            if (values[i] <= desired) selected = i;
        SendMessageW(control(id), CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    }

    void layout() {
        RECT area{};
        GetClientRect(window_, &area);
        const int width = MulDiv(area.right, 96, dpi_);
        const int height = MulDiv(area.bottom, 96, dpi_);
        const int left = 24, content = std::max(500, width - 48);
        const auto place = [&](HWND item, int x, int y, int w, int h) {
            MoveWindow(item, px(x), px(y), px(std::max(1, w)), px(std::max(1, h)), TRUE);
        };
        place(heading_, left, 17, 170, 39);
        place(subtitle_, left, 60, content, 25);
        place(sdk_label_, left, 96, content, 22);
        place(control(sdk_path), left, 121, content - 126, 31);
        place(control(browse_sdk), left + content - 116, 120, 116, 33);
        place(sdk_hint_, left, 163, content - 223, 47);
        place(control(install_sdk), left + content - 213, 165, 213, 34);
        place(device_label_, left, 218, content, 22);
        place(control(device_list), left, 243, content - 176, 240);
        place(control(refresh_devices), left + content - 166, 241, 166, 34);
        const int field_width = (content - 36) / 4;
        const HWND labels[] = {cpu_label_, memory_label_, gpu_label_, resolution_label_};
        const int fields[] = {cpu_list, memory_list, gpu_list, resolution_list};
        for (int i = 0; i < 4; ++i) {
            const int column_width = i == 3 ? content - 3 * (field_width + 12) : field_width;
            const int column_x = left + i * (field_width + 12);
            place(labels[i], column_x, 288, column_width, 22);
            place(control(fields[i]), column_x, 313, column_width, 240);
        }
        const int button_width = (content - 40) / 6;
        const int buttons[] = {start_android, stop_android, open_play, game_page, start_game, input_settings};
        for (int i = 0; i < 6; ++i)
            place(control(buttons[i]), left + i * (button_width + 8), 360, button_width, 38);
        place(status_heading_, left, 417, content, 25);
        place(status_detail_, left, 448, content, 43);
        place(log_label_, left, 502, content, 22);
        const int log_height = std::max(65, height - 611);
        place(control(log_output), left, 528, content, log_height);
        place(limitation_, left, 544 + log_height, content, 51);
    }

    void update_controls() {
        const bool editing = !busy_ && !status_.running && !closing_;
        for (int id : {sdk_path, browse_sdk, refresh_devices, device_list, cpu_list,
                      memory_list, gpu_list, resolution_list})
            EnableWindow(control(id), editing);
        const auto selected = SendMessageW(control(device_list), CB_GETCURSEL, 0, 0);
        EnableWindow(control(start_android), editing && sdk_.valid && selected != CB_ERR);
        EnableWindow(control(stop_android), !busy_ && status_.running && !closing_);
        EnableWindow(control(open_play), !busy_ && status_.running && status_.boot_completed &&
            status_.play_store_installed && !closing_);
        EnableWindow(control(game_page), !busy_ && status_.running && status_.boot_completed &&
            status_.play_store_installed && !closing_);
        EnableWindow(control(start_game), !busy_ && status_.running && status_.boot_completed &&
            status_.game_installed && !closing_);
        EnableWindow(control(input_settings), !busy_ && status_.running && status_.boot_completed && !closing_);
        EnableWindow(control(install_sdk), !closing_);
        if (busy_) {
            SetWindowTextW(status_heading_, closing_ ? L"Завершение работы…" : activity_.c_str());
        } else {
            SetWindowTextW(status_heading_, status_text(status_).c_str());
        }
        std::wstring detail;
        if (status_.running) {
            detail = L"Устройство: " + wide(status_.avd_name) + L"    " + wide(status_.serial);
            if (status_.boot_completed) {
                detail += L"\nGoogle Play: ";
                detail += status_.play_store_installed ? L"установлен" : L"не найден";
                detail += L"    Standoff 2: ";
                detail += status_.game_installed ? L"установлен" : L"не установлен";
            } else detail += L"\nПервый запуск Android может занять несколько минут.";
        } else if (!sdk_.valid) {
            detail = L"Нужен Android SDK с emulator.exe и adb.exe. Выберите папку SDK и обновите список.";
        } else if (devices_.empty()) {
            detail = L"Устройств AVD нет. Создайте устройство с образом Google Play в Android Studio → Device Manager.";
        } else if (selected >= 0 && static_cast<std::size_t>(selected) < devices_.size() &&
                   !devices_[static_cast<std::size_t>(selected)].google_play) {
            detail = L"Выбран образ без Google Play. Для установки игры из магазина выберите или создайте AVD с Google Play.";
        } else {
            detail = L"Android откроется в собственном окне. Через Google Play можно найти и установить игру.";
        }
        SetWindowTextW(status_detail_, detail.c_str());
    }

    android::LaunchOptions options_from_controls() const {
        android::LaunchOptions options;
        options.sdk_root = std::filesystem::path(text_of(control(sdk_path)));
        const auto selected = SendMessageW(control(device_list), CB_GETCURSEL, 0, 0);
        if (selected >= 0 && static_cast<std::size_t>(selected) < devices_.size())
            options.avd_name = devices_[static_cast<std::size_t>(selected)].name;
        const auto cpu = SendMessageW(control(cpu_list), CB_GETCURSEL, 0, 0);
        if (cpu >= 0 && static_cast<std::size_t>(cpu) < cpu_values_.size())
            options.cores = cpu_values_[static_cast<std::size_t>(cpu)];
        const auto memory = SendMessageW(control(memory_list), CB_GETCURSEL, 0, 0);
        if (memory >= 0 && static_cast<std::size_t>(memory) < memory_values_.size())
            options.memory_mb = memory_values_[static_cast<std::size_t>(memory)];
        const auto gpu = SendMessageW(control(gpu_list), CB_GETCURSEL, 0, 0);
        options.gpu = gpu == 1 ? "host" : gpu == 2 ? "software" : "auto";
        const auto resolution_index = SendMessageW(control(resolution_list), CB_GETCURSEL, 0, 0);
        const auto& resolution = resolutions[resolution_index >= 0 &&
            static_cast<std::size_t>(resolution_index) < resolutions.size()
                ? static_cast<std::size_t>(resolution_index) : 0];
        options.width = resolution.width;
        options.height = resolution.height;
        return options;
    }

    void begin(Operation operation) {
        if (busy_) return;
        if (operation == Operation::stop && game_input_) game_input_->stop();
        const auto options = options_from_controls();
        if (operation == Operation::start) {
            saved_ = options;
            save_preferences();
        }
        switch (operation) {
        case Operation::inspect: activity_ = L"Проверка SDK и устройств…"; break;
        case Operation::start: activity_ = L"Запуск Android…"; break;
        case Operation::poll: activity_ = status_text(status_); break;
        case Operation::stop: activity_ = L"Остановка Android…"; break;
        case Operation::play: activity_ = L"Открытие Google Play…"; break;
        case Operation::game_store: activity_ = L"Открытие страницы Standoff 2…"; break;
        case Operation::game: activity_ = L"Запуск установленной игры…"; break;
        }
        busy_ = true;
        update_controls();
        worker_ = std::thread([this, operation, options] {
            auto result = std::make_unique<Result>();
            result->operation = operation;
            try {
                switch (operation) {
                case Operation::inspect:
                    result->sdk = android::discover_sdk(options.sdk_root);
                    result->devices = android::discover_avds(result->error);
                    result->ok = result->sdk.valid;
                    if (!result->sdk.valid) {
                        if (!result->error.empty()) result->error += "\n";
                        result->error += result->sdk.error;
                    }
                    break;
                case Operation::start: result->ok = runtime_.start(options, result->error); break;
                case Operation::poll:
                    result->status = runtime_.refresh();
                    result->ok = result->status.error.empty();
                    result->error = result->status.error;
                    break;
                case Operation::stop: result->ok = runtime_.stop(result->error); break;
                case Operation::play: result->ok = runtime_.open_play_store(result->error); break;
                case Operation::game_store: result->ok = runtime_.open_game_store_page(result->error); break;
                case Operation::game: result->ok = runtime_.open_game(result->error); break;
                }
                if (operation != Operation::poll) result->status = runtime_.snapshot();
            } catch (const std::exception& error) {
                result->ok = false;
                result->error = error.what();
                result->status = runtime_.snapshot();
            }
            {
                const std::lock_guard lock(result_mutex_);
                result_ = std::move(result);
            }
            PostMessageW(window_, work_completed, 0, 0);
        });
    }

    void completed() {
        if (worker_.joinable()) worker_.join();
        std::unique_ptr<Result> result;
        {
            const std::lock_guard lock(result_mutex_);
            result = std::move(result_);
        }
        busy_ = false;
        if (!result) return;
        status_ = result->status;
        if (!status_.running && game_input_) game_input_->stop();
        if (result->operation == Operation::inspect) {
            const auto previous = options_from_controls().avd_name;
            sdk_ = result->sdk;
            devices_ = result->devices;
            if (sdk_.valid) {
                setting_sdk_path_ = true;
                SetWindowTextW(control(sdk_path), sdk_.root.c_str());
                setting_sdk_path_ = false;
            }
            SendMessageW(control(device_list), CB_RESETCONTENT, 0, 0);
            std::size_t selected = 0;
            bool preferred_found = false;
            for (std::size_t i = 0; i < devices_.size(); ++i) {
                const auto& device = devices_[i];
                const auto label = wide(device.name) + (device.google_play ? L"  · Google Play" : L"  · без Google Play") +
                    (device.abi.empty() ? L"" : L"  · " + wide(device.abi));
                SendMessageW(control(device_list), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                if ((!preferred_found && device.google_play) || device.name == saved_.avd_name ||
                    (!previous.empty() && device.name == previous)) {
                    selected = i;
                    preferred_found = true;
                }
            }
            if (!devices_.empty()) SendMessageW(control(device_list), CB_SETCURSEL, selected, 0);
            if (sdk_.valid) append(L"Android SDK: " + sdk_.root.wstring());
            append(L"Найдено устройств AVD: " + std::to_wstring(devices_.size()));
        }
        std::wstring report = status_text(status_);
        if (status_.running && status_.boot_completed)
            report += status_.play_store_installed ? L". Google Play найден." : L". Google Play не найден в этом образе.";
        if (status_.running && status_.game_installed) report += L" Standoff 2 установлен.";
        if (!result->error.empty()) report += L"\r\n" + wide(result->error);
        if (report != last_report_ || result->operation != Operation::poll) {
            if (result->ok) {
                if (result->operation == Operation::play) append(L"Отправлена команда открытия Google Play.");
                if (result->operation == Operation::game_store) append(L"Отправлена команда открытия страницы Standoff 2 в Google Play.");
                if (result->operation == Operation::game) append(L"Отправлена команда запуска Standoff 2. Успешный вход в игру ещё не проверен.");
                if (result->operation == Operation::start && !status_.log_path.empty())
                    append(L"Журнал Android Emulator: " + status_.log_path.wstring());
            }
            append(report);
            last_report_ = report;
        }
        update_controls();
        if (closing_) {
            if (status_.running && !close_stop_attempted_) {
                close_stop_attempted_ = true;
                begin(Operation::stop);
            } else if (status_.running) {
                closing_ = false;
                close_stop_attempted_ = false;
                append(L"Android пока не остановлен. Повторите остановку; окно Sifa оставлено открытым.");
                update_controls();
            } else DestroyWindow(window_);
        }
    }

    void append(const std::wstring& value) {
        const HWND output = control(log_output);
        if (GetWindowTextLengthW(output) > 48000) SetWindowTextW(output, L"");
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t time[20]{};
        swprintf(time, std::size(time), L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
        const auto line = std::wstring(time) + value + L"\r\n";
        SendMessageW(output, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(output, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(output, EM_SCROLLCARET, 0, 0);
    }

    void pick_sdk() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) {
            append(L"Не удалось открыть выбор папки. Введите путь Android SDK вручную.");
            return;
        }
        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Выберите папку Android SDK (с папками emulator и platform-tools)");
        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* selection = nullptr;
            if (SUCCEEDED(dialog->GetResult(&selection))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(selection->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    SetWindowTextW(control(sdk_path), path);
                    CoTaskMemFree(path);
                    begin(Operation::inspect);
                }
                selection->Release();
            }
        }
        dialog->Release();
    }

    void load_preferences() {
        saved_.width = 1280;
        saved_.height = 720;
        try {
            std::ifstream stream(data_directory_ / "android-desktop.json");
            if (!stream) return;
            const auto json = nlohmann::json::parse(stream);
            saved_.sdk_root = std::filesystem::path(wide(json.value("sdk_root", std::string{})));
            saved_.avd_name = json.value("avd", std::string{});
            saved_.cores = json.value("cores", 2U);
            saved_.memory_mb = json.value("memory_mb", 2048U);
            saved_.gpu = json.value("gpu", std::string("auto"));
            const unsigned width = json.value("width", 1280U);
            const unsigned height = json.value("height", 720U);
            if (std::any_of(resolutions.begin(), resolutions.end(), [&](const Resolution& value) {
                return value.width == width && value.height == height;
            })) {
                saved_.width = width;
                saved_.height = height;
            }
        } catch (const std::exception&) {
            saved_ = android::LaunchOptions{};
            saved_.width = 1280;
            saved_.height = 720;
        }
    }

    void save_preferences() {
        try {
            std::filesystem::create_directories(data_directory_);
            const nlohmann::json json = {
                {"sdk_root", utf8(saved_.sdk_root.wstring())}, {"avd", saved_.avd_name},
                {"cores", saved_.cores}, {"memory_mb", saved_.memory_mb}, {"gpu", saved_.gpu},
                {"width", saved_.width}, {"height", saved_.height}
            };
            const auto destination = data_directory_ / "android-desktop.json";
            const auto temporary = data_directory_ / "android-desktop.json.tmp";
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                if (!stream || !(stream << json.dump(2) << '\n'))
                    throw std::runtime_error("Cannot save desktop settings");
            }
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace desktop settings");
        } catch (const std::exception& error) {
            append(L"Настройки не сохранены: " + wide(error.what()));
        }
    }

    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (game_input_) {
            LRESULT result = 0;
            if (game_input_->handle_message(message, wparam, lparam, result)) return result;
        }
        switch (message) {
        case WM_CREATE:
            create_controls();
            game_input_ = std::make_unique<GameInput>(window_, runtime_, data_directory_ / "game-input.json");
            return 0;
        case WM_SIZE: if (heading_) layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {px(900), px(700)};
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(33, 43, 54));
            return reinterpret_cast<LRESULT>(background_);
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                switch (LOWORD(wparam)) {
                case browse_sdk: if (!busy_) pick_sdk(); return 0;
                case refresh_devices: begin(Operation::inspect); return 0;
                case start_android: begin(Operation::start); return 0;
                case stop_android: begin(Operation::stop); return 0;
                case open_play: begin(Operation::play); return 0;
                case game_page: begin(Operation::game_store); return 0;
                case start_game: begin(Operation::game); return 0;
                case input_settings:
                    if (game_input_ && status_.running && status_.boot_completed) game_input_->show_settings();
                    return 0;
                case install_sdk:
                    ShellExecuteW(window_, L"open", L"https://developer.android.com/studio",
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
            }
            if (LOWORD(wparam) == sdk_path && HIWORD(wparam) == EN_CHANGE && !busy_ && !setting_sdk_path_) {
                sdk_.valid = false;
                update_controls();
            }
            if (LOWORD(wparam) == device_list && HIWORD(wparam) == CBN_SELCHANGE) update_controls();
            return 0;
        case WM_TIMER:
            if (wparam == poll_timer && status_.running && !busy_ && !closing_) begin(Operation::poll);
            return 0;
        case work_completed: completed(); return 0;
        case WM_CLOSE:
            if (closing_) return 0;
            if (game_input_) game_input_->stop();
            saved_ = options_from_controls();
            save_preferences();
            closing_ = true;
            if (!busy_ && status_.running) {
                close_stop_attempted_ = true;
                begin(Operation::stop);
            } else if (!busy_) DestroyWindow(window_);
            else update_controls();
            return 0;
        case WM_DESTROY:
            KillTimer(window_, poll_timer);
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(window_, message, wparam, lparam);
        }
    }
};

} // namespace

int run_desktop_app() {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    try {
        Desktop app;
        result = app.run(GetModuleHandleW(nullptr));
    } catch (const std::exception& error) {
        const auto message = L"Не удалось открыть Sifa:\n" + wide(error.what());
        MessageBoxW(nullptr, message.c_str(), L"Sifa", MB_ICONERROR | MB_OK);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

} // namespace sifa

#endif
