#!/usr/bin/env python3
"""Tiny localhost-to-WSL TCP relay used for Android gdbserver captures."""

import selectors
import socket


def main() -> None:
    selector = selectors.DefaultSelector()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Bind only the private Windows/WSL virtual interface, never LAN interfaces.
    listener.bind(("172.17.0.1", 5041))
    listener.listen(1)
    client, _ = listener.accept()
    upstream = socket.create_connection(("127.0.0.1", 5039))
    for source, target in ((client, upstream), (upstream, client)):
        source.setblocking(False)
        selector.register(source, selectors.EVENT_READ, target)
    while selector.get_map():
        for key, _ in selector.select():
            data = key.fileobj.recv(65536)
            if not data:
                selector.unregister(key.fileobj)
                key.fileobj.close()
                key.data.close()
                continue
            key.data.sendall(data)


if __name__ == "__main__":
    main()
