// k6 WebSocket benchmark: Echo with small text payloads (64–256 bytes)
// Measures message throughput and latency under sustained small-message load.

import {check} from 'k6';
import {Counter, Trend} from 'k6/metrics';
import ws from 'k6/ws';

const msgSent = new Counter('ws_messages_sent');
const msgRecv = new Counter('ws_messages_received');
const rtt = new Trend('ws_echo_rtt_ms');

const BASE_URL = __ENV.WS_URL || 'ws://127.0.0.1:8080/ws';
const MSG_INTERVAL_MS = parseInt(__ENV.MSG_INTERVAL_MS || '10');
const SESSION_DURATION_MS = parseInt(__ENV.SESSION_DURATION_MS || '10000');
const PAYLOAD_SIZE = parseInt(__ENV.PAYLOAD_SIZE || '128');
// Pipeline depth > 0 activates tight-loop mode: send initial burst then
// immediately send on each received echo.  Maximises server saturation.
const PIPELINE_DEPTH = parseInt(__ENV.PIPELINE_DEPTH || '0');

// Pre-generate a deterministic payload
const PAYLOAD = 'A'.repeat(PAYLOAD_SIZE);

export const options = {
  scenarios: {
    echo_small: {
      executor: 'constant-vus',
      vus: parseInt(__ENV.VUS || '50'),
      duration: __ENV.DURATION || '30s',
    },
  },
  thresholds: {
    ws_echo_rtt_ms: ['p(95)<100', 'p(99)<500'],
  },
};

export default function() {
  let lastSend = 0;

  const res = ws.connect(BASE_URL, {}, function(socket) {
    socket.on('open', () => {
      if (PIPELINE_DEPTH > 0) {
        lastSend = Date.now();
        for (let i = 0; i < PIPELINE_DEPTH; i++) {
          socket.send(PAYLOAD);
          msgSent.add(1);
        }
      } else {
        socket.setInterval(() => {
          lastSend = Date.now();
          socket.send(PAYLOAD);
          msgSent.add(1);
        }, MSG_INTERVAL_MS);
      }
    });

    socket.on('message', (_data) => {
      msgRecv.add(1);
      if (lastSend > 0) {
        rtt.add(Date.now() - lastSend);
      }
      if (PIPELINE_DEPTH > 0) {
        lastSend = Date.now();
        socket.send(PAYLOAD);
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
