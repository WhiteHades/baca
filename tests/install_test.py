#!/usr/bin/env python3

import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import shutil
import struct
import sys
import termios
import time
import zipfile


TIMEOUT_SECONDS = 15.0
MAX_OUTPUT_BYTES = 8 * 1024 * 1024


class TestFailure(Exception):
    pass


class PtyProcess:
    def __init__(self, binary: Path, arguments: list[str], environment: dict[str, str], cwd: Path):
        self.output = bytearray()
        self.status: int | None = None
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            os.execve(binary, [binary.name, *arguments], environment)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", 16, 80, 0, 0))
        os.set_blocking(self.master, False)

    def _drain(self) -> None:
        while True:
            try:
                chunk = os.read(self.master, 65536)
            except BlockingIOError:
                return
            except OSError as error:
                if error.errno == errno.EIO:
                    return
                raise
            if not chunk:
                return
            self.output.extend(chunk)
            if len(self.output) > MAX_OUTPUT_BYTES:
                raise TestFailure("PTY output exceeded 8 MiB")

    def _poll_exit(self) -> bool:
        if self.status is not None:
            return True
        waited, status = os.waitpid(self.pid, os.WNOHANG)
        if waited == self.pid:
            self.status = status
            return True
        return False

    def wait_for(self, marker: bytes, start: int = 0) -> None:
        deadline = time.monotonic() + TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            select.select([self.master], [], [], 0.05)
            self._drain()
            if marker in self.output[start:]:
                return
            if self._poll_exit():
                break
        raise TestFailure(f"did not observe {marker!r}")

    def send(self, data: bytes) -> None:
        written = 0
        while written < len(data):
            written += os.write(self.master, data[written:])

    def wait_exit(self) -> None:
        deadline = time.monotonic() + TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            select.select([self.master], [], [], 0.05)
            self._drain()
            if self._poll_exit():
                self._drain()
                if os.waitstatus_to_exitcode(self.status) != 0:
                    raise TestFailure(f"process exited with {os.waitstatus_to_exitcode(self.status)}")
                return
        raise TestFailure("process did not exit")

    def close(self) -> None:
        if self.status is None:
            try:
                os.kill(self.pid, 9)
            except ProcessLookupError:
                pass
            try:
                _, self.status = os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
        os.close(self.master)


def create_epub(path: Path) -> None:
    mimetype = b"application/epub+zip"
    container = (
        b'<?xml version="1.0"?>'
        b'<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        b'<rootfiles><rootfile full-path="OEBPS/content.opf" '
        b'media-type="application/oebps-package+xml"/></rootfiles></container>'
    )
    package = (
        b'<?xml version="1.0"?>'
        b'<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">'
        b'<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        b'<dc:title>Installed QA</dc:title><dc:creator>Release Test</dc:creator>'
        b'<dc:identifier id="id">installed-qa</dc:identifier></metadata><manifest>'
        b'<item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
        b'<item id="pixel" href="pixel.png" media-type="image/png"/>'
        b'</manifest><spine><itemref idref="chapter"/></spine></package>'
    )
    chapter = (
        b'<html xmlns="http://www.w3.org/1999/xhtml"><body>'
        b'<img src="pixel.png" alt="installed image"/>'
        b"<p>INSTALLED EPUB BODY</p></body></html>"
    )
    pixel = bytes.fromhex(
        "89504e470d0a1a0a0000000d4948445200000001000000010804000000b51c0c02"
        "0000000b4944415478da6364f80f00010501012718e3660000000049454e44ae426082"
    )
    path.parent.mkdir(parents=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("mimetype", mimetype, compress_type=zipfile.ZIP_STORED)
        archive.writestr("META-INF/container.xml", container)
        archive.writestr("OEBPS/content.opf", package)
        archive.writestr("OEBPS/chapter.xhtml", chapter)
        archive.writestr("OEBPS/pixel.png", pixel)


def test_environment(root: Path, **updates: str) -> dict[str, str]:
    environment = os.environ.copy()
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("LD_PRELOAD", None)
    environment.pop("TMUX", None)
    environment.pop("STY", None)
    environment.pop("COLORTERM", None)
    environment.pop("TERM_PROGRAM", None)
    environment.pop("KITTY_WINDOW_ID", None)
    environment["HOME"] = str(root / "home")
    environment["XDG_CONFIG_HOME"] = str(root / "config")
    environment["XDG_CACHE_HOME"] = str(root / "cache")
    environment["TMPDIR"] = str(root / "temporary")
    environment["LANG"] = "C.UTF-8"
    environment["LC_ALL"] = "C.UTF-8"
    environment.update(updates)
    for name in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "TMPDIR"):
        Path(environment[name]).mkdir(parents=True, exist_ok=True)
    return environment


def assert_installed_library_loaded(process: PtyProcess, binary: Path) -> None:
    expected = (binary.parent.parent / "lib/mereader-tui/libfff_c.so").resolve()
    maps = Path(f"/proc/{process.pid}/maps").read_text(encoding="utf-8")
    loaded = {
        Path(line.split()[-1]).resolve()
        for line in maps.splitlines()
        if line.endswith("libfff_c.so")
    }
    if loaded != {expected}:
        raise TestFailure(f"expected installed {expected}, loaded {sorted(map(str, loaded))}")


def run_reader(binary: Path, root: Path, epub: Path, kitty: bool) -> None:
    environment = test_environment(
        root,
        TERM="xterm-256color" if kitty else "vt100",
        **({"COLORTERM": "truecolor", "TERM_PROGRAM": "kitty", "KITTY_WINDOW_ID": "1"} if kitty else {}),
    )
    process = PtyProcess(binary, [str(epub)], environment, root)
    try:
        if kitty:
            process.wait_for(b"\x1b_G")
        else:
            process.wait_for(b"INSTALLED EPUB BODY")
        process.send(b"q")
        process.wait_exit()
        if kitty and b"d=I,i=" not in process.output:
            raise TestFailure("Kitty image cleanup was not emitted")
        if not kitty and (b"IMAGE" not in process.output or b"\x1b_G" in process.output):
            raise TestFailure("non-Kitty image fallback was not rendered")
    finally:
        process.close()


def run_library(binary: Path, root: Path, library: Path) -> None:
    environment = test_environment(root, TERM="xterm-256color", MEREADER_TUI_LIBRARY_PATH=str(library))
    process = PtyProcess(binary, [], environment, root)
    try:
        process.wait_for(b"installed-library")
        assert_installed_library_loaded(process, binary)
        process.send(b"\n")
        process.wait_for(b"INSTALLED LIBRARY BODY")
        checkpoint = len(process.output)
        process.send(b"q")
        process.wait_for(b"installed-library", checkpoint)
        process.send(b"q")
        process.wait_exit()
    except TestFailure as error:
        raise TestFailure(f"{error}; PTY output: {bytes(process.output[-2000:])!r}") from error
    finally:
        process.close()


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {Path(sys.argv[0]).name} BINARY WORK_ROOT", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    content = root / "content"
    library = root / "library"
    epub = content / "installed-qa.epub"
    create_epub(epub)
    library.mkdir(parents=True)
    (library / "installed-library.txt").write_text("INSTALLED LIBRARY BODY\n", encoding="utf-8")
    try:
        run_reader(binary, root / "fallback", epub, kitty=False)
        print("PASS install.reader_fallback")
        run_reader(binary, root / "kitty", epub, kitty=True)
        print("PASS install.reader_kitty")
        run_library(binary, root / "library-run", library)
        print("PASS install.library_and_rpath")
    except (OSError, TestFailure) as error:
        print(f"FAIL install: {error}", file=sys.stderr)
        return 1
    print("SUMMARY install passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
