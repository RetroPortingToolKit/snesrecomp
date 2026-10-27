"""Journalled, bounded debug routes for original-ROM cheat/teleport QA.

Launch/configuration and game-specific WRAM knowledge belong to the caller.
Use a private working directory for the host: screenshot and tier2_dump paths
below are relative to its cwd. This driver never retries an ambiguous command.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import time


class FrameDriver:
    def __init__(self, client, output: Path, *, max_step_frames=60,
                 step_timeout=30):
        if type(max_step_frames) is not int or not 1 <= max_step_frames <= 60:
            raise ValueError("Step batch must be between 1 and 60 frames")
        self.max_step_frames = max_step_frames
        if not 0 < step_timeout <= 60:
            raise ValueError("Step timeout must be above zero and at most 60 seconds")
        self.step_timeout = step_timeout
        self.client = client
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.journal = (self.output / "commands.jsonl").open("w", encoding="utf-8")

    def command(self, command):
        self.journal.write(json.dumps({"command": command}) + "\n")
        self.journal.flush()
        response = self.client.query(command, retry=False)
        self.journal.write(json.dumps({"response": response}) + "\n")
        self.journal.flush()
        if "error" in response or response.get("ok") is False:
            raise RuntimeError(f"{command}: {response}")
        return response

    def wait_started(self, timeout=90):
        """Wait for a running cold boot, then enter bounded frame stepping.

        Some custom hosts open the TCP server before they finish initializing.
        Launch unpaused: ping alone cannot prove that an initial step is safe.
        The starting frame is recorded; callers requiring an exact power-on
        replay should use the host's startup script instead.
        """
        deadline = time.monotonic() + timeout
        while self.command("frame")["frame"] == 0:
            if time.monotonic() >= deadline:
                raise TimeoutError("Host answered TCP but never simulated a frame")
            time.sleep(0.05)
        # The debug server deliberately rejects the separate pause command.
        # A bounded step owns its own stop and is the supported test API.
        self.command("set_controller 0")
        try:
            result = self.command("step 1")
            # The first request races an already-running frame. It may observe
            # that frame plus its requested frame before the stop takes effect.
            # Some hosts publish the frame counter just after acknowledging
            # the stop. Observe that publication without sending another step.
            published_by = time.monotonic() + 2
            while (not result.get("timeout")
                   and result["frame_after"] == result["frame_before"]
                   and time.monotonic() < published_by):
                result["frame_after"] = self.command("frame")["frame"]
            # Subsequent steps start stopped and retain the exact-delta check.
            if result.get("timeout") or result["frame_after"] <= result["frame_before"]:
                raise RuntimeError(f"Guest did not enter bounded stepping: {result}")
        finally:
            self.command("clear_controller")
        return self.command("frame")["frame"]

    def read(self, address, size):
        if not 0 <= address < 0x20000 or not 0 <= size <= 0x20000 - address:
            raise ValueError("WRAM read is outside 128 KiB")
        data = bytes.fromhex(self.command(f"read_ram {address:x} {size}")["hex"])
        if len(data) != size:
            raise RuntimeError("Short WRAM read")
        return data

    def write(self, address, data):
        data = bytes(data)
        if not data or not 0 <= address < 0x20000 or len(data) > 0x20000 - address:
            raise ValueError("WRAM write is empty or outside 128 KiB")
        if self.command(f"write_ram {address:x} {data.hex()}")["count"] != len(data):
            raise RuntimeError("Short WRAM write")

    def byte(self, address, value=None):
        if value is None:
            return self.read(address, 1)[0]
        self.write(address, bytes([value]))

    def word(self, address, value=None):
        if value is None:
            return int.from_bytes(self.read(address, 2), "little")
        self.write(address, int(value).to_bytes(2, "little"))

    def step(self, frames, buttons="0"):
        if type(frames) is not int or frames < 0:
            raise ValueError("Frame count must be a nonnegative integer")
        self.command(f"set_controller {buttons}")
        try:
            while frames:
                count = min(frames, self.max_step_frames)
                result = self.command(f"step {count}")
                expected = result["frame_before"] + count
                # A server deadline is a pending request, not cancellation.
                # Its original step continues running. Only query progress;
                # never resend the mutation or race it with another edit.
                deadline = time.monotonic() + self.step_timeout
                while (result["frame_after"] < expected
                       and time.monotonic() < deadline):
                    time.sleep(0.01)
                    result["frame_after"] = self.command("frame")["frame"]
                if result["frame_after"] != expected:
                    raise RuntimeError(f"Guest did not advance: {result}")
                frames -= count
        finally:
            self.command("clear_controller")

    def until(self, address, expected: bytes, max_frames, stride=15):
        if type(max_frames) is not int or max_frames < 0 or stride < 1:
            raise ValueError("Invalid condition frame bound")
        elapsed = 0
        while self.read(address, len(expected)) != expected:
            if elapsed == max_frames:
                raise TimeoutError(f"WRAM ${address:05X} did not become {expected.hex()} in {max_frames} frames")
            count = min(stride, max_frames - elapsed)
            self.step(count)
            elapsed += count
        return elapsed

    def snapshot(self, name):
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name):
            raise ValueError("Snapshot name must be a plain filename stem")
        self.command(f"screenshot {name}.bmp")
        (self.output / f"{name}.wram").write_bytes(self.read(0, 0x20000))
        data = {command: self.command(command) for command in
                ("frame", "get_v2_cpu", "get_ppu_state", "interp_stats", "offrails_get")}
        (self.output / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n")
        self.command(f"tier2_dump {name}-coverage.json")
        return data

    def close(self):
        try:
            self.command("clear_controller")
        finally:
            self.client.close()
            self.journal.close()
