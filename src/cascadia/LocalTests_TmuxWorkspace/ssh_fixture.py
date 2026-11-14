"""Local SSH fixture. Requires Paramiko; never edits user SSH files.

Exercises real Windows OpenSSH against simulated remote tmux responses.
This is not a replacement for testing against a real Linux server.
"""
from __future__ import annotations

import argparse
import logging
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import threading
import sys
import time

import paramiko


def main() -> int:
    logging.getLogger("paramiko.transport").setLevel(logging.CRITICAL)
    parser = argparse.ArgumentParser()
    parser.add_argument("test_exe", type=Path)
    parser.add_argument("--work-directory", required=True, type=Path)
    args = parser.parse_args()
    args.work_directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ssh-fixture-", dir=args.work_directory) as temporary:
        root = Path(temporary).resolve()
        assert root.is_relative_to(args.work_directory.resolve())
        host_key = paramiko.RSAKey.generate(2048)
        user_key = paramiko.RSAKey.generate(2048)
        key_path = root / "test_key"
        user_key.write_private_key_file(str(key_path))
        if sys.platform == "win32":
            # Only the ephemeral test key is changed. OpenSSH rejects the
            # workspace's inherited OWNER RIGHTS entry for private keys.
            identity = subprocess.check_output(["whoami"], text=True).strip()
            subprocess.run(
                ["icacls", str(key_path), "/inheritance:r", "/grant:r",
                 identity + ":F", "/remove", "*S-1-3-4"],
                check=True, capture_output=True,
            )
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(12)
        listener.settimeout(0.2)
        port = listener.getsockname()[1]
        known_hosts = root / "known_hosts"
        known_hosts.write_text(f"[127.0.0.1]:{port} {host_key.get_name()} {host_key.get_base64()}\n", encoding="utf-8")
        config = root / "config"
        config.write_text(
            f'Host fixture\n HostName 127.0.0.1\n Port {port}\n User tmux-test\n'
            f' IdentityFile "{key_path.as_posix()}"\n IdentitiesOnly yes\n'
            f' UserKnownHostsFile "{known_hosts.as_posix()}"\n StrictHostKeyChecking yes\n',
            encoding="utf-8",
        )
        sessions = [
            {"id": "$0", "created": 1800000000, "name": "dev", "windows": 2},
            {"id": "$1", "created": 1800000001, "name": "训练", "windows": 1},
        ]
        discovery = root / "discovery"
        (discovery / "extras").mkdir(parents=True)
        (discovery / "config").write_text(
            "Host alpha beta\n User someone\nInclude extras/*.conf\nHost *.internal !blocked\n"
            "Host gamma # comment\nInclude config\n  Host=eta\n  Host = theta\n", encoding="utf-8"
        )
        (discovery / "extras/one.conf").write_text("Host delta alpha\n HostName localhost\n", encoding="utf-8")
        (discovery / "extras/two.conf").write_text('Host "epsilon"\n', encoding="utf-8")
        (discovery / "ssh_config").write_text("Host zeta\n", encoding="utf-8")
        lock = threading.Lock()
        stop = threading.Event()
        transports = []

        def line(session):
            return f"1234\t{session['id']}\t{session['created']}\t{session['windows']}\t0\t{session['name']}\n"

        class Server(paramiko.ServerInterface):
            def __init__(self):
                self.command = None
                self.ready = threading.Event()

            def check_auth_publickey(self, username, key):
                return paramiko.AUTH_SUCCESSFUL if username == "tmux-test" and key == user_key else paramiko.AUTH_FAILED

            def get_allowed_auths(self, username):
                return "publickey"

            def check_channel_request(self, kind, channel_id):
                return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

            def check_channel_exec_request(self, channel, command):
                self.command = command.decode("utf-8")
                wrapper = shlex.split(self.command)
                if len(wrapper) == 3 and wrapper[:2] == ["sh", "-c"]:
                    self.command = wrapper[2]
                self.ready.set()
                return True

        def handle(connection):
            transport = paramiko.Transport(connection)
            transports.append(transport)
            try:
                transport.add_server_key(host_key)
                server = Server()
                transport.start_server(server=server)
                channel = transport.accept(6)
                if channel is None or not server.ready.wait(6):
                    return
                command = server.command
                code, output, error = 0, "", ""
                if command.startswith("command -v tmux "):
                    with lock:
                        output = "".join(line(session) for session in sessions)
                    error = "fixture banner\n"
                elif command.startswith("tmux new-session "):
                    tokens = shlex.split(command)
                    name_index = tokens.index("-s") + 1
                    if len(tokens) != name_index + 1:
                        raise AssertionError("Unquoted data escaped into additional remote arguments")
                    name = tokens[name_index]
                    with lock:
                        if any(session["name"] == name for session in sessions):
                            code, error = 1, "duplicate session\n"
                        else:
                            session = {"id": "$" + str(len(sessions)), "created": 1800000010, "name": name, "windows": 1}
                            sessions.append(session)
                            output = line(session)
                elif command == "fixture:exit-seven":
                    code, error = 7, "expected error\n"
                elif command == "fixture:no-server":
                    code, error = 1, "no server running on /tmp/tmux-test/default\n"
                elif command == "fixture:permission-denied":
                    code, error = 1, "error connecting to /tmp/tmux-test/default (Permission denied)\n"
                elif command == "fixture:flood":
                    output = "x" * (1024 * 1024 + 65536)
                elif command == "fixture:hang":
                    while transport.is_active() and not stop.wait(0.1):
                        pass
                    return
                else:
                    code, error = 99, "unexpected fixture command\n"
                if output:
                    channel.sendall(output.encode("utf-8"))
                if error:
                    channel.sendall_stderr(error.encode("utf-8"))
                channel.send_exit_status(code)
                channel.shutdown_write()
                deadline = time.monotonic() + 3
                while not channel.closed and transport.is_active() and time.monotonic() < deadline:
                    stop.wait(0.01)
                channel.close()
            except (EOFError, OSError, paramiko.SSHException):
                pass
            finally:
                transport.close()

        def serve():
            while not stop.is_set():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                threading.Thread(target=handle, args=(connection,), daemon=True).start()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            result = subprocess.run([str(args.test_exe.resolve()), str(config), str(discovery)], timeout=90)
            return result.returncode
        finally:
            stop.set()
            listener.close()
            for transport in transports:
                transport.close()
            thread.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
