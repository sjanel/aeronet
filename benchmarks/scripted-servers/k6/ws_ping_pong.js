// k6 WebSocket benchmark: Ping/Pong latency
// Measures control-frame round-trip time by sending periodic pings.

import {check} from 'k6';
import {Counter, Trend} from 'k6/metrics';
import ws from 'k6/ws';

const pingSent = new Counter('ws_pings_sent');
const pongRecv = new Counter('ws_pongs_received');
const pingRtt = new Trend('ws_ping_rtt_ms');

const BASE_URL = __ENV.WS_URL || 'ws://127.0.0.1:8080/ws';
const PING_INTERVAL_MS = parseInt(__ENV.PING_INTERVAL_MS || '50');
const SESSION_DURATION_MS = parseInt(__ENV.SESSION_DURATION_MS || '10000');

export const options = {
  scenarios: {
    ping_pong: {
      executor: 'constant-vus',
      vus: parseInt(__ENV.VUS || '50'),
      duration: __ENV.DURATION || '30s',
    },
  },
  thresholds: {
    ws_ping_rtt_ms: ['p(95)<50', 'p(99)<200'],
  },
};

export default function() {
  const res = ws.connect(BASE_URL, {}, function(socket) {
    socket.on('open', () => {
      socket.setInterval(() => {
        socket.locals = socket.locals || {};
        socket.locals.lastPing = Date.now();
        socket.ping();
        pingSent.add(1);
      }, PING_INTERVAL_MS);
    });

    socket.on('pong', () => {
      pongRecv.add(1);
      if (socket.locals && socket.locals.lastPing) {
        pingRtt.add(Date.now() - socket.locals.lastPing);
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
