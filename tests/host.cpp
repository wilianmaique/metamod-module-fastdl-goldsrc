// Exercise the built plugin through the same four exported functions as Metamod.
#include <iostream>
#include <extdll.h>
#include <meta_api.h>

namespace {
const char* game_dir;
const char* original = "http://previous.invalid/";
std::string url = original;
const char* game_info(plid_t, ginfo_t tag) { return tag == GINFO_GAMEDIR ? game_dir : nullptr; }
const char* get_cvar(const char* key) { return strcmp(key, "sv_downloadurl") == 0 ? url.c_str() : ""; }
void set_cvar(const char* key, const char* value) { if (!strcmp(key, "sv_downloadurl")) url = value; }
void log_message(plid_t, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    game_dir = argv[2];
#ifdef _WIN32
    auto module = LoadLibraryA(argv[1]);
    auto symbol = [&](const char* name) { return GetProcAddress(module, name); };
#else
    auto module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) std::cerr << dlerror() << std::endl;
    auto symbol = [&](const char* name) { return dlsym(module, name); };
#endif
    if (!module) return 3;
    auto give = reinterpret_cast<GIVE_ENGINE_FUNCTIONS_FN>(symbol("GiveFnptrsToDll"));
    auto query = reinterpret_cast<META_QUERY_FN>(symbol("Meta_Query"));
    auto attach = reinterpret_cast<META_ATTACH_FN>(symbol("Meta_Attach"));
    auto detach = reinterpret_cast<META_DETACH_FN>(symbol("Meta_Detach"));
    if (!give || !query || !attach || !detach) return 4;
    enginefuncs_t engine{};
    engine.pfnCVarGetString = get_cvar;
    engine.pfnCVarSetString = set_cvar;
    mutil_funcs_t utils{};
    utils.pfnGetGameInfo = game_info;
    utils.pfnLogConsole = utils.pfnLogError = log_message;
    META_FUNCTIONS hooks{};
    meta_globals_t globals{};
    gamedll_funcs_t game{};
    plugin_info_t* info{};
    give(&engine, nullptr);
    char version[] = META_INTERFACE_VERSION;
    if (!query(version, &info, &utils) || !info || strcmp(info->ifvers, version)) return 5;
    if (!attach(PT_STARTUP, &hooks, &globals, &game)) return url == original ? 1 : 6;
    std::cout << url << std::endl;
    int command = std::cin.get();
    if (command == 's') {
        NEW_DLL_FUNCTIONS functions{};
        int api = NEW_DLL_FUNCTIONS_VERSION;
        if (!hooks.pfnGetNewDLLFunctions(&functions, &api) || !functions.pfnGameShutdown) return 7;
        functions.pfnGameShutdown();
    }
    if (command == 'm') url = "manual";
    if (!detach(PT_ANYTIME, PNL_COMMAND) || url != (command == 'm' ? "manual" : original)) return 8;
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
    return 0;
}
