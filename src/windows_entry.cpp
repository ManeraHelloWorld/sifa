#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#include <vector>

int sifa_main(int argc, char** argv);

// The desktop build has no console window. Explicit CLI calls still retain
// inherited redirected handles, which are used by CTest and diagnostics.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) return 1;
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(count));
    bool valid = true;
    for (int i = 0; i < count; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) { valid = false; break; }
        std::string text(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
            text.data(), length, nullptr, nullptr);
        text.pop_back();
        values.push_back(std::move(text));
    }
    LocalFree(wide);
    if (!valid) return 1;
    std::vector<char*> arguments;
    for (auto& value : values) arguments.push_back(value.data());
    arguments.push_back(nullptr);
    return sifa_main(count, arguments.data());
}
#endif
