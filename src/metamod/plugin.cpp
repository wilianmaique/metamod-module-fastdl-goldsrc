#include "http.hpp"
#include <cstring>
#include <extdll.h>
#include <meta_api.h>

plugin_info_t Plugin_info = {META_INTERFACE_VERSION, "GoldSrc FastDL", FASTDL_VERSION, "2026-11-22",
                             "GoldSrc FastDL", "", "FASTDL", PT_ANYTIME, PT_ANYTIME};
meta_globals_t* gpMetaGlobals;
namespace {
mutil_funcs_t* utils;
enginefuncs_t engine{};
struct Running {
    Running(const std::filesystem::path& root, const fastdl::Config& config) : http(root, config) {}
    fastdl::HttpService http;
    std::string previous_url;
    std::string assigned_url; // Empty when server.cfg owns the cvar.
};
// Shutdown occurs on the engine thread. Never join workers from DLL_PROCESS_DETACH.
Running* running = nullptr;

void stop() {
    if (!running) return;
    std::unique_ptr<Running> state(running);
    running = nullptr;
    if (state->assigned_url.empty()) return;
    const char* current = engine.pfnCVarGetString("sv_downloadurl");
    if (current && state->assigned_url == current)
        engine.pfnCVarSetString("sv_downloadurl", state->previous_url.c_str());
}

void game_shutdown() {
    stop();
    RETURN_META(MRES_IGNORED);
}

int new_functions(NEW_DLL_FUNCTIONS* out, int* version) {
    if (!version || !out) return FALSE;
    if (*version != NEW_DLL_FUNCTIONS_VERSION) {
        *version = NEW_DLL_FUNCTIONS_VERSION;
        return FALSE;
    }
    *out = {};
    out->pfnGameShutdown = game_shutdown;
    return TRUE;
}
}

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* in, globalvars_t*) {
    // Copy only the stable entries we use from the host's SDK table.
    engine.pfnCVarGetString = in->pfnCVarGetString;
    engine.pfnCVarSetString = in->pfnCVarSetString;
}

C_DLLEXPORT int Meta_Query(char*, plugin_info_t** info, mutil_funcs_t* api) {
    if (!info || !api) return FALSE;
    *info = &Plugin_info;
    utils = api;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS* out, meta_globals_t* globals, gamedll_funcs_t*) {
    if (!out || !globals || !utils || running) return FALSE;
    gpMetaGlobals = globals;
    *out = {};
    out->pfnGetNewDLLFunctions = new_functions;
    try {
        const char* dir = utils->pfnGetGameInfo(PLID, GINFO_GAMEDIR);
        if (!dir || !*dir) throw std::runtime_error("Metamod did not provide the game directory");
        std::filesystem::path root(dir);
        auto config = fastdl::read_config(root / "addons" / "fastdl" / "fastdl.ini");
        auto state = std::make_unique<Running>(root, config);
        if (config.auto_set_downloadurl) {
            const char* previous = engine.pfnCVarGetString("sv_downloadurl");
            state->previous_url = previous ? previous : "";
            state->assigned_url = config.url();
            engine.pfnCVarSetString("sv_downloadurl", state->assigned_url.c_str());
        }
        utils->pfnLogConsole(PLID,
                             "\n----------------------------------------\n"
                             "HTTP %s:%d, sv_downloadurl %s, %d file workers, %d connections\n"
                             "----------------------------------------\n",
                             config.bind.c_str(), config.port,
                             config.auto_set_downloadurl ? state->assigned_url.c_str() : "(manual)",
                             config.workers, config.max_connections);
        running = state.release();
        return TRUE;
    } catch (const std::exception& error) {
        utils->pfnLogError(PLID, "FastDL startup failed: %s", error.what());
        return FALSE;
    }
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
    stop();
    return TRUE;
}
