#include "sifa/game_input.hpp"
#ifdef _WIN32
#include "sifa/android_runtime.hpp"
#include "third_party/json.hpp"
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

namespace sifa {
namespace {
constexpr UINT input_ready = WM_APP + 72;
constexpr UINT input_failed = WM_APP + 73;
constexpr UINT_PTR input_timer = 0x53494641;
constexpr int input_hotkey = 0x5349;
constexpr wchar_t overlay_class[] = L"SifaInputCapture";
constexpr wchar_t settings_class[] = L"SifaInputSettings";
using input::MappingConfig;

std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(static_cast<std::size_t>(std::max(0, size)), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), size);
    return value;
}
std::wstring text(HWND window) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    return value;
}
unsigned window_pid(HWND window) { DWORD pid = 0; GetWindowThreadProcessId(window, &pid); return pid; }

struct Preferences {
    MappingConfig mapping;
    unsigned rotation = 0;
    bool enabled = false;
};

void load_preferences(const std::filesystem::path& path, Preferences& result) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 65536 || ec) return;
    try {
        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        Preferences saved;
        saved.enabled = json.value("enabled", false);
        saved.rotation = json.value("rotation", 0U);
        auto& config = saved.mapping;
        config.width = json.value("width", 1280U);
        config.height = json.value("height", 720U);
        config.mouse_gain = json.value("mouse_gain", 0.001);
        config.invert_y = json.value("invert_y", false);
        config.joystick_radius = json.value("joystick_radius", 0.10);
        const auto point = [&](const char* name, input::Point fallback) {
            if (!json.contains(name)) return fallback;
            return input::Point{json.at(name).at(0).get<double>(), json.at(name).at(1).get<double>()};
        };
        config.joystick_center = point("joystick", config.joystick_center);
        config.aim_center = point("aim", config.aim_center);
        config.fire = point("fire", config.fire);
        config.jump = point("jump", config.jump);
        config.reload = point("reload", config.reload);
        config.crouch = point("crouch", config.crouch);
        if (json.contains("aim_bounds")) {
            const auto& bounds = json.at("aim_bounds");
            config.aim_bounds = {bounds.at(0).get<double>(), bounds.at(1).get<double>(),
                                bounds.at(2).get<double>(), bounds.at(3).get<double>()};
        }
        input::Mapper validate(config);
        if (saved.rotation <= 3) result = saved;
    } catch (...) { /* Invalid user configuration is not activated. */ }
}

void save_preferences(const std::filesystem::path& path, const Preferences& saved) {
    const auto point = [](input::Point p) { return nlohmann::json::array({p.x, p.y}); };
    const auto& c = saved.mapping;
    const nlohmann::json json{{"schema_version", 1}, {"enabled", saved.enabled},
        {"rotation", saved.rotation}, {"width", c.width}, {"height", c.height},
        {"mouse_gain", c.mouse_gain}, {"invert_y", c.invert_y},
        {"joystick_radius", c.joystick_radius}, {"joystick", point(c.joystick_center)},
        {"aim", point(c.aim_center)}, {"fire", point(c.fire)}, {"jump", point(c.jump)},
        {"reload", point(c.reload)}, {"crouch", point(c.crouch)},
        {"aim_bounds", {c.aim_bounds.left, c.aim_bounds.top, c.aim_bounds.right, c.aim_bounds.bottom}}};
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << json.dump(2) << '\n';
        if (!file) throw std::runtime_error("Не удалось сохранить настройки управления");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Не удалось заменить файл настроек управления");
}

struct SettingsDialog {
    HWND window = nullptr, owner = nullptr;
    HFONT font = nullptr;
    Preferences values;
    bool done = false, accepted = false;
    struct Field { const wchar_t* label; double number; HWND edit = nullptr; };
    std::vector<Field> fields;
    explicit SettingsDialog(HWND parent, Preferences current) : owner(parent), values(current) {
        const auto& c = values.mapping;
        fields = {
            {L"Ширина Android", static_cast<double>(c.width)}, {L"Высота Android", static_cast<double>(c.height)},
            {L"Поворот: 0, 1, 2, 3", static_cast<double>(values.rotation)},
            {L"Джойстик X", c.joystick_center.x}, {L"Джойстик Y", c.joystick_center.y}, {L"Радиус джойстика", c.joystick_radius},
            {L"Прицеливание X", c.aim_center.x}, {L"Прицеливание Y", c.aim_center.y}, {L"Чувствительность", c.mouse_gain},
            {L"Стрельба X", c.fire.x}, {L"Стрельба Y", c.fire.y}, {L"Зона: левая граница", c.aim_bounds.left},
            {L"Прыжок X", c.jump.x}, {L"Прыжок Y", c.jump.y}, {L"Зона: правая граница", c.aim_bounds.right},
            {L"Перезарядка X", c.reload.x}, {L"Перезарядка Y", c.reload.y}, {L"Зона: верхняя граница", c.aim_bounds.top},
            {L"Присесть X", c.crouch.x}, {L"Присесть Y", c.crouch.y}, {L"Зона: нижняя граница", c.aim_bounds.bottom}
        };
    }
    ~SettingsDialog() { if (font) DeleteObject(font); }
    HWND add(const wchar_t* kind, const wchar_t* label, DWORD style, int x, int y, int w, int h, int id=0) {
        const auto item = CreateWindowExW(wcscmp(kind,L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,
            kind,label,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr),nullptr);
        SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        return item;
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self=reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<SettingsDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self) return DefWindowProcW(window,message,wp,lp);
        if(message==WM_CLOSE) { self->done=true; return 0; }
        if(message==WM_COMMAND && LOWORD(wp)==2) { self->done=true; return 0; }
        if(message==WM_COMMAND && LOWORD(wp)==1) {
            try {
                for(auto& field:self->fields) {
                    auto value=text(field.edit); std::replace(value.begin(),value.end(),L',',L'.');
                    std::size_t used=0; field.number=std::stod(value,&used);
                    if(used!=value.size() || !std::isfinite(field.number)) throw std::runtime_error("Проверьте числовые значения");
                }
                auto& c=self->values.mapping;
                const auto n=[&](int i){return self->fields[static_cast<std::size_t>(i)].number;};
                for (int i : {0,1,2}) if(n(i)<0 || n(i)>16384 || std::floor(n(i))!=n(i))
                    throw std::runtime_error("Размеры и поворот должны быть целыми числами");
                c.width=static_cast<unsigned>(n(0));c.height=static_cast<unsigned>(n(1));
                self->values.rotation=static_cast<unsigned>(n(2));
                if(self->values.rotation>3) throw std::runtime_error("Поворот должен быть от 0 до 3");
                c.joystick_center={n(3),n(4)};c.joystick_radius=n(5);
                c.aim_center={n(6),n(7)};c.mouse_gain=n(8);
                c.fire={n(9),n(10)};c.jump={n(12),n(13)};c.reload={n(15),n(16)};c.crouch={n(18),n(19)};
                c.aim_bounds={n(11),n(17),n(14),n(20)};
                c.invert_y=SendMessageW(GetDlgItem(window,102),BM_GETCHECK,0,0)==BST_CHECKED;
                self->values.enabled=SendMessageW(GetDlgItem(window,101),BM_GETCHECK,0,0)==BST_CHECKED;
                input::Mapper valid(c);
                self->accepted=self->done=true;
            } catch(const std::exception& e) { MessageBoxW(window,widen(e.what()).c_str(),L"Настройки управления",MB_OK|MB_ICONWARNING); }
            return 0;
        }
        return DefWindowProcW(window,message,wp,lp);
    }
    bool show() {
        WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=settings_class;wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
        wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&wc);
        window=CreateWindowExW(WS_EX_DLGMODALFRAME,settings_class,L"Sifa — управление мышью и клавиатурой",
            WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,860,590,owner,nullptr,wc.hInstance,this);
        if(!window)return false;
        font=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        add(L"STATIC",L"WASD — движение · мышь — обзор · ЛКМ — огонь · Space — прыжок · R — перезарядка · Ctrl — присесть",SS_LEFT,18,16,808,42);
        add(L"STATIC",L"F8 в окне Android включает захват. Esc или смена окна отпускает клавиши и курсор.\nКоординаты 0…1 относятся к экрану Android после поворота; настройте их под свой HUD.\nЗначения по умолчанию и чувствительность ещё не проверены в Standoff 2.",SS_LEFT,18,62,808,66);
        auto enabled=add(L"BUTTON",L"Разрешить экспериментальное управление для этого устройства",BS_AUTOCHECKBOX|WS_TABSTOP,18,135,800,26,101);
        SendMessageW(enabled,BM_SETCHECK,values.enabled?BST_CHECKED:BST_UNCHECKED,0);
        auto invert=add(L"BUTTON",L"Инверсия вертикального движения мыши",BS_AUTOCHECKBOX|WS_TABSTOP,18,163,800,25,102);
        SendMessageW(invert,BM_SETCHECK,values.mapping.invert_y?BST_CHECKED:BST_UNCHECKED,0);
        for(std::size_t i=0;i<fields.size();++i) {
            const int x=18+static_cast<int>(i%3)*275,y=203+static_cast<int>(i/3)*39;
            add(L"STATIC",fields[i].label,SS_LEFT,x,y+5,170,24);
            std::wostringstream value;value.precision(7);value<<fields[i].number;
            fields[i].edit=add(L"EDIT",value.str().c_str(),ES_AUTOHSCROLL|WS_TABSTOP,x+173,y,84,29,200+static_cast<int>(i));
        }
        add(L"BUTTON",L"Сохранить",BS_DEFPUSHBUTTON|WS_TABSTOP,574,492,120,34,1);
        add(L"BUTTON",L"Отмена",BS_PUSHBUTTON|WS_TABSTOP,706,492,120,34,2);
        EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);
        MSG message{};
        while(!done && GetMessageW(&message,nullptr,0,0)>0) {
            if(!IsDialogMessageW(window,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}
        }
        EnableWindow(owner,TRUE);DestroyWindow(window);SetForegroundWindow(owner);
        return accepted;
    }
};
}

struct GameInput::Impl {
    HWND owner=nullptr, overlay=nullptr, emulator_window=nullptr;
    android::Runtime& runtime;
    std::filesystem::path settings_file;
    Preferences preferences;
    std::unique_ptr<input::Mapper> mapper;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    enum class Kind { connect, send, release, quit };
    struct Task { Kind kind; std::uint64_t epoch; std::uint16_t port=0; unsigned rotation=0; input::Frames frames; };
    std::deque<Task> tasks;
    std::uint64_t epoch=0;
    bool registered=false, connecting=false, shutting_down=false;
    int cursor_hides=0;
    double pending_x=0,pending_y=0;
    std::string last_error;

    Impl(HWND window,android::Runtime& target,std::filesystem::path file):owner(window),runtime(target),settings_file(std::move(file)) {
        load_preferences(settings_file,preferences);
        worker=std::thread([this]{work();});
        SetTimer(owner,input_timer,8,nullptr);
    }
    ~Impl() {
        shutting_down=true;stop();KillTimer(owner,input_timer);
        if(registered)UnregisterHotKey(owner,input_hotkey);
        {
            std::lock_guard lock(mutex);tasks.clear();tasks.push_back({Kind::quit,epoch,0,0,{}});
        }
        wake.notify_one();if(worker.joinable())worker.join();
    }
    void post(Task task) {
        bool overflow=false;
        {
            std::lock_guard lock(mutex);
            if(tasks.size()>128) {tasks.clear();tasks.push_back({Kind::release,epoch,0,0,{}});last_error="Очередь ввода переполнена; захват остановлен";overflow=true;}
            else tasks.push_back(std::move(task));
        }
        wake.notify_one();
        if(overflow)PostMessageW(owner,input_failed,static_cast<WPARAM>(epoch),0);
    }
    void work() {
        input::ConsoleInput connection;
        std::uint64_t connected_epoch=0;
        std::uint16_t connected_port=0;
        const auto release=[&](std::string& error) {
            if(connection.release_all(error)) return;
            android::InputEndpoint endpoint;
            // A broken transport must not leave a held finger behind. Reconnect
            // only after checking ownership again; never control a reused port.
            if(connected_port && runtime.owned_console_endpoint(endpoint,error)
                && endpoint.port==connected_port && connection.connect(connected_port,error))
                connection.release_all(error);
        };
        while(true) {
            Task task;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return !tasks.empty();});task=std::move(tasks.front());tasks.pop_front();}
            std::string error;
            if(task.kind==Kind::quit) {release(error);connection.close();return;}
            if(task.kind==Kind::release) {release(error);connection.close();connected_epoch=0;connected_port=0;continue;}
            if(task.kind==Kind::connect) {
                connection.release_all(error);connection.close();error.clear();
                if(connection.connect(task.port,error)) {
                    connected_epoch=task.epoch;connected_port=task.port;
                    PostMessageW(owner,input_ready,static_cast<WPARAM>(task.epoch),0);
                } else {
                    {std::lock_guard lock(mutex);last_error=error;}
                    PostMessageW(owner,input_failed,static_cast<WPARAM>(task.epoch),0);
                }
            } else if(task.epoch==connected_epoch && !connection.send(task.frames,task.rotation,error)) {
                const auto cause=error;
                release(error);connection.close();connected_epoch=0;connected_port=0;
                error=cause;
                {std::lock_guard lock(mutex);last_error=error;}
                PostMessageW(owner,input_failed,static_cast<WPARAM>(task.epoch),0);
            }
        }
    }
    bool owned_foreground() {
        const auto foreground=GetForegroundWindow();
        return foreground && runtime.owns_process(window_pid(foreground));
    }
    void emit(input::Frames frames) {
        if(!frames.empty())post({Kind::send,epoch,0,preferences.rotation,std::move(frames)});
    }
    void activate() {
        if(overlay || connecting) {stop();return;}
        if(!preferences.enabled || !owned_foreground())return;
        android::InputEndpoint endpoint;
        std::string error;
        if(!runtime.owned_console_endpoint(endpoint,error)) {
            MessageBoxW(owner,widen(error).c_str(),L"Sifa — управление",MB_OK|MB_ICONWARNING);return;
        }
        emulator_window=GetForegroundWindow();
        ++epoch;connecting=true;
        post({Kind::connect,epoch,endpoint.port,preferences.rotation,{}});
    }
    static LRESULT CALLBACK overlay_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->overlay=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self)return DefWindowProcW(window,message,wp,lp);
        if(message==WM_INPUT) {self->raw_input(reinterpret_cast<HRAWINPUT>(lp));return DefWindowProcW(window,message,wp,lp);}
        if(message==WM_ACTIVATE && LOWORD(wp)==WA_INACTIVE) {self->stop();return 0;}
        if(message==WM_CLOSE) {self->stop();return 0;}
        if(message==WM_KEYDOWN && (wp==VK_ESCAPE || wp==VK_F8)) {self->stop();return 0;}
        if(message==WM_SETCURSOR) {SetCursor(nullptr);return TRUE;}
        return DefWindowProcW(window,message,wp,lp);
    }
    void begin_capture() {
        connecting=false;
        if(!IsWindow(emulator_window) || GetForegroundWindow()!=emulator_window || !owned_foreground()) {stop();return;}
        RECT rect{};GetClientRect(emulator_window,&rect);
        POINT origin{0,0};ClientToScreen(emulator_window,&origin);OffsetRect(&rect,origin.x,origin.y);
        if(rect.right-rect.left<64 || rect.bottom-rect.top<64) {stop();return;}
        WNDCLASSW wc{};wc.lpfnWndProc=overlay_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=overlay_class;
        wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));RegisterClassW(&wc);
        mapper=std::make_unique<input::Mapper>(preferences.mapping);
        // A focused almost-transparent overlay consumes legacy Qt mouse events,
        // preventing duplicate touches without installing global input hooks.
        overlay=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,overlay_class,L"Sifa — F8 / Esc",
            WS_POPUP,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,owner,nullptr,wc.hInstance,this);
        if(!overlay) {stop();return;}
        SetLayeredWindowAttributes(overlay,0,1,LWA_ALPHA);
        const RAWINPUTDEVICE devices[]={{1,2,RIDEV_NOLEGACY,overlay},{1,6,RIDEV_NOLEGACY,overlay}};
        if(!RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE))) {stop();return;}
        ShowWindow(overlay,SW_SHOW);SetForegroundWindow(overlay);SetFocus(overlay);
        const LONG x=rect.left+(rect.right-rect.left)/2,y=rect.top+(rect.bottom-rect.top)/2;
        RECT cursor{x,y,x+1,y+1};ClipCursor(&cursor);SetCursorPos(x,y);
        cursor_hides=0;
        do {++cursor_hides;} while(ShowCursor(FALSE)>=0 && cursor_hides<16);
        pending_x=pending_y=0;
    }
    void stop() {
        ++epoch;connecting=false;pending_x=pending_y=0;
        if(mapper)mapper->release_all();
        mapper.reset();
        if(overlay) {
            const auto old=overlay;const bool foreground=GetForegroundWindow()==old;overlay=nullptr;
            const RAWINPUTDEVICE devices[]={{1,2,RIDEV_REMOVE,nullptr},{1,6,RIDEV_REMOVE,nullptr}};
            RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE));
            ClipCursor(nullptr);
            while(cursor_hides>0) {ShowCursor(TRUE);--cursor_hides;}
            DestroyWindow(old);
            if(foreground && IsWindow(emulator_window))SetForegroundWindow(emulator_window);
        }
        {std::lock_guard lock(mutex);tasks.clear();tasks.push_back({Kind::release,epoch,0,0,{}});}
        wake.notify_one();
    }
    void raw_input(HRAWINPUT handle) {
        if(!overlay || !mapper || GetForegroundWindow()!=overlay)return;
        UINT size=0;
        if(GetRawInputData(handle,RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER))!=0 || size>4096)return;
        alignas(RAWINPUT) std::array<unsigned char,4096> bytes{};
        if(GetRawInputData(handle,RID_INPUT,bytes.data(),&size,sizeof(RAWINPUTHEADER))!=size)return;
        const auto& raw=*reinterpret_cast<const RAWINPUT*>(bytes.data());
        try {
            if(raw.header.dwType==RIM_TYPEMOUSE) {
                if(!(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)) {
                    pending_x+=raw.data.mouse.lLastX;pending_y+=raw.data.mouse.lLastY;
                }
                if(raw.data.mouse.usButtonFlags&RI_MOUSE_LEFT_BUTTON_DOWN)emit(mapper->set_button(input::Button::fire,true));
                if(raw.data.mouse.usButtonFlags&RI_MOUSE_LEFT_BUTTON_UP)emit(mapper->set_button(input::Button::fire,false));
            } else if(raw.header.dwType==RIM_TYPEKEYBOARD) {
                const bool down=(raw.data.keyboard.Flags&RI_KEY_BREAK)==0;
                const auto key=raw.data.keyboard.VKey;
                if(down && (key==VK_ESCAPE || key==VK_F8)) {stop();return;}
                switch(key) {
                case 'W':emit(mapper->set_button(input::Button::forward,down));break;
                case 'S':emit(mapper->set_button(input::Button::back,down));break;
                case 'A':emit(mapper->set_button(input::Button::left,down));break;
                case 'D':emit(mapper->set_button(input::Button::right,down));break;
                case 'R':emit(mapper->set_button(input::Button::reload,down));break;
                case VK_SPACE:emit(mapper->set_button(input::Button::jump,down));break;
                case VK_CONTROL:case VK_LCONTROL:case VK_RCONTROL:emit(mapper->set_button(input::Button::crouch,down));break;
                default:break;
                }
            }
        } catch(const std::exception& e) {
            {std::lock_guard lock(mutex);last_error=e.what();}
            PostMessageW(owner,input_failed,static_cast<WPARAM>(epoch),0);
        }
    }
    void tick() {
        const auto status=runtime.snapshot();
        if((overlay || connecting) && (!status.running || !status.boot_completed))stop();
        if(overlay) {
            if(GetForegroundWindow()!=overlay || !IsWindow(emulator_window) || !runtime.owns_process(window_pid(emulator_window))) {stop();return;}
            if(pending_x || pending_y) {
                const auto dx=pending_x,dy=pending_y;pending_x=pending_y=0;
                try {emit(mapper->mouse_move(dx,dy));}
                catch(const std::exception& e) {
                    {std::lock_guard lock(mutex);last_error=e.what();}
                    PostMessageW(owner,input_failed,static_cast<WPARAM>(epoch),0);
                }
            }
        }
        const bool want=preferences.enabled && status.running && status.boot_completed && (overlay || owned_foreground());
        if(want && !registered)registered=RegisterHotKey(owner,input_hotkey,MOD_NOREPEAT,VK_F8)!=FALSE;
        if(!want && registered) {UnregisterHotKey(owner,input_hotkey);registered=false;}
    }
};

GameInput::GameInput(HWND owner,android::Runtime& runtime,std::filesystem::path settings_file)
    :impl_(std::make_unique<Impl>(owner,runtime,std::move(settings_file))) {}
GameInput::~GameInput()=default;
void GameInput::stop(){impl_->stop();}
void GameInput::show_settings(){
    impl_->stop();
    SettingsDialog dialog(impl_->owner,impl_->preferences);
    if(!dialog.show())return;
    try {
        save_preferences(impl_->settings_file,dialog.values);
        impl_->preferences=dialog.values;
    } catch(const std::exception& e) {MessageBoxW(impl_->owner,widen(e.what()).c_str(),L"Sifa — управление",MB_OK|MB_ICONERROR);}
}
bool GameInput::handle_message(UINT message,WPARAM wparam,LPARAM, LRESULT& result){
    result=0;
    if(message==WM_TIMER && wparam==input_timer){impl_->tick();return true;}
    if(message==WM_HOTKEY && wparam==input_hotkey){impl_->activate();return true;}
    if(message==input_ready){if(static_cast<std::uint64_t>(wparam)==impl_->epoch)impl_->begin_capture();return true;}
    if(message==input_failed){
        if(static_cast<std::uint64_t>(wparam)==impl_->epoch){
            std::string error;{std::lock_guard lock(impl_->mutex);error=impl_->last_error;}
            impl_->stop();
            MessageBoxW(impl_->owner,widen(error).c_str(),L"Sifa — управление остановлено",MB_OK|MB_ICONWARNING);
        }
        return true;
    }
    return false;
}
}
#endif
