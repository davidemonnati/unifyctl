"""Subprocess checks: never invoke a valid hardware command."""
import subprocess
import sys
import tempfile

binary = sys.argv[1]
good = [
    ["help"], ["--help"], ["list", "--help"], ["add", "--help"],
    ["remove", "--help"], ["--receiver", "/no/such/device", "list", "--help"],
    ["add", "--timeout", "255", "--help"],
    ["remove", "6", "--yes", "--help"],
]
bad = [
    [], ["wat"], ["list", "extra"], ["--receiver"], ["--wat"], ["remove"],
    ["remove", "0"], ["remove", "7"], ["remove", "-1"], ["remove", "1x"],
    ["remove", "999999999999999999999999"], ["remove", "1", "2"],
    ["add", "--timeout"], ["add", "--timeout", "0"],
    ["add", "--timeout", "256"], ["add", "--timeout", "1.5"],
    ["add", "--timeout", "+1"], ["add", "--timeout", ""],
    ["list", "--timeout", "30"], ["add", "--yes"], ["help", "extra"],
    ["--receiver", "a", "--receiver", "b", "list"],
]
for args in good:
    result = subprocess.run([binary, *args], capture_output=True, text=True)
    assert result.returncode == 0, (args, result)
    assert "Usage:" in result.stdout and "--receiver" in result.stdout
    assert not result.stderr, (args, result.stderr)
for args in bad:
    result = subprocess.run([binary, *args], capture_output=True, text=True)
    assert result.returncode == 2, (args, result)
    assert "Usage:" in result.stderr and not result.stdout
print(f"CLI: {len(good) + len(bad)} cases passed")

if sys.platform.startswith("linux"):
    with tempfile.NamedTemporaryFile() as ordinary:
        for path, expected in [("/dev/null", 3), (ordinary.name, 3),
                               ("/nonexistent-unifyctl-receiver/device", 4)]:
            result = subprocess.run([binary, "--receiver", path, "list"],
                                    capture_output=True, text=True)
            assert result.returncode == expected, (path, result)
            assert not result.stdout
    print("Linux: invalid receiver path checks passed")
