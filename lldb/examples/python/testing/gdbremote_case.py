"""A mock GDB-remote server test base, for testing gdb-remote clients
(including ``ScriptedProcess``/``ScriptedThread`` implementations that
connect to a kernel or other target over the gdb-remote protocol) without a
real target or hardware.

Modernized reimplementation of ``lldbsuite.test.gdbclientutils`` and
``lldbsuite.test.lldbgdbclient.GDBRemoteTestBase`` — both already mostly
decoupled from ``configuration``/build-step machinery; the only real
coupling this removes is inheriting from ``lldbsuite``'s ``TestBase``.
"""

from __future__ import annotations

import ctypes
import enum
import errno
import io
import socket
import threading
import traceback
from abc import ABC, abstractmethod

import lldb

from lldb.testing.testcase import LLDBTestCase


def checksum(message: str) -> int:
    """Compute the GDB-remote protocol's modulo-256 checksum of ``message``."""
    return sum(ord(c) for c in message) % 256


def frame_packet(message: str, prefix: str = "$") -> str:
    """Frame ``message`` for sending over a gdb-remote connection.

    Args:
        message: The unframed packet body.
        prefix: ``"$"`` for a normal packet, ``"%"`` for a notification.

    Returns:
        The framed packet: ``prefix``, ``message``, ``#``, and a
        two-character hex checksum.
    """
    return f"{prefix}{message}#{checksum(message):02x}"


def escape_binary(message: str) -> str:
    """Escape ``$``, ``#``, and ``{`` per the gdb-remote binary escaping rules."""
    out = []
    for c in message:
        code = ord(c)
        if code in (0x23, 0x24, 0x7D):
            out.append(chr(0x7D))
            out.append(chr(code ^ 0x20))
        else:
            out.append(c)
    return "".join(out)


def hex_encode_bytes(message: str) -> str:
    """Encode ``message`` as a two-hex-digit-per-byte string."""
    return "".join(f"{ord(c):02x}" for c in message)


def hex_decode_bytes(hex_bytes: str) -> str:
    """Decode a two-hex-digit-per-byte string back into a binary message."""
    return "".join(
        chr(int(hex_bytes[i : i + 2], 16)) for i in range(0, len(hex_bytes) - 1, 2)
    )


def parse_memory_read_packet(packet: str) -> tuple[int, int] | None:
    """Parse an ``"m<addr>,<len>"``/``"x<addr>,<len>"`` packet.

    Returns:
        ``(addr, length)``, or ``None`` if ``packet`` isn't a memory read.
    """
    if not packet or packet[0] not in ("m", "x"):
        return None
    try:
        addr, length = (int(part, 16) for part in packet[1:].split(","))
    except ValueError:
        return None
    return addr, length


class PacketDirection(enum.Enum):
    """Direction of a logged gdb-remote packet, from the client's perspective."""

    RECV = "recv"
    SEND = "send"


class PacketLog:
    """Records ``(PacketDirection, packet)`` pairs for later assertions."""

    def __init__(self) -> None:
        self._packets: list[tuple[PacketDirection, str]] = []

    def add_sent(self, packet: str) -> None:
        self._packets.append((PacketDirection.SEND, packet))

    def add_received(self, packet: str) -> None:
        self._packets.append((PacketDirection.RECV, packet))

    def get_sent(self) -> list[str]:
        return [pkt for direction, pkt in self._packets if direction == PacketDirection.SEND]

    def get_received(self) -> list[str]:
        return [pkt for direction, pkt in self._packets if direction == PacketDirection.RECV]

    def __iter__(self):
        return iter(self._packets)


class SpecialResponse(enum.Enum):
    """Sentinel responses a :class:`MockGDBServerResponder` method may return
    instead of a packet body."""

    RESPONSE_DISCONNECT = enum.auto()
    """Terminate the connection instead of sending a response."""

    RESPONSE_NONE = enum.auto()
    """Send nothing for this part of a multi-part response."""


Response = str | SpecialResponse


class UnexpectedPacketException(Exception):
    """Raised by a :class:`MockGDBServerResponder` method with no
    implementation for the packet it was asked to handle."""


class MockGDBServerResponder:
    """Handles client packets and issues server responses for gdb-remote tests.

    Handles many typical situations out of the box (register/memory
    read-write, thread enumeration, ``qHostInfo``, ...); override the
    specific packet-type method you care about (``readRegisters``,
    ``qHostInfo``, ``haltReason``, ``vAttach``, ...). Anything not
    recognized falls through to :meth:`other`.
    """

    register_count: int = 40

    def __init__(self) -> None:
        self.packet_log = PacketLog()

    def respond(self, packet: str) -> Response | list[Response]:
        """Return the unframed response(s) to ``packet``, logging both sides."""
        self.packet_log.add_received(packet)
        response = self._respond_impl(packet)
        parts = response if isinstance(response, list) else [response]
        for part in parts:
            if not isinstance(part, SpecialResponse):
                self.packet_log.add_sent(part)
        return response

    def _respond_impl(self, packet: str) -> Response | list[Response]:
        if packet is MockGDBServer.PACKET_INTERRUPT:
            return self.interrupt()
        if packet == "c":
            return self.cont()
        if packet.startswith("vCont;c"):
            return self.v_cont(packet)
        if packet[0] == "g":
            return self.readRegisters()
        if packet[0] == "G":
            return self.writeRegisters(packet[1:].split(";")[0])
        if packet[0] == "p":
            return self.readRegister(int(packet[1:].split(";")[0], 16))
        if packet[0] == "P":
            register, value = packet[1:].split("=")
            return self.writeRegister(int(register, 16), value)
        if packet[0] == "m":
            addr, length = parse_memory_read_packet(packet)
            return self.readMemory(addr, length)
        if packet[0] == "M":
            location, encoded_data = packet[1:].split(":")
            addr, _ = (int(x, 16) for x in location.split(","))
            return self.writeMemory(addr, encoded_data)
        if packet.startswith("qSupported"):
            return self.qSupported(packet[11:].split(";"))
        if packet == "qfThreadInfo":
            return self.qfThreadInfo()
        if packet == "qsThreadInfo":
            return self.qsThreadInfo()
        if packet == "qC":
            return self.qC()
        if packet == "?":
            return self.haltReason()
        if packet.startswith("vAttach;"):
            return self.vAttach(int(packet.partition(";")[2], 16))
        if packet[0] == "Z":
            return self.setBreakpoint(packet)
        if packet == "qHostInfo":
            return self.qHostInfo()
        if packet == "qProcessInfo":
            return self.qProcessInfo()
        if packet == "k":
            return self.k()
        return self.other(packet)

    def interrupt(self) -> Response:
        raise UnexpectedPacketException()

    def cont(self) -> Response:
        raise UnexpectedPacketException()

    def v_cont(self, packet: str) -> Response:
        raise UnexpectedPacketException()

    def readRegisters(self) -> str:
        return "00000000" * self.register_count

    def readRegister(self, register: int) -> str:
        return "00000000"

    def writeRegisters(self, registers_hex: str) -> str:
        return "OK"

    def writeRegister(self, register: int, value_hex: str) -> str:
        return "OK"

    def readMemory(self, addr: int, length: int) -> str:
        return "00" * length

    def writeMemory(self, addr: int, data_hex: str) -> str:
        return "OK"

    def qSupported(self, client_supported: list[str]) -> str:
        return "qXfer:features:read+;PacketSize=3fff;QStartNoAckMode+"

    def qfThreadInfo(self) -> str:
        return "l"

    def qsThreadInfo(self) -> str:
        return "l"

    def qC(self) -> str:
        return "QC0"

    def haltReason(self) -> str:
        return "S02"  # SIGINT

    def qHostInfo(self) -> str:
        return "ptrsize:8;endian:little;"

    def qProcessInfo(self) -> str:
        return ""

    def vAttach(self, pid: int) -> Response:
        raise UnexpectedPacketException()

    def setBreakpoint(self, packet: str) -> Response:
        raise UnexpectedPacketException()

    def k(self) -> list[Response]:
        return ["W01", SpecialResponse.RESPONSE_DISCONNECT]

    def other(self, packet: str) -> str:
        """Fallback for any packet not recognized above. The empty string
        means "unsupported"; override to customize."""
        return ""


class ServerChannel(ABC):
    """A transport (TCP, Unix socket, or pty) a :class:`MockGDBServer` talks over."""

    @abstractmethod
    def get_connect_address(self) -> str:
        """Return the address for a client to connect to."""

    @abstractmethod
    def get_connect_url(self) -> str:
        """Return the URL suitable for ``SBTarget.ConnectRemote``."""

    def close_server(self) -> None:
        """Close all resources used by the server."""

    def accept(self) -> None:
        """Accept a single client connection."""

    def close_connection(self) -> None:
        """Close all resources used by the accepted connection."""

    @abstractmethod
    def recv(self) -> bytes:
        """Receive a data chunk from the connected client."""

    @abstractmethod
    def sendall(self, data: bytes) -> None:
        """Send ``data`` to the connected client."""


class _SocketChannel(ServerChannel):
    def __init__(self, family: int, kind: int, proto: int, addr) -> None:
        self._server_socket = socket.socket(family, kind, proto)
        self._connection: socket.socket | None = None
        self._server_socket.bind(addr)
        self._server_socket.listen(1)

    def close_server(self) -> None:
        self._server_socket.close()

    def accept(self) -> None:
        assert self._connection is None
        self._server_socket.settimeout(30.0)
        client, _ = self._server_socket.accept()
        client.settimeout(None)
        self._connection = client

    def close_connection(self) -> None:
        assert self._connection is not None
        self._connection.close()
        self._connection = None

    def recv(self) -> bytes:
        assert self._connection is not None
        return self._connection.recv(4096)

    def sendall(self, data: bytes) -> None:
        assert self._connection is not None
        self._connection.sendall(data)


class TCPServerSocket(_SocketChannel):
    """A loopback TCP :class:`ServerChannel`."""

    def __init__(self) -> None:
        family, kind, proto, _, addr = socket.getaddrinfo(
            "localhost", 0, proto=socket.IPPROTO_TCP
        )[0]
        super().__init__(family, kind, proto, addr)

    def get_connect_address(self) -> str:
        host, port = self._server_socket.getsockname()[:2]
        return f"[{host}]:{port}"

    def get_connect_url(self) -> str:
        return f"connect://{self.get_connect_address()}"


class UnixServerSocket(_SocketChannel):
    """A Unix-domain-socket :class:`ServerChannel`."""

    def __init__(self, addr: str) -> None:
        super().__init__(socket.AF_UNIX, socket.SOCK_STREAM, 0, addr)

    def get_connect_address(self) -> str:
        return self._server_socket.getsockname()

    def get_connect_url(self) -> str:
        return f"unix-connect://{self.get_connect_address()}"


class PtyServerSocket(ServerChannel):
    """A pty-based :class:`ServerChannel`, for testing serial-style connections."""

    def __init__(self) -> None:
        import pty
        import tty

        primary, secondary = pty.openpty()
        tty.setraw(primary)
        self._primary = io.FileIO(primary, "r+b")
        self._secondary = io.FileIO(secondary, "r+b")

    def get_connect_address(self) -> str:
        libc = ctypes.CDLL(None)
        libc.ptsname.argtypes = (ctypes.c_int,)
        libc.ptsname.restype = ctypes.c_char_p
        return libc.ptsname(self._primary.fileno()).decode()

    def get_connect_url(self) -> str:
        return f"serial://{self.get_connect_address()}"

    def close_server(self) -> None:
        self._secondary.close()
        self._primary.close()

    def recv(self) -> bytes:
        try:
            return self._primary.read(4096)
        except OSError as error:
            if error.errno == errno.EIO:
                return b""  # closing the pty results in EIO on Linux
            raise

    def sendall(self, data: bytes) -> None:
        self._primary.write(data)


class TerminateConnectionException(Exception):
    """Raised internally to unwind out of :meth:`MockGDBServer.run` on ``RESPONSE_DISCONNECT``."""


class InvalidPacketException(Exception):
    """Raised when the client sends malformed gdb-remote protocol data."""


class MockGDBServer:
    """A simple gdb-remote server for testing client behavior with
    custom-tailored responses.

    Responses are generated via :attr:`responder`, an instance of a
    :class:`MockGDBServerResponder` subclass; swap it out per test to
    customize behavior.
    """

    PACKET_ACK = object()
    PACKET_INTERRUPT = object()

    def __init__(self, channel: ServerChannel) -> None:
        self._channel = channel
        self.responder: MockGDBServerResponder = MockGDBServerResponder()
        self._thread: threading.Thread | None = None
        self._received_data = ""
        self._received_offset = 0
        self._should_send_ack = True

    def start(self) -> None:
        """Start a background thread that waits for a single client connection."""
        self._thread = threading.Thread(target=self.run)
        self._thread.start()

    def stop(self) -> None:
        """Join the background thread started by :meth:`start`."""
        if self._thread is not None:
            self._thread.join()
            self._thread = None

    def get_connect_address(self) -> str:
        """Return the address for a client to connect to."""
        return self._channel.get_connect_address()

    def get_connect_url(self) -> str:
        """Return the URL suitable for ``SBTarget.ConnectRemote``."""
        return self._channel.get_connect_url()

    def run(self) -> None:
        """Accept one client connection and serve packets until it disconnects."""
        try:
            self._channel.accept()
        except Exception:
            traceback.print_exc()
            return
        self._should_send_ack = True
        self._received_data = ""
        self._received_offset = 0
        try:
            while True:
                data = self._channel.recv().decode("latin-1")
                if not data:
                    break
                self._received_data += data
                packet = self._parse_packet()
                while packet is not None:
                    self._handle_packet(packet)
                    packet = self._parse_packet()
        except TerminateConnectionException:
            pass
        except Exception:
            traceback.print_exc()
        finally:
            self._channel.close_connection()
            self._channel.close_server()

    def _parse_packet(self):
        data = self._received_data
        index = self._received_offset
        data_len = len(data)
        if data_len == 0:
            return None

        if index == 0:
            if data[0] == "+":
                self._received_data = data[1:]
                return self.PACKET_ACK
            if ord(data[0]) == 3:
                self._received_data = data[1:]
                return self.PACKET_INTERRUPT
            if data[0] == "$":
                index += 1
            else:
                raise InvalidPacketException(f"Unexpected leading byte: {data[0]}")

        while index < data_len and data[index] != "#":
            index += 1
        if index > data_len - 3:
            self._received_offset = index
            return None

        packet = data[1:index]
        index += 1
        try:
            received_checksum = int(data[index : index + 2], 16)
        except ValueError:
            raise InvalidPacketException("Checksum is not valid hex")
        index += 2
        if received_checksum != checksum(packet):
            raise InvalidPacketException(
                f"Checksum {received_checksum:02x} does not match content {checksum(packet):02x}"
            )

        self._received_data = data[index:]
        self._received_offset = 0
        return packet

    def _send_packet(self, packet: str, prefix: str = "$") -> None:
        self._channel.sendall(frame_packet(packet, prefix).encode("latin-1"))

    def _handle_packet(self, packet) -> None:
        if packet is self.PACKET_ACK:
            return

        response: Response | list[Response] = [""]
        if self._should_send_ack:
            self._channel.sendall(b"+")
        if packet == "QStartNoAckMode":
            self._should_send_ack = False
            response = ["OK"]
        elif self.responder is not None:
            response = self.responder.respond(packet)

        parts = response if isinstance(response, list) else [response]
        for part in parts:
            if part is SpecialResponse.RESPONSE_NONE:
                continue
            if part is SpecialResponse.RESPONSE_DISCONNECT:
                raise TerminateConnectionException()
            assert isinstance(part, str)
            self._send_packet(part)


class GDBRemoteTestCase(LLDBTestCase):
    """Starts a :class:`MockGDBServer` for each test and connects to it.

    Use this to test any client that talks the gdb-remote protocol —
    including a ``ScriptedProcess``/``ScriptedThread`` implementation that
    connects to a kernel or hardware target over gdb-remote — without a
    real target. Set :attr:`server_socket_class` to change the transport
    (defaults to a loopback TCP socket).
    """

    server_socket_class: type[ServerChannel] = TCPServerSocket

    def setUp(self) -> None:
        super().setUp()
        self.server = MockGDBServer(self.server_socket_class())
        self.server.start()

    def tearDown(self) -> None:
        self.server.stop()
        super().tearDown()

    def connect(self, target: lldb.SBTarget, plugin: str = "gdb-remote") -> lldb.SBProcess:
        """Connect ``target`` to the mock server.

        Args:
            target: The target to connect.
            plugin: The process plugin name to use for the connection.

        Returns:
            The connected ``lldb.SBProcess``.
        """
        listener = target.GetDebugger().GetListener()
        error = lldb.SBError()
        process = target.ConnectRemote(listener, self.server.get_connect_url(), plugin, error)
        self.assertSuccess(error)
        self.assertTrue(process.IsValid(), "ConnectRemote returned an invalid process")
        return process

    def assertPacketLogReceived(
        self, packets: list[str], log: PacketLog | None = None
    ) -> None:
        """Assert that ``packets`` appear, in order, among the received packets.

        Args:
            packets: The expected packets, in order. They need not be
                consecutive in the actual log.
            log: The log to check; defaults to ``self.server.responder.packet_log``.
        """
        received = (log or self.server.responder.packet_log).get_received()
        expected_index = 0
        for actual in received:
            if expected_index < len(packets) and actual == packets[expected_index]:
                expected_index += 1
        if expected_index < len(packets):
            joined = "\n\t".join(received[-10:])
            self.fail(f"Did not receive: {packets[expected_index]}\nLast 10 packets:\n\t{joined}")
