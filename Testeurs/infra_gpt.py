#!/usr/bin/env python3
import os
import shutil
from pathlib import Path

TARGET = Path("/tmp/www")
SCRIPT_DIR = Path(__file__).resolve().parent
CGI_TESTER = SCRIPT_DIR / "cgi_tester"


def write_file(path, data, mode="w"):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, mode) as f:
        f.write(data)


def main():
    if TARGET.exists():
        shutil.rmtree(str(TARGET))

    dirs = [
        TARGET,
        TARGET / "upload",
        TARGET / "limit",
        TARGET / "auto",
        TARGET / "index_test",
        TARGET / "index_dir",
        TARGET / "cgi-bin",
        TARGET / "port2",
        TARGET / "host2",
        TARGET / "YoupiBanane" / "nop",
        TARGET / "YoupiBanane" / "Yeah",
    ]
    for directory in dirs:
        directory.mkdir(parents=True, exist_ok=True)

    write_file(TARGET / "index.html", "MAIN_INDEX_CONTENT")
    write_file(TARGET / "empty.html", "")
    write_file(TARGET / "custom_404.html", "CUSTOM_404_PAGE_CONTENT")

    write_file(TARGET / "port2" / "index.html", "PORT2_CONTENT")
    write_file(TARGET / "host2" / "index.html", "HOST2_CONTENT")
    write_file(TARGET / "index_dir" / "my_index.html", "INDEX_DIR_CONTENT")

    write_file(TARGET / "auto" / "auto_file.txt", "AUTOINDEX_TEST")
    write_file(TARGET / "auto" / "nested.html", "<h1>nested</h1>")

    write_file(TARGET / "YoupiBanane" / "youpi.bad_extension", "YOUPI_INDEX_CONTENT")
    write_file(TARGET / "YoupiBanane" / "youpi.bla", "CGI_TARGET_FILE")
    write_file(TARGET / "YoupiBanane" / "nop" / "youpi.bad_extension", "YOUPI_NOP_INDEX")
    write_file(TARGET / "YoupiBanane" / "nop" / "other.pouic", "YOUPI_OTHER")
    write_file(TARGET / "YoupiBanane" / "Yeah" / "not_happy.bad_extension", "YOUPI_YEAH_FILE")

    env_cgi = """#!/usr/bin/env python3
import os
import sys

body = ""
if os.environ.get("REQUEST_METHOD") == "POST":
    body = sys.stdin.read()

print("Content-Type: text/plain\\r\\n\\r\\n", end="")
print("METHOD=" + os.environ.get("REQUEST_METHOD", ""))
print("SERVER_PORT=" + os.environ.get("SERVER_PORT", ""))
print("REQUEST_URI=" + os.environ.get("REQUEST_URI", ""))
print("PATH_INFO=" + os.environ.get("PATH_INFO", ""))
print("CONTENT_LENGTH=" + os.environ.get("CONTENT_LENGTH", ""))
if body:
    print("BODY=" + body)
"""
    env_path = TARGET / "cgi-bin" / "env.py"
    write_file(env_path, env_cgi)
    os.chmod(str(env_path), 0o755)

    loop_cgi = """#!/usr/bin/env python3
import time

print("Content-Type: text/plain\\r\\n\\r\\n", end="")
while True:
    time.sleep(1)
"""
    loop_path = TARGET / "cgi-bin" / "infinite_loop.py"
    write_file(loop_path, loop_cgi)
    os.chmod(str(loop_path), 0o755)

    if not CGI_TESTER.exists():
        raise SystemExit("Missing school CGI tester: %s" % CGI_TESTER)

    shutil.copy2(str(CGI_TESTER), str(TARGET / "cgi_test"))
    os.chmod(str(TARGET / "cgi_test"), 0o755)

    print("GPT test infrastructure ready in %s" % TARGET)
    print("Use config: conf/gpt.conf")


if __name__ == "__main__":
    main()
