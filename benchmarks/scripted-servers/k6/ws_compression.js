// k6 WebSocket benchmark: Compression on/off comparison
// Sends compressible payloads (repeated JSON) over permessage-deflate.
// Run twice: once with compression headers, once without, to compare.
// NOTE: k6's ws module uses the browser WebSocket API which negotiates
// permessage-deflate automatically when the server supports it.
// Use WS_COMPRESS=true/false env var to toggle the comparison label,
// and start the server with/without compression enabled accordingly.

import {check} from 'k6';
import {Counter, Trend} from 'k6/metrics';
import ws from 'k6/ws';

const msgSent = new Counter('ws_messages_sent');
const msgRecv = new Counter('ws_messages_received');
const rtt = new Trend('ws_echo_rtt_ms');

const BASE_URL = __ENV.WS_URL || 'ws://127.0.0.1:8080/ws';
const MSG_INTERVAL_MS = parseInt(__ENV.MSG_INTERVAL_MS || '20');
const SESSION_DURATION_MS = parseInt(__ENV.SESSION_DURATION_MS || '10000');

// Highly compressible JSON-like payload (~1 KB)
const PAYLOAD = JSON.stringify({
  type: 'benchmark',
  data: Array.from({length: 20}, (_, i) => ({
                                   id: i,
                                   name: 'item-' + i,
                                   value: i * 100,
                                   tags: ['benchmark', 'test', 'compression'],
                                 })),
});

export const options = {
  scenarios: {
    compression: {
      executor: 'constant-vus',
      vus: parseInt(__ENV.VUS || '50'),
      duration: __ENV.DURATION || '30s',
    },
  },
  thresholds: {
    ws_echo_rtt_ms: ['p(95)<200', 'p(99)<1000'],
  },
};

export default function() {
  const params = {};
  // k6 ws.connect supports a headers option; permessage-deflate negotiation
  // depends on server configuration, not client headers in ws module.

  const res = ws.connect(BASE_URL, params, function(socket) {
    socket.on('open', () => {
      socket.setInterval(() => {
        socket.locals = socket.locals || {};
        socket.locals.lastSend = Date.now();
        socket.send(PAYLOAD);
        msgSent.add(1);
      }, MSG_INTERVAL_MS);
    });

    socket.on('message', (_data) => {
      msgRecv.add(1);
      if (socket.locals && socket.locals.lastSend) {
        rtt.add(Date.now() - socket.locals.lastSend);
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
