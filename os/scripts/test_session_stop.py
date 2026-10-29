#!/usr/bin/env python3
"""Exercise the real init script with fake commands; never touch running processes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

script = Path(__file__).resolve().parents[1] / "buildroot/board/common/session-juce-x11/etc/init.d/S95oscairn"
with tempfile.TemporaryDirectory(prefix="oscairn-init-test-") as temporary:
    root = Path(temporary)
    command = root / "command"
    command.write_text(f"#!{sys.executable}\n" + '''import os, sys
from pathlib import Path
root = Path(__file__).parent
name = Path(sys.argv[0]).name
with (root / "log").open("a") as log: log.write(name + " " + " ".join(sys.argv[1:]) + "\\n")
if name == "pidof":
    counter = root / "polls"
    n = int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(n+1))
    sys.exit(0 if os.environ["STUCK"] == "1" or n < 2 else 1)
''')
    command.chmod(0o755)
    for name in ("start-stop-daemon", "killall", "pidof", "rm", "sleep"):
        (root / name).symlink_to(command)
    for action, stuck in (("stop", False), ("stop", True), ("restart", True)):
        (root / "log").write_text("")
        (root / "polls").unlink(missing_ok=True)
        result = subprocess.run(["/bin/sh", str(script), action], text=True, capture_output=True,
                                env={**os.environ, "PATH": str(root), "STUCK": str(int(stuck))}, timeout=10)
        commands = (root / "log").read_text().splitlines()
        assert "killall Oscairn oscairn-engine" in commands, commands
        assert any(line.startswith("pidof ") for line in commands), commands
        assert (result.returncode != 0) == stuck, result
        assert ("OK" in result.stdout) != stuck, result.stdout
        assert not any(line.startswith("start-stop-daemon -S") for line in commands), commands
        if not stuck:
            assert commands[-1] == "killall Xorg", commands
print("ok: service stop terminates and waits for detached engine; failed stop prevents restart")
