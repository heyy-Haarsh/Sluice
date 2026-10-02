import socket, sys, time
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(5)
print(f"slow backend on {port}", flush=True)

while True:
    c,_ = s.accept()
    total = 0
    while True:
        d = c.recv(1024)
        if not d: break
        total += len(d)
        time.sleep(0.02)
    print("slow backend received total bytes:", total, flush=True)
    time.sleep(1)