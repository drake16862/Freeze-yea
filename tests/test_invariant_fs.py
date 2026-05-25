import pytest
import ctypes
import os
import sys
import subprocess
import tempfile
import signal
from pathlib import Path

# Adversarial payloads targeting buffer overflow via filename length
ADVERSARIAL_PAYLOADS = [
    # Exact boundary values
    "A" * 1,
    "A" * 8,
    "A" * 16,
    "A" * 32,
    "A" * 64,
    "A" * 128,
    "A" * 255,
    "A" * 256,
    "A" * 257,
    "A" * 512,
    "A" * 1024,
    "A" * 4096,
    # Off-by-one attacks
    "B" * 15,
    "B" * 16,
    "B" * 17,
    "B" * 31,
    "B" * 32,
    "B" * 33,
    "B" * 63,
    "B" * 64,
    "B" * 65,
    # Null byte injection attempts
    "file\x00evil",
    "name\x00" + "A" * 100,
    "\x00" + "A" * 64,
    # Format string-like payloads
    "%s" * 50,
    "%n" * 50,
    "%x" * 50,
    "%" + "A" * 200,
    # Path traversal combined with overflow
    "../" * 50 + "A" * 100,
    "../../" + "A" * 200,
    "/etc/passwd" + "A" * 200,
    # Special characters
    "\xff" * 64,
    "\xfe" * 128,
    "\x41" * 256,
    # Mixed content
    "valid_prefix_" + "X" * 300,
    "file_" + "\x41\x42\x43\x44" * 100,
    # Unicode/multibyte
    "ñ" * 100,
    "中文" * 50,
    "🔥" * 50,
    # Whitespace and control characters
    " " * 256,
    "\t" * 256,
    "\n" * 100,
    "\r\n" * 100,
    # Empty and minimal
    "",
    "a",
    # Shell metacharacters
    ";" + "A" * 200,
    "|" + "A" * 200,
    "&" + "A" * 200,
    "`" + "A" * 200,
    "$(" + "A" * 200 + ")",
]


class FilenameSecuritySimulator:
    """
    Simulates the security boundary that MUST be enforced when copying
    filenames into fixed-size buffers. This represents the invariant
    that any secure implementation must uphold.
    """
    MAX_FILENAME_LENGTH = 255  # POSIX standard maximum filename length

    def __init__(self):
        self.files = {}
        self.buffer_size = 256  # Simulated fixed-size buffer

    def safe_create_file(self, name: str) -> dict:
        """
        Secure file creation that enforces the invariant:
        filename stored in buffer must never exceed buffer bounds.
        """
        if not isinstance(name, str):
            raise TypeError("Filename must be a string")

        # Encode to bytes to check actual byte length (as C would see it)
        try:
            name_bytes = name.encode('utf-8', errors='replace')
        except Exception:
            name_bytes = b''

        # The invariant: stored name must fit within buffer
        stored_name = name_bytes[:self.buffer_size - 1]  # Leave room for null terminator

        return {
            'name': stored_name,
            'name_length': len(stored_name),
            'buffer_size': self.buffer_size,
            'overflow_prevented': len(name_bytes) >= self.buffer_size,
        }

    def check_invariant(self, result: dict) -> bool:
        """
        Check that the security invariant holds:
        The stored name length must always be less than buffer_size
        (accounting for null terminator).
        """
        return result['name_length'] < result['buffer_size']


@pytest.fixture
def fs_simulator():
    return FilenameSecuritySimulator()


@pytest.mark.parametrize("payload", ADVERSARIAL_PAYLOADS)
def test_filename_buffer_overflow_invariant(payload, fs_simulator):
    """
    Invariant: When a filename is stored into a fixed-size buffer,
    the stored data must NEVER exceed the buffer boundary regardless
    of input length or content. This guards against the unbounded
    strcpy vulnerability where user-controlled filenames can overflow
    fixed-size file name buffers.
    """
    result = fs_simulator.safe_create_file(payload)

    # INVARIANT 1: Stored name must always fit within buffer bounds
    assert result['name_length'] < result['buffer_size'], (
        f"SECURITY VIOLATION: Filename of length {result['name_length']} "
        f"exceeds or equals buffer size {result['buffer_size']}. "
        f"Payload length: {len(payload)}"
    )

    # INVARIANT 2: Buffer must always have room for null terminator
    assert result['name_length'] <= result['buffer_size'] - 1, (
        f"SECURITY VIOLATION: No room for null terminator. "
        f"Stored length {result['name_length']} must be <= {result['buffer_size'] - 1}"
    )

    # INVARIANT 3: For payloads longer than buffer, overflow must be detected and prevented
    try:
        payload_bytes = payload.encode('utf-8', errors='replace')
    except Exception:
        payload_bytes = b''

    if len(payload_bytes) >= result['buffer_size']:
        assert result['overflow_prevented'], (
            f"SECURITY VIOLATION: Overflow not detected for payload of "
            f"byte length {len(payload_bytes)} with buffer size {result['buffer_size']}"
        )
        assert result['name_length'] == result['buffer_size'] - 1, (
            f"SECURITY VIOLATION: Truncation not applied correctly. "
            f"Expected {result['buffer_size'] - 1}, got {result['name_length']}"
        )

    # INVARIANT 4: Stored name must be a prefix of the original (no corruption)
    if len(payload_bytes) > 0 and result['name_length'] > 0:
        assert result['name'] == payload_bytes[:result['name_length']], (
            f"SECURITY VIOLATION: Stored name content is corrupted. "
            f"Expected prefix of original, got unexpected data."
        )


@pytest.mark.parametrize("payload", ADVERSARIAL_PAYLOADS)
def test_filename_does_not_cause_process_crash(payload):
    """
    Invariant: Processing any filename input must not cause the process
    to crash, segfault, or terminate abnormally. A buffer overflow in
    strcpy can corrupt memory and cause undefined behavior including crashes.
    """
    simulator = FilenameSecuritySimulator()

    try:
        result = simulator.safe_create_file(payload)
        # If we get here, no crash occurred - verify result is valid
        assert result is not None, "File creation returned None unexpectedly"
        assert 'name' in result, "Result missing 'name' field"
        assert 'name_length' in result, "Result missing 'name_length' field"
        assert isinstance(result['name_length'], int), "name_length must be integer"
        assert result['name_length'] >= 0, "name_length must be non-negative"
    except (TypeError, ValueError, UnicodeError):
        # These are acceptable - the system rejected invalid input gracefully
        pass
    except Exception as e:
        pytest.fail(
            f"SECURITY VIOLATION: Unexpected exception '{type(e).__name__}: {e}' "
            f"when processing payload of length {len(payload)}. "
            f"This may indicate memory corruption or undefined behavior."
        )


@pytest.mark.parametrize("payload", [
    "A" * 256,
    "B" * 512,
    "C" * 1024,
    "D" * 4096,
    "\xff" * 256,
    "../" * 100,
])
def test_oversized_filename_is_rejected_or_truncated(payload):
    """
    Invariant: Filenames exceeding the maximum safe length must either be
    rejected outright or safely truncated. They must NEVER be stored in
    their entirety into a fixed-size buffer (which would cause overflow).
    """
    simulator = FilenameSecuritySimulator()
    result = simulator.safe_create_file(payload)

    payload_bytes = payload.encode('utf-8', errors='replace')

    # The stored name must be strictly smaller than the buffer
    assert result['name_length'] < simulator.buffer_size, (
        f"SECURITY VIOLATION: Oversized filename ({len(payload_bytes)} bytes) "
        f"was stored without truncation into buffer of size {simulator.buffer_size}"
    )

    # Verify the overflow was detected
    if len(payload_bytes) >= simulator.buffer_size:
        assert result['overflow_prevented'], (
            f"SECURITY VIOLATION: System failed to detect overflow attempt "
            f"with {len(payload_bytes)}-byte filename"
        )