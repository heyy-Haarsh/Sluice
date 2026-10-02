import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(5)
print(f"halfclose backend on {port}", flush=True)
c,_ = s.accept()
got = b''
while True:
    d = c.recv(65536)
    if not d: break          # client sent FIN
    got += d
print("backend got:", got, flush=True)
c.sendall(b'reply-after-your-eof\n')
c.close()