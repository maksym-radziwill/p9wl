1. `docker build -t p9wl-rdp .`
2. `docker run -itd --security-opt seccomp=unconfined -p 3389:3389 -p 8080:8080 p9wl-rdp`