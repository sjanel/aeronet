// k6 WebSocket benchmark: Echo with large binary payloads (default 64 KB).
//
// Designed to make masked-frame demasking visible in throughput numbers:
// large payloads make the per-byte XOR demask cost dominate over per-frame
// overhead, so SIMD vectorization of the mask loop shows up cleanly.
//
// Binary frames are used (not text) to avoid mixing in UTF-8 validation cost.
// Override payload size via PAYLOAD_SIZE env var (bytes).

import {check} from 'k6';
import {Counter, Trend} from 'k6/metrics';
import ws from 'k6/ws';

const msgSent = new Counter('ws_messages_sent');
const msgRecv = new Counter('ws_messages_received');
const rtt = new Trend('ws_echo_rtt_ms');

const BASE_URL = __ENV.WS_URL || 'ws://127.0.0.1:8080/ws';
const MSG_INTERVAL_MS = parseInt(__ENV.MSG_INTERVAL_MS || '50');
const SESSION_DURATION_MS = parseInt(__ENV.SESSION_DURATION_MS || '10000');
const PAYLOAD_SIZE = parseInt(__ENV.PAYLOAD_SIZE || '65536');
const PIPELINE_DEPTH = parseInt(__ENV.PIPELINE_DEPTH || '0');

const PAYLOAD = new Uint8Array(PAYLOAD_SIZE);
for (let i = 0; i < PAYLOAD.length; i++) {
  PAYLOAD[i] = i & 0xFF;
}

export const options = {
  scenarios: {
    echo_large: {
      executor: 'constant-vus',
      vus: parseInt(__ENV.VUS || '50'),
      duration: __ENV.DURATION || '30s',
    },
  },
  thresholds: {
    ws_echo_rtt_ms: ['p(95)<2000', 'p(99)<5000'],
  },
};

export default function() {
  let lastSend = 0;

  const res = ws.connect(BASE_URL, {}, function(socket) {
    socket.on('open', () => {
      if (PIPELINE_DEPTH > 0) {
        lastSend = Date.now();
        for (let i = 0; i < PIPELINE_DEPTH; i++) {
          socket.sendBinary(PAYLOAD.buffer);
          msgSent.add(1);
        }
      } else {
        socket.setInterval(() => {
          lastSend = Date.now();
          socket.sendBinary(PAYLOAD.buffer);
          msgSent.add(1);
        }, MSG_INTERVAL_MS);
      }
    });

    socket.on('binaryMessage', (_data) => {
      msgRecv.add(1);
      if (lastSend > 0) {
        rtt.add(Date.now() - lastSend);
      }
      if (PIPELINE_DEPTH > 0) {
        lastSend = Date.now();
        socket.sendBinary(PAYLOAD.buffer);
        msgSent.add(1);
      }
    });

    socket.setTimeout(() => {
      socket.close();
    }, SESSION_DURATION_MS);
  });

  check(res, {
    'ws connected successfully': (r) => r && r.status === 101,
  });
}
