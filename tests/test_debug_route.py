import socket
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from sneslib.client import DebugClient
from sneslib.route import FrameDriver


def test_lost_mutation_reply_is_not_replayed():
    received = []
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(0.25)

    def server():
        try:
            while True:
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    break
                with connection:
                    received.append(connection.recv(100).decode())
                    # Simulate executing the mutation, then losing its reply.
        finally:
            listener.close()

    thread = threading.Thread(target=server)
    thread.start()
    client = DebugClient(listener.getsockname()[1], timeout=1)
    try:
        with pytest.raises(ConnectionError):
            client.query("step 60", retry=False)
    finally:
        client.close()
        thread.join(timeout=2)
    assert not thread.is_alive()
    assert received == ["step 60\n"]


class Host:
    def __init__(self, *, timeout=False):
        self.frame = 7
        self.calls = []
        self.timeout = timeout

    def query(self, command, *, retry):
        assert retry is False
        self.calls.append(command)
        if command.startswith("step "):
            before = self.frame
            count = int(command.split()[1])
            self.frame += count if not self.timeout else count - 1
            return dict(ok=True, frame_before=before, frame_after=self.frame, timeout=self.timeout)
        if command.startswith("read_ram "):
            return {"hex": "01"}
        if command == "frame":
            return {"frame": self.frame}
        return {"ok": True}

    def close(self):
        pass


def test_step_verifies_progress_and_always_releases_controller(tmp_path):
    host = Host()
    driver = FrameDriver(host, tmp_path, step_timeout=0.02)
    try:
        driver.step(125, "right+y")
        assert host.frame == 132
        assert host.calls == ["set_controller right+y", "step 60", "step 60", "step 5", "clear_controller"]
        host.timeout = True
        with pytest.raises(RuntimeError, match="did not advance"):
            driver.step(12, "b")
        assert host.calls[-1] == "clear_controller"
    finally:
        driver.close()


def test_condition_is_bounded_and_invalid_edits_never_reach_host(tmp_path):
    host = Host()
    driver = FrameDriver(host, tmp_path)
    try:
        with pytest.raises(TimeoutError):
            driver.until(0x118, b"\x0f", 20)
        assert host.frame == 27
        assert [c for c in host.calls if c.startswith("step")] == ["step 15", "step 5"]
        before = list(host.calls)
        for address, data in ((0x20000, b"x"), (0x1ffff, b"xx"), (-1, b"x")):
            with pytest.raises(ValueError):
                driver.write(address, data)
        with pytest.raises(ValueError):
            driver.snapshot("../owner-save")
        assert host.calls == before
        with pytest.raises(RuntimeError, match="Short WRAM read"):
            driver.read(0, 2)
    finally:
        driver.close()


def test_heavy_renderers_can_use_smaller_bounded_steps(tmp_path):
    host = Host()
    with pytest.raises(ValueError):
        FrameDriver(host, tmp_path, max_step_frames=0)
    driver = FrameDriver(host, tmp_path, max_step_frames=5)
    try:
        driver.step(12)
        assert [c for c in host.calls if c.startswith("step")] == ["step 5", "step 5", "step 2"]
    finally:
        driver.close()


def test_startup_handoff_accepts_an_inflight_frame_but_later_steps_are_exact(tmp_path):
    class StartingHost(Host):
        def query(self, command, *, retry):
            if command == "frame":
                return {"frame": self.frame}
            if command == "step 1" and not self.calls:
                raise AssertionError("Controller should be cleared before stepping")
            result = super().query(command, retry=retry)
            if command == "step 1":
                self.frame += 1
                result["frame_after"] += 1
            return result
    host = StartingHost()
    driver = FrameDriver(host, tmp_path)
    try:
        assert driver.wait_started() == 9
        driver.step(2)
        assert host.frame == 11
    finally:
        driver.close()


def test_startup_waits_for_frame_publication_without_repeating_step(tmp_path):
    class PublishingHost(Host):
        def query(self, command, *, retry):
            if command == "frame":
                return {"frame": self.frame}
            result = super().query(command, retry=retry)
            if command.startswith("step "):
                result["frame_after"] -= 1
            return result
    host = PublishingHost()
    driver = FrameDriver(host, tmp_path)
    try:
        assert driver.wait_started() == 8
        assert host.calls.count("step 1") == 1
    finally:
        driver.close()


def test_server_timeout_observes_the_original_pending_step(tmp_path):
    class PendingHost(Host):
        def query(self, command, *, retry):
            if command == "frame":
                self.frame += 1
                return {"frame": self.frame}
            return super().query(command, retry=retry)
    host = PendingHost(timeout=True)
    driver = FrameDriver(host, tmp_path)
    try:
        driver.step(3)
        assert host.frame == 10
        assert host.calls.count("step 3") == 1
        assert host.calls[-1] == "clear_controller"
    finally:
        driver.close()
