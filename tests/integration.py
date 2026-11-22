"""Integration checks against the loadable x86 module, using loopback only."""
import concurrent.futures
from contextlib import ExitStack
import http.client
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import threading


def check_capacity_and_unload(port, slots, stop_server):
    # Each real GET receives its headers. Small receive windows keep the large
    # responses in flight until we explicitly release a reader.
    with ExitStack() as stack:
        readers = []
        for _ in range(slots):
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            stack.callback(conn.close)
            conn.sock = socket.socket()
            conn.sock.settimeout(5)
            conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
            conn.sock.connect((conn.host, port))
            conn.request("GET", "/maps/large.bsp")
            response = stack.enter_context(conn.getresponse())
            assert response.status == 200 and response.read(1) == b"\x00"
            readers.append(response)

        next_conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        stack.callback(next_conn.close)
        next_conn.request("GET", "/texture.wad")
        readable, _, _ = select.select([next_conn.sock], [], [], 0.2)
        assert not readable, "the excess request received data while every slot was occupied"
        readers.pop().close()
        with next_conn.getresponse() as next_response:
            assert next_response.status == 200 and next_response.read() == b"wad"
        stop_server()  # unload while the remaining readers still have open sockets


def run(host, module, work):
    root = work / "cstrike"
    config = root / "addons/fastdl/fastdl.ini"
    config.parent.mkdir(parents=True)
    sample = root / "maps/test map.bsp"
    sample.parent.mkdir()
    data = bytes(range(256)) * 4096
    sample.write_bytes(data)
    (root / "server.cfg").write_bytes(b"private")
    (root / "maps/config.cfg.bz2").write_bytes(b"private")
    (root / "maps/test map.bsp.bz2").write_bytes(b"compressed fixture")
    (root / "maps/empty.bsp").touch()
    for name in ("maps/event/guide.TXT", "maps/event/guide.TXT.bz2", "sound/event.wav",
                 "gfx/env/sky.png", "gfx/other/sky.png", "notice.txt", "addons/notice.txt"):
        fixture = root / name
        fixture.parent.mkdir(parents=True, exist_ok=True)
        fixture.write_bytes(b"fixture")
    with (root / "maps/large.bsp").open("wb") as large:
        large.truncate(64 * 1024 * 1024)
    (root / "texture.wad").write_bytes(b"wad")
    outside = work / "outside"
    outside.mkdir()
    (outside / "private.bsp").write_bytes(b"private")
    os.link(outside / "private.bsp", root / "maps/hard.bsp")
    alias = root / "maps/link"
    if os.name == "nt":
        subprocess.run(["cmd", "/c", "mklink", "/j", str(alias), str(outside)],
                       check=True, stdout=subprocess.DEVNULL)
    else:
        alias.symlink_to(outside, target_is_directory=True)

    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    settings = f"port={port}\nworkers=4\npublic_host=fastdl.example\npublic_port=12345\n"
    command = [host, module, str(root)]

    def request(path, method="GET", headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            conn.request(method, path, headers=headers or {})
            res = conn.getresponse()
            return res.status, dict(res.getheaders()), res.read()
        finally:
            conn.close()

    def check_failure():
        proc = subprocess.run(command, input=b"q", capture_output=True, timeout=8)
        assert proc.returncode == 1 and not proc.stdout, (proc.returncode, proc.stderr)

    check_failure()  # missing configuration
    for bad in ["workers=0", "max_connections=0", "max_connections=257",
                "bind=0.0.0.0", "port=8080junk", "unknown=3", "port=4\nport=5",
                "auto_set_downloadurl=yes", "auto_set_downloadurl=2",
                "extensions.maps=*.bsp", "extensions.maps=.bsp,", "extensions.sound=wav",
                "extensions.addons=.txt", "extensions.maps=.BZ2", "extensions_files=*.bsp",
                "extensions_files=.wav,", "extensions_files=.BZ2"]:
        config.write_text(bad)
        check_failure()
    for mode, stop in [(None, b"q"), ("true", b"s"), ("1", b"m"), ("false", b"s"), ("0", b"m")]:
        options = settings if mode is None else settings + f"auto_set_downloadurl={mode}\n"
        if mode == "true":
            options += "max_connections=2\n"
        if mode == "1":
            options += ("extensions.maps=.TXT\nextensions.root=.wad, .txt\n"
                        "extensions.sound=\nextensions.gfx/env=.png\n")
        if mode == "false":
            options += "extensions_files=.TXT, .png, .wad\nextensions.maps=.bsp\n"
        if mode == "0":
            options += "extensions_files=\n"
        config.write_text(options)
        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            manual = mode in ("false", "0")
            expected = b"http://previous.invalid/" if manual else b"http://fastdl.example:12345/"
            assert proc.stdout.readline().strip() == expected
            if mode == "false":
                assert request("/texture.wad")[::2] == (200, b"wad")
            if mode == "0":
                for path in ("/texture.wad", "/maps/test%20map.bsp", "/maps/test%20map.bsp.bz2"):
                    assert request(path)[0] == 404, path
            if mode in ("1", "false"):
                for path in ("/maps/event/guide.TXT", "/maps/event/guide.TXT.bz2",
                             "/gfx/env/sky.png", "/notice.txt"):
                    assert request(path)[::2] == (200, b"fixture"), path
                for path in ("/maps/test%20map.bsp", "/maps/test%20map.bsp.bz2",
                             "/sound/event.wav", "/gfx/other/sky.png", "/addons/notice.txt", "/server.cfg"):
                    assert request(path, "HEAD")[0] == 404, path
            if stop == b"q":
                for path in ("/notice.txt", "/maps/event/guide.TXT"):
                    assert request(path)[::2] == (200, b"fixture"), path
                check_failure()  # failed bind must preserve the previous cvar
                url = "/maps/test%20map.bsp"
                status, headers, body = request(url)
                assert status == 200 and body == data
                tag = headers["ETag"]
                assert request(url, "HEAD")[1]["Content-Length"] == str(len(data))
                assert request(url, "HEAD")[2] == b""
                assert request(url, headers={"If-None-Match": tag})[::2] == (304, b"")
                assert request(url, headers={"Range": "bytes=123-4097"})[::2] == (206, data[123:4098])
                assert request(url, headers={"Range": "bytes=0-2", "If-Range": tag})[::2] == (200, data)
                assert request(url, headers={"Range": f"bytes={len(data)}-"})[0] == 416
                assert request(url, "POST")[0] == 405
                assert request("/texture.wad")[2] == b"wad"
                assert request(url + ".bz2")[2] == b"compressed fixture"
                assert request("/maps/empty.bsp")[::2] == (200, b"")
                for path in ("/", "/maps/", "/server.cfg", "/addons/fastdl/fastdl.ini",
                             "/maps/config.cfg.bz2", "/maps/%2e%2e/texture.wad",
                             "/maps/hard.bsp", "/maps/link/private.bsp", "/maps/NUL.bsp"):
                    assert request(path)[0] == 404, path
                # A map change can bring a full server to the same resource at once.
                players = 32
                start = threading.Barrier(players, timeout=10)

                def join_player():
                    start.wait()
                    status, _, body = request(url)
                    assert status == 200 and body == data

                with concurrent.futures.ThreadPoolExecutor(players) as pool:
                    downloads = [pool.submit(join_player) for _ in range(players)]
                    for download in downloads:
                        download.result()
                new = sample.with_suffix(".new")
                new.write_bytes(data[::-1])
                os.replace(new, sample)
                assert request(url, headers={"If-None-Match": tag})[::2] == (200, data[::-1])
            def stop_server():
                proc.communicate(stop, timeout=10)
                assert proc.returncode == 0, proc.returncode

            if mode is None or mode == "true":
                check_capacity_and_unload(port, 32 if mode is None else 2, stop_server)
            else:
                stop_server()
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        run(*sys.argv[1:], Path(directory))
