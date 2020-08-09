# Evergreen: inter-process socket transfer example

Evergreen is a TCP proxy that can be updated without breaking connections.
This is achieved by transferring open TCP sockets to the new instance
via a control Unix socket. Should be POSIX-compatible, tested on Linux.

Build the proxy: `gcc evergreen.c -o evergreen`

Run the proxy: `./evergreen proxy 10000 20000 /tmp/evergreen`

Connect to it from both sides:

* `nc -nv 127.0.0.1 10000`
* `nc -nv 127.0.0.1 20000`

Type a few lines to each `netcat` instance to verify the proxy works.

Modify the proxy, e.g. add a log statement when transferring data,
rebuild the proxy.

Update the proxy: `./evergreen update /tmp/evergreen`

Original instance should stop, `netcat`s should not.
Verify you can send more data through proxy and see that new code is running.

Implementation notes:

* Proxy part is very basic, not all socket events are handled properly.

* Transfer is done socket-by-socket, although `sendmsg`/`recvmsg` allows
    sending an array of descriptors.
