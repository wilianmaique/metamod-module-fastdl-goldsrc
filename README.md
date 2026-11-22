# GoldSrc FastDL

GoldSrc FastDL is a Metamod plugin that serves Counter-Strike 1.6 resources over HTTP directly from the game directory. The plugin can set the download URL for you, or you can set it in `server.cfg`. It works alongside AMX Mod X and serves resources precached by Pawn plugins.

## Installation

1. With Metamod or Metamod-R installed, extract the package into `cstrike/`. The configuration file (`cstrike/addons/fastdl/fastdl.ini`) and the plugin binary for your platform belong in the same directory.
2. Add the line for your platform to `cstrike/addons/metamod/plugins.ini`, keeping any existing plugin entries.

   ```ini
   win32 addons/fastdl/fastdl_mm.dll
   linux addons/fastdl/fastdl_mm.so
   ```

3. Edit `addons/fastdl/fastdl.ini` to match how players will reach the server. Use `auto_set_downloadurl` to choose whether the plugin or `server.cfg` sets the download URL.
4. Start the server and run `meta list` in the server console. **GoldSrc FastDL** should show **RUN**. Enter `sv_downloadurl` to see the URL advertised to players. Metamod loads the plugin, including on servers that use AMX Mod X. To apply INI changes, reload the plugin through Metamod or restart the server.

Keep your existing `fastdl.ini` when upgrading. The plugin needs to be able to read this file at startup. If the port is in use or a setting is invalid, the plugin logs the startup error and preserves the previous value of `sv_downloadurl`.

## Local, LAN and public access

With `auto_set_downloadurl = true` or `1`, the plugin sets `sv_downloadurl` when the HTTP server is ready. Automatic mode is the default when the setting is omitted, including in older INI files. In this mode, avoid overriding the cvar in `server.cfg` or in map-specific configs.

With `auto_set_downloadurl = false` or `0`, set the URL in `server.cfg`, for example `sv_downloadurl "http://downloads.example.com:8080/"`. The plugin listens on the configured `bind` address and `port`. You retain control of the cvar when the plugin loads or unloads, and you can leave `public_host` blank even when listening on all interfaces.

| Player location | `bind` | `port` | `public_host` | Advertised URL |
| --- | --- | --- | --- | --- |
| On the server machine | `127.0.0.1` | `8080` | blank | `http://127.0.0.1:8080/` |
| On another computer on the LAN | `192.168.1.50` | `8080` | blank | `http://192.168.1.50:8080/` |
| On the internet | `0.0.0.0` | `8080` | `downloads.example.com` | `http://downloads.example.com:8080/` |

Replace the example addresses with your own. Set `bind` to a local IPv4 address, or use `0.0.0.0` to listen on all IPv4 interfaces. In automatic mode, `public_host` takes a hostname or IPv4 address without a scheme, path or port. It is required when `bind = 0.0.0.0`. On a remote client, `127.0.0.1` points to the player's own computer.

For public access, allow inbound **TCP** traffic on the HTTP port in your hosting provider's firewall. If the server is behind a router, forward the external port to the server's LAN IPv4 address and listening port. In automatic mode, set `public_port` if the external port differs from the listening port. For example, if external TCP port 80 forwards to port 8080 on the server, use `port = 8080` and `public_port = 80`. The default `public_port = 0` advertises the value of `port`.

## Files served

Resource URLs are relative to the game directory. For example, `cstrike/maps/de_dust2.bsp` is available at `/maps/de_dust2.bsp`. The `extensions_files` setting in `fastdl.ini` specifies which file extensions the plugin serves from all published locations. The package includes the following default list.

```ini
extensions_files = .bsp, .res, .wad, .mdl, .wav, .mp3, .spr, .txt, .tga, .bmp
```

The list applies to files directly in `cstrike/` and to files anywhere under `maps/`, `models/`, `sound/`, `sprites/`, `overviews/` and `gfx/env/`. Other directories, including `addons/`, are blocked. For example, allowing `.txt` makes both `/notice.txt` and `/maps/event/guide.txt` available if those files exist.

Separate extensions with commas. Each extension starts with a dot followed by ASCII letters or digits. Extension matching is case-insensitive. An empty `extensions_files =` disables all file downloads. To migrate a version 1.4 INI file, replace its `extensions.*` entries with this single setting and reload the plugin or restart the server. The global setting takes precedence. An INI file that uses only the legacy extension settings retains its per-directory permissions.

For precompressed files, the plugin checks the original extension. For example, `map.bsp.bz2` is available when `extensions_files` includes `.bsp`. Keep the uncompressed file for clients that request it, and update both versions together.

Anyone who can reach the HTTP port and knows a permitted file's path can download it. With the default settings, `/server.cfg` returns 404. Adding `.cfg` to `extensions_files` also makes the root `server.cfg` available for download.

URL paths must match the spelling and case of filenames on disk. Names may contain ASCII letters, digits, spaces, periods, underscores, hyphens, parentheses and square brackets. The per-file limit is 512 MiB. Requests for the site root or a directory return 404. The service supports `GET` and `HEAD` and blocks hidden names, directory traversal and links to other files. On Linux, it opens each path component with `openat` and `O_NOFOLLOW`. On Windows, it checks for reparse points and verifies the final path of the open file. Both platforms reject files with multiple hard links.

The client chooses what to download from the resource list sent by the game server. For custom content, use the appropriate precache function or the `maps/<mapname>.res` manifest. The plugin serves individual files from disk.

## Concurrency and caching

With the settings below, 32 players can download at the same time if each has one active HTTP request. Four workers open files and read blocks of up to 64 KiB. A separate network thread sends these blocks using asynchronous I/O. If the first four players have slow connections, their writes can remain pending while the workers read data for the other 28.

```ini
workers = 4
max_connections = 32
```

`workers` accepts 1 to 32 file worker threads. `max_connections` accepts 1 to 256 active HTTP connections and defaults to 32 when omitted from older configurations. Each request occupies a slot from the time the server accepts the connection until the response ends. A player can occupy several slots if their client opens connections in parallel.

When all 32 slots are occupied, the 33rd connection waits in the operating system's TCP listen queue until a slot opens. The listener requests a backlog of 128 pending connections. The actual queue capacity depends on the operating system, and each client determines how long it will wait. A full queue or a timeout can cause a request to fail. Existing transfers continue as new players connect.

Each transfer holds at most one block of file data in memory and requests the next block after sending the current one. With 32 active file transfers, these buffers use up to 2 MiB in total, in addition to HTTP headers, sockets, thread stacks and the operating system's file cache. Clients have 4 seconds to send their request headers, and each write operation has 30 seconds to complete. A download can run longer as long as it keeps making progress. When the plugin unloads, it closes the listening socket and active connections, skips disk work that has yet to start, and waits for in-flight operations to finish and the threads to exit.

During play, the network and file threads handle incoming connections, file reads and data transfers. The service requests a lower CPU priority for these threads. FastDL uses the available bandwidth and shares the disk and network with the game. Saturating the network link can still increase player latency. [Linux TCP listen queue](https://man7.org/linux/man-pages/man2/listen.2.html) · [Boost.Beast I/O timeouts](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_io/timeouts.html)

Repeated downloads benefit from the operating system's file cache, so the 32-bit process does not need to keep its own copy of every popular map. The HTTP service supports byte-range requests for resumable downloads and generates an `ETag` from the open file's metadata. With `cache_seconds = 0`, a client or proxy can revalidate its cached copy and receive `304 Not Modified` without downloading the body again. A positive value, up to 86400, allows the cache to reuse its copy for that many seconds. If you replace a file during that period, a cache may continue serving the previous version until its freshness period expires.

To replace a resource during play, prepare the new file on the same volume and rename it over the old one. An active transfer can finish using the file it already has open, while the next request can open the new version.

## Checking your server

Start the server and make sure the map exists on disk. From a machine that reaches the server the same way a player would, run the following commands using your advertised `sv_downloadurl` as the base URL.

```sh
curl -I http://127.0.0.1:8080/maps/de_dust2.bsp
curl -I http://127.0.0.1:8080/server.cfg
```

In PowerShell, use `curl.exe`. With the default file rules, the map request should return `200` with the file size, and the `server.cfg` request should return `404`. To check the in-game experience, connect with a client that allows downloads and is missing a custom resource listed by the map.

## Building from source

The HTTP service uses C++20 coroutines. The project requires CMake 3.24 or later. CMake fetches pinned versions of the Metamod-R SDK and Boost 1.92.0 headers and verifies the SHA-256 checksum of each archive. The game uses a 32-bit x86 runtime on both platforms. The distributed Windows DLL links the C++ runtime statically and requires Windows 10 or later. The distributed Linux module targets the i386 ABI with glibc.

**Windows** builds require Visual Studio Build Tools with the C++ tools and Windows SDK installed.

```powershell
cmake -S . -B build/win -A Win32
cmake --build build/win --config Release
cmake --install build/win --config Release --prefix dist/windows
```

**Linux** builds require GCC and 32-bit development libraries. On x86-64 Debian or Ubuntu, install `g++-multilib`.

```sh
cmake -S . -B build/linux -DCMAKE_TOOLCHAIN_FILE=cmake/linux-x86.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
cmake --install build/linux --prefix dist/linux
```

To run the optional test suite, make sure Python 3 is available, configure the build with `-DBUILD_TESTING=ON` and rebuild. Run `ctest --test-dir build/win -C Release --output-on-failure` on Windows or `ctest --test-dir build/linux --output-on-failure` on Linux. The suite loads the built module and uses temporary resource files. It checks that 32 downloads can receive headers before the first transfers finish, that a queued connection proceeds when a slot becomes available, and that the plugin can unload with downloads still open.

## Preparing a release

1. Set the release version in the `project(... VERSION ...)` line of `CMakeLists.txt` and push the commit you want to release.
2. In GitHub, open **Actions → Prepare draft release → Run workflow** and select the branch. Alternatively, push a tag matching the CMake version, such as `v1.5.1`, to start the same workflow automatically.
3. When both builds finish, open the draft from the workflow summary or the repository's **Releases** page. Review the generated notes and click **Publish release** when ready.

The workflow builds a Windows x86 DLL and a Linux i386 shared object targeting glibc 2.28. The installation ZIP contains `fastdl_mm.dll`, `fastdl_mm.so` and `fastdl.ini` under `addons/fastdl/`. The release includes `THIRD-PARTY-NOTICES.txt` as a separate attachment. Rerunning the workflow for the same commit and version refreshes the draft's attachments and retains any edits to its notes. Published versions are skipped.

To assemble the same ZIP locally, install both Release builds into one staging directory and run the packaging script. Adjust the build paths and version to match your builds.

```sh
cmake --install build/win --config Release --prefix build/stage
cmake --install build/linux --config Release --prefix build/stage
python scripts/package_release.py build/stage dist/GoldSrc-FastDL-1.5.1-x86.zip
cmake --install build/linux --config Release --component Notices --prefix dist
```

## Project layout and references

- `src/metamod/` handles plugin loading and unloading and manages `sv_downloadurl`.
- `src/http.cpp` handles asynchronous HTTP transfers, connection limits and file workers. `src/http_response.cpp` handles HTTP methods, cache revalidation and byte ranges. `src/assets.cpp` checks whether a file may be served before opening it with platform-specific safeguards.
- `src/config.cpp` reads the INI file. The configuration template is in `config/fastdl.ini`.
- `cmake/` pins the dependencies and configures the target architecture. `tests/` contains the optional integration test suite.

GoldNav served as a reference for the native x86 plugin and its Metamod integration. FastDL uses the SDK interfaces and can set the `sv_downloadurl` cvar that ReHLDS sends to clients to tell them where to download resources. [GoldNav](https://github.com/H0z0n/GoldNav) · [Metamod-R SDK](https://github.com/rehlds/Metamod-R/tree/56590c5c70fa95775051888b7fe0a4e285856ebb/metamod/extra/example) · [Resource delivery in ReHLDS](https://github.com/rehlds/ReHLDS/blob/master/rehlds/engine/sv_main.cpp) · [Boost 1.92.0](https://www.boost.org/releases/1.92.0/)
