"""Wire framing and mutation safety for title QA clients."""
import json
import socket
import threading

import pytest

from tools.sneslib.client import DebugClient, GameplayClient


def server(handler):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    port = listener.getsockname()[1]
    errors = []
    def run():
        try:
            with listener:
                with listener.accept()[0] as connection:
                    handler(connection)
        except BaseException as error:
            errors.append(error)
    thread = threading.Thread(target=run)
    thread.start()
    return port, thread, errors


def test_fragmented_banner_and_multiline_buffer_are_preserved():
    def handler(connection):
        assert connection.recv(1024) == b'ping\n'
        connection.sendall(b'{"connected":tr')
        connection.sendall(b'ue}\n{"frame":7}\n{"ok":true}\n')
        assert connection.recv(1024) == b'next\n'
    port, thread, errors = server(handler)
    with DebugClient(port) as client:
        assert client.query('ping') == {'frame': 7}
        assert client.query('next') == {'ok': True}
    thread.join()
    assert not errors


def test_lost_mutation_reply_is_not_replayed():
    commands = []
    def handler(connection):
        commands.append(connection.recv(1024))
    port, thread, errors = server(handler)
    with GameplayClient(port) as client:
        with pytest.raises(ConnectionError):
            client.q('step 60')
    thread.join()
    assert not errors
    assert commands == [b'step 60\n']


def test_json_error_is_a_reply_and_does_not_reconnect():
    def handler(connection):
        assert connection.recv(1024) == b'unsupported\n'
        connection.sendall(b'{"error":"unsupported command"}\n')
    port, thread, errors = server(handler)
    with DebugClient(port) as client:
        assert client.query('unsupported')['error'] == 'unsupported command'
    thread.join()
    assert not errors
