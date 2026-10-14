"""Minimal PINE client for PCSX2 (read/write emulated EE memory over IPC).

Protocol (pcsx2/PINE.cpp, PCSX2 v2.x; on Windows a TCP socket on 127.0.0.1:<slot>):

* request  = u32 LE total length (including these 4 bytes) + one or more commands back to back
* command  = u8 opcode + arguments; MsgRead8..64 (0..3) take a u32 address,
  MsgWrite8..64 (4..7) take a u32 address + the value (1/2/4/8 bytes, LE)
* reply    = u32 LE total length + u8 result (0x00 OK / 0xFF FAIL) + concatenated results
* addresses are EE *virtual* addresses, translated by PCSX2's vtlb on the PINE server thread.
  CAUTION (PCSX2 v2.6.3, verified): reads go through the full vtlb - an address in a page that is not
  mapped yet (e.g. kuseg 0x0010_0000 before the BIOS kernel has written its TLB entries) takes the
  TLB-miss path ("TLB Miss, pc=... addr=..." in the log; with the EE *interpreter* PCSX2 then raises a
  real TLB-miss exception in the guest, from the wrong thread), and I/O-register pages call their
  handlers (side effects).  For EE main RAM use the kseg0 alias 0x8000_0000 | phys, which PCSX2 maps
  statically at reset - see ee_ram_alias(); Pcsx2Session does this by default.  (Newer PCSX2 serve
  PINE via vtlb_ramRead, where handler pages read as 0; the alias is harmless there.)
* the server answers one request at a time per connection and buffers at most
  MAX_IPC_SIZE request bytes / MAX_IPC_RETURN_SIZE reply bytes - an oversize request gets *no reply*,
  and a batch that would overflow the reply buffer fails as a whole, so batches are capped well below both.
* commands fail (0xFF) while no VM exists (before the boot / after shutdown).  In v2.6.3 that includes
  MsgVersion; only MsgStatus answers without a VM.
"""
from __future__ import annotations

import socket
import struct
import time
from enum import IntEnum

__all__ = ["PineClient", "PineError", "PineCommandError", "EmuStatus", "Op", "ee_ram_alias", "EE_RAM_SIZE"]

MAX_IPC_SIZE = 650000          # server request buffer, bytes (including the 4-byte length)
MAX_IPC_RETURN_SIZE = 450000   # server reply buffer, bytes (including the 5-byte header)

EE_RAM_SIZE = 32 * 1024 * 1024
# virtual windows onto EE main RAM: kuseg (cached), the kernel's uncached / uncached-accelerated
# mirrors, kseg0, kseg1.  Only kseg0/kseg1 are mapped by PCSX2 itself; the others need the guest TLB.
_RAM_WINDOWS = (0x00000000, 0x20000000, 0x30000000, 0x80000000, 0xA0000000)


def ee_ram_alias(addr: int, nbytes: int = 1) -> int:
    """kseg0 alias (0x8000_0000 | phys) of an EE main-RAM address given in any of the usual windows.

    Raises ValueError if [addr, addr+nbytes) is not entirely inside one 32 MiB RAM window."""
    addr &= 0xFFFFFFFF
    for base in _RAM_WINDOWS:
        if base <= addr and addr + max(1, nbytes) <= base + EE_RAM_SIZE:
            return 0x80000000 | (addr - base)
    raise ValueError(f"0x{addr:08X}+{nbytes:#x} is not in EE main RAM; use the raw PineClient for other regions")


class Op(IntEnum):
    READ8 = 0
    READ16 = 1
    READ32 = 2
    READ64 = 3
    WRITE8 = 4
    WRITE16 = 5
    WRITE32 = 6
    WRITE64 = 7
    VERSION = 8
    SAVE_STATE = 9
    LOAD_STATE = 0xA
    TITLE = 0xB
    ID = 0xC
    UUID = 0xD
    GAME_VERSION = 0xE
    STATUS = 0xF


class EmuStatus(IntEnum):
    RUNNING = 0
    PAUSED = 1
    SHUTDOWN = 2


class PineError(RuntimeError):
    """Transport-level problem (cannot connect, connection lost, malformed reply)."""


class PineCommandError(PineError):
    """The server answered IPC_FAIL (e.g. no VM running yet / VM shut down)."""


_READ_OPS = {1: Op.READ8, 2: Op.READ16, 4: Op.READ32, 8: Op.READ64}
_WRITE_OPS = {1: Op.WRITE8, 2: Op.WRITE16, 4: Op.WRITE32, 8: Op.WRITE64}
_FMT = {1: "<B", 2: "<H", 4: "<I", 8: "<Q"}


def _split_aligned(addr: int, nbytes: int):
    """Split [addr, addr+nbytes) into naturally aligned 8/4/2/1-byte accesses (never crossing a page)."""
    out = []
    end = addr + nbytes
    while addr < end:
        rem = end - addr
        for size in (8, 4, 2, 1):
            if rem >= size and addr % size == 0:
                out.append((addr, size))
                addr += size
                break
    return out


class PineClient:
    """One persistent PINE connection.  Raw addresses (see the module notes); not thread-safe.

    >>> with PineClient(28911).connect(retry_timeout=30) as pine:
    ...     pine.read32(ee_ram_alias(0x00100000))   # EE RAM via kseg0, never a TLB miss
    """

    DEFAULT_SLOT = 28911          # not PCSX2's default (28011), so it never collides with the user's PCSX2
    MAX_BATCH_REPLY = 256 * 1024  # data bytes per batched read (server limit ~450 KB)
    MAX_BATCH_REQUEST = 256 * 1024  # request bytes per batched write (server limit ~650 KB)

    def __init__(self, slot: int = DEFAULT_SLOT, host: str = "127.0.0.1", timeout: float = 5.0):
        self.slot = int(slot)
        self.host = host
        self.timeout = float(timeout)
        self._sock: socket.socket | None = None

    # - connection ------------------------------------------------------------------------------
    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self, retry_timeout: float = 0.0, retry_interval: float = 0.05,
                attempt_timeout: float = 0.25) -> "PineClient":
        """Connect, retrying until `retry_timeout` seconds have passed (0 = single attempt).

        Each attempt is capped at `attempt_timeout`: on Windows a connect to a loopback port nobody
        listens on is not refused immediately (the SYN is retried for ~1 s), while a listening PINE
        server accepts within microseconds."""
        self.close()
        deadline = time.monotonic() + max(0.0, retry_timeout)
        last_err: Exception | None = None
        while True:
            try:
                sock = socket.create_connection((self.host, self.slot), timeout=min(self.timeout, attempt_timeout))
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.settimeout(self.timeout)
                self._sock = sock
                return self
            except OSError as e:  # refused / timed out / unreachable
                last_err = e
            if time.monotonic() >= deadline:
                raise PineError(f"cannot connect to PINE at {self.host}:{self.slot}: {last_err}") from last_err
            time.sleep(retry_interval)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def __enter__(self) -> "PineClient":
        if not self.connected:
            self.connect()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # - transport -------------------------------------------------------------------------------
    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise PineError("PINE connection closed by PCSX2")
            buf += chunk
        return bytes(buf)

    def transact(self, payload: bytes) -> bytes:
        """Send one request (commands only, the length prefix is added) and return the reply data
        (after the status byte).  Raises PineCommandError on IPC_FAIL."""
        if self._sock is None:
            raise PineError("PINE client is not connected")
        total = len(payload) + 4
        if total > MAX_IPC_SIZE - 1:
            raise ValueError(f"PINE request too large ({total} bytes)")
        try:
            self._sock.sendall(struct.pack("<I", total) + payload)
            (rlen,) = struct.unpack("<I", self._recv_exact(4))
            if rlen < 5 or rlen > MAX_IPC_RETURN_SIZE + 16:
                raise PineError(f"malformed PINE reply length {rlen}")
            body = self._recv_exact(rlen - 4)
        except (OSError, PineError) as e:  # socket.timeout is an OSError
            self.close()  # the stream is out of sync now; force a reconnect
            raise e if isinstance(e, PineError) else PineError(f"PINE I/O error: {e}") from e
        if body[0] != 0x00:
            raise PineCommandError(f"PINE command failed (status 0x{body[0]:02X}; is a VM running?)")
        return body[1:]

    # - scalar memory access --------------------------------------------------------------------
    def _read(self, size: int, addr: int) -> int:
        data = self.transact(struct.pack("<BI", _READ_OPS[size], addr & 0xFFFFFFFF))
        if len(data) != size:
            raise PineError(f"short PINE read reply ({len(data)} != {size})")
        return struct.unpack(_FMT[size], data)[0]

    def _write(self, size: int, addr: int, value: int) -> None:
        mask = (1 << (8 * size)) - 1
        self.transact(struct.pack("<BI", _WRITE_OPS[size], addr & 0xFFFFFFFF) + struct.pack(_FMT[size], value & mask))

    def read8(self, addr: int) -> int:
        return self._read(1, addr)

    def read16(self, addr: int) -> int:
        return self._read(2, addr)

    def read32(self, addr: int) -> int:
        return self._read(4, addr)

    def read64(self, addr: int) -> int:
        return self._read(8, addr)

    def write8(self, addr: int, value: int) -> None:
        self._write(1, addr, value)

    def write16(self, addr: int, value: int) -> None:
        self._write(2, addr, value)

    def write32(self, addr: int, value: int) -> None:
        self._write(4, addr, value)

    def write64(self, addr: int, value: int) -> None:
        self._write(8, addr, value)

    # - block access ----------------------------------------------------------------------------
    def read_block(self, addr: int, nbytes: int) -> bytes:
        """Read `nbytes` of EE memory starting at `addr`, batching aligned Read64/32/16/8 commands
        into as few requests as the server's buffers allow.  Returns bytes in memory order."""
        if nbytes <= 0:
            return b""
        out = bytearray()
        batch_cmds = bytearray()
        batch_bytes = 0
        for a, size in _split_aligned(addr, nbytes):
            if batch_bytes + size > self.MAX_BATCH_REPLY:
                out += self._flush_reads(batch_cmds, batch_bytes)
                batch_cmds, batch_bytes = bytearray(), 0
            batch_cmds += struct.pack("<BI", _READ_OPS[size], a & 0xFFFFFFFF)
            batch_bytes += size
        out += self._flush_reads(batch_cmds, batch_bytes)
        return bytes(out)

    def _flush_reads(self, cmds: bytearray, expect: int) -> bytes:
        if not cmds:
            return b""
        data = self.transact(bytes(cmds))
        if len(data) != expect:
            raise PineError(f"short PINE batch reply ({len(data)} != {expect})")
        return data  # each result is the LE value == the bytes in memory order

    def write_block(self, addr: int, data: bytes) -> None:
        """Write `data` to EE memory at `addr` (batched aligned Write64/32/16/8)."""
        view = memoryview(bytes(data))
        cmds = bytearray()
        for a, size in _split_aligned(addr, len(view)):
            off = a - addr
            cmd = struct.pack("<BI", _WRITE_OPS[size], a & 0xFFFFFFFF) + bytes(view[off:off + size])
            if len(cmds) + len(cmd) > self.MAX_BATCH_REQUEST:
                self.transact(bytes(cmds))
                cmds = bytearray()
            cmds += cmd
        if cmds:
            self.transact(bytes(cmds))

    # - misc ------------------------------------------------------------------------------------
    def status(self) -> EmuStatus:
        data = self.transact(bytes([Op.STATUS]))
        return EmuStatus(struct.unpack("<I", data[:4])[0])

    def version(self) -> str:
        return self._string(Op.VERSION)

    def _string(self, op: Op) -> str:
        data = self.transact(bytes([op]))
        (n,) = struct.unpack("<I", data[:4])
        return data[4:4 + n].split(b"\0", 1)[0].decode("utf-8", "replace")

    def title(self) -> str:
        return self._string(Op.TITLE)

    def game_id(self) -> str:
        return self._string(Op.ID)
