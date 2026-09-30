# --- Build Stage for p9wl ---
FROM alpine:latest AS builder
WORKDIR /app
RUN apk add --no-cache \
    build-base \
    pkgconf \
    wlroots0.19-dev \
    wayland-dev \
    wayland-protocols \
    libxkbcommon-dev \
    pixman-dev \
    freerdp-dev \
    openssl-dev \
    fftw-dev \
    lz4-dev \
    zlib-dev \
    xkeyboard-config \
    linux-headers \
    openssl

COPY . .
RUN openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout /app/server.key \
    -out /app/server.crt \
    -days 365 -subj "/CN=p9wl"

RUN ln -s src/types.h types.h || true
RUN make

# --- Runtime & Web Frontend Stage ---
FROM alpine:latest
RUN apk add --no-cache \
    wlroots0.19 \
    wayland \
    libxkbcommon \
    pixman \
    freerdp \
    openssl \
    fftw \
    lz4 \
    zlib \
    xkeyboard-config \
    firefox \
    ttf-dejavu \
    ttf-liberation \
    fontconfig \
    nodejs \
    npm

WORKDIR /app
COPY --from=builder /app/p9wl-rdp-alpine /app/p9wl-rdp-alpine
COPY --from=builder /app/server.crt /app/server.crt
COPY --from=builder /app/server.key /app/server.key

RUN chmod 600 /app/server.key && chmod 644 /app/server.crt
RUN printf 'openssl_conf = openssl_init\n\n\
[openssl_init]\n\
providers = provider_sect\n\n\
[provider_sect]\n\
default = default_sect\n\
legacy = legacy_sect\n\n\
[default_sect]\n\
activate = 1\n\n\
[legacy_sect]\n\
activate = 1\n' > /app/openssl.cnf

ENV OPENSSL_CONF=/app/openssl.cnf
ENV WLOG_LEVEL=TRACE
RUN mkdir -p /tmp/xdg
ENV XDG_RUNTIME_DIR=/tmp/xdg \
    WAYLAND_DISPLAY=wayland-0 \
    MOZ_ENABLE_WAYLAND=1 \
    MOZ_DISABLE_CONTENT_SANDBOX=1

RUN mkdir -p /app/web
WORKDIR /app/web
RUN npm init -y && npm install express ws

# Light Mode, Minimal Vertical UI with Fixed Stream Parsing
RUN cat << 'EOF' > server.js
const express = require('express');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ noServer: true });

const PORT = process.env.PORT || 8080;

app.get('/', (req, res) => {
    res.send(`<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>p9wl Live Viewer</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #f8fafc; color: #0f172a; margin: 0; padding: 10px; display: flex; flex-direction: column; align-items: center; height: 100vh; box-sizing: border-box; outline: none; }
        .main-layout { width: 100%; max-width: 1400px; height: 100%; display: flex; flex-direction: column; gap: 8px; }
        header { display: flex; justify-content: space-between; align-items: center; padding: 4px 8px; background: #ffffff; border: 1px solid #e2e8f0; border-radius: 6px; font-size: 0.85rem; }
        h1 { color: #0284c7; margin: 0; font-size: 1rem; }
        .screen-container { flex-grow: 1; position: relative; background: #000000; border-radius: 6px; overflow: hidden; display: flex; justify-content: center; align-items: center; border: 1px solid #cbd5e1; min-height: 0; }
        canvas { width: 100%; height: 100%; object-fit: contain; display: block; cursor: crosshair; }
        .toolbar { display: flex; justify-content: space-between; align-items: center; background: #ffffff; padding: 6px 12px; border-radius: 6px; border: 1px solid #e2e8f0; font-size: 0.85rem; }
        button { background: #0284c7; color: white; border: none; padding: 4px 12px; border-radius: 4px; font-weight: 600; cursor: pointer; font-size: 0.85rem; }
        button:hover { background: #0369a1; }
        .status { display: inline-block; width: 8px; height: 8px; background: #eab308; border-radius: 50%; margin-right: 4px; }
        .status.connected { background: #22c55e; }
        .status.disconnected { background: #ef4444; }
        .meta { color: #64748b; font-family: monospace; font-size: 0.8rem; }
    </style>
</head>
<body tabindex="0">
    <div class="main-layout">
        <header>
            <h1>p9wl</h1>
            <div><span id="status-dot" class="status"></span><span id="status-text" class="meta">Connecting...</span></div>
        </header>
        
        <div class="screen-container">
            <canvas id="rdpCanvas" width="1280" height="800"></canvas>
        </div>
        
        <div class="toolbar">
            <button id="playPauseBtn" onclick="togglePlayPause()">Pause</button>
            <div class="meta">Frame: <span id="frameNumLabel">--</span></div>
        </div>
    </div>

    <script>
        const canvas = document.getElementById('rdpCanvas');
        const ctx = canvas.getContext('2d');
        const statusDot = document.getElementById('status-dot');
        const statusText = document.getElementById('status-text');

        let isPlaying = true;
        let latestBuffer = null;
        let latestFrameNum = '--';

        function drawPlaceholder(text) {
            ctx.fillStyle = '#0f172a';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
            ctx.fillStyle = '#38bdf8';
            ctx.font = '16px sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText(text, canvas.width / 2, canvas.height / 2);
        }

        drawPlaceholder('Waiting for frames...');

        const ws = new WebSocket((location.protocol === 'https:' ? 'wss:' : 'ws:') + '//' + location.host + '/frame-stream');
        ws.binaryType = 'arraybuffer';

        ws.onopen = () => {
            statusDot.className = 'status connected';
            statusText.innerText = 'Live';
        };

        ws.onclose = () => {
            statusDot.className = 'status disconnected';
            statusText.innerText = 'Disconnected';
            drawPlaceholder('Connection lost');
        };

        ws.onmessage = (event) => {
            const view = new DataView(event.data);
            const nameLen = view.getUint32(0);
            const nameBytes = new Uint8Array(event.data, 4, nameLen);
            const frameName = new TextDecoder().decode(nameBytes);
            const ppmBuffer = event.data.slice(4 + nameLen);

            const match = frameName.match(/frame_(\\d+)\\.ppm/);
            latestFrameNum = match ? match[1] : frameName;
            latestBuffer = ppmBuffer;

            if (isPlaying) {
                renderPPM(latestBuffer, latestFrameNum);
            }
        };

        function renderPPM(arrayBuffer, frameNum) {
            const bytes = new Uint8Array(arrayBuffer);
            let text = new TextDecoder().decode(bytes.subarray(0, 200));
            let match = text.match(/^P6\\s+(\\d+)\\s+(\\d+)\\s+(\\d+)\\s/);
            if (!match) return;

            let w = parseInt(match[1]);
            let h = parseInt(match[2]);
            let headerLen = match[0].length;

            if (canvas.width !== w || canvas.height !== h) {
                canvas.width = w;
                canvas.height = h;
            }

            let pixelData = bytes.subarray(headerLen);
            let imgData = ctx.createImageData(w, h);
            let data = imgData.data;

            let p = 0;
            let q = 0;
            let totalPixels = w * h;
            for (let i = 0; i < totalPixels; i++) {
                data[q]     = pixelData[p];     
                data[q + 1] = pixelData[p + 1]; 
                data[q + 2] = pixelData[p + 2]; 
                data[q + 3] = 255;              
                p += 3;
                q += 4;
            }

            ctx.putImageData(imgData, 0, 0);
            document.getElementById('frameNumLabel').innerText = frameNum;
        }

        function sendInput(type, data) {
            if (ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ type, ...data }));
            }
        }

        canvas.addEventListener('mousemove', (e) => {
            const rect = canvas.getBoundingClientRect();
            const x = Math.floor((e.clientX - rect.left) * (canvas.width / rect.width));
            const y = Math.floor((e.clientY - rect.top) * (canvas.height / rect.height));
            sendInput('mouse', { x, y, buttons: e.buttons });
        });

        canvas.addEventListener('mousedown', (e) => {
            const rect = canvas.getBoundingClientRect();
            const x = Math.floor((e.clientX - rect.left) * (canvas.width / rect.width));
            const y = Math.floor((e.clientY - rect.top) * (canvas.height / rect.height));
            let btnMask = e.button === 0 ? 1 : (e.button === 2 ? 4 : 2);
            sendInput('mouse', { x, y, buttons: btnMask });
        });

        canvas.addEventListener('mouseup', (e) => {
            const rect = canvas.getBoundingClientRect();
            const x = Math.floor((e.clientX - rect.left) * (canvas.width / rect.width));
            const y = Math.floor((e.clientY - rect.top) * (canvas.height / rect.height));
            sendInput('mouse', { x, y, buttons: 0 });
        });

        window.addEventListener('keydown', (e) => {
            sendInput('key', { rune: e.keyCode, pressed: 1 });
        });

        window.addEventListener('keyup', (e) => {
            sendInput('key', { rune: e.keyCode, pressed: 0 });
        });

        function togglePlayPause() {
            isPlaying = !isPlaying;
            const btn = document.getElementById('playPauseBtn');
            btn.innerText = isPlaying ? 'Pause' : 'Play';
            if (isPlaying && latestBuffer) {
                renderPPM(latestBuffer, latestFrameNum);
            }
        }
    </script>
</body>
</html>`);
});

server.on('upgrade', (request, socket, head) => {
    if (request.url === '/frame-stream') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request);
        });
    } else {
        socket.destroy();
    }
});

wss.on('connection', (ws) => {
    ws.on('message', (message) => {
        try {
            const msg = JSON.parse(message);
            if (msg.type === 'mouse') {
                fs.writeFileSync('/tmp/p9wl_input.fifo', `MOUSE ${msg.x} ${msg.y} ${msg.buttons}\n`);
            } else if (msg.type === 'key') {
                fs.writeFileSync('/tmp/p9wl_input.fifo', `KEY ${msg.rune} ${msg.pressed}\n`);
            }
        } catch (e) {}
    });
});

setInterval(() => {
    try {
        const files = fs.readdirSync('/app')
            .filter(f => f.startsWith('frame_') && f.endsWith('.ppm'))
            .map(f => ({ name: f, time: fs.statSync(path.join('/app', f)).mtime.getTime() }))
            .sort((a, b) => b.time - a.time);

        if (files.length > 0) {
            const latestName = files[0].name;
            const ppmBuffer = fs.readFileSync(path.join('/app', latestName));
            
            const nameBytes = Buffer.from(latestName, 'utf-8');
            const header = Buffer.alloc(4);
            header.writeUInt32BE(nameBytes.length, 0);
            const payload = Buffer.concat([header, nameBytes, ppmBuffer]);

            wss.clients.forEach(client => {
                if (client.readyState === client.OPEN) {
                    client.send(payload);
                }
            });
        }
    } catch (e) {}
}, 50);

server.listen(PORT, '0.0.0.0', () => {
    console.log('Live server running on http://0.0.0.0:' + PORT);
});
EOF

WORKDIR /app
RUN cat << 'EOF' > /app/entrypoint.sh
#!/bin/sh
/app/p9wl-rdp-alpine -d firefox &
node /app/web/server.js
EOF

RUN chmod +x /app/entrypoint.sh
EXPOSE 3389 8080
ENTRYPOINT ["/app/entrypoint.sh"]