// k6 WebSocket benchmark: Mixed text + binary messages
// Alternates between text and binary payloads to exercise both code paths.

import {check} from 'k6';
import {Counter, Trend} from 'k6/metrics';
import ws from 'k6/ws';

const msgSent = new Counter('ws_messages_sent');
const msgRecv = new Counter('ws_messages_received');
const rtt = new Trend('ws_echo_rtt_ms');

const BASE_URL = __ENV.WS_URL || 'ws://127.0.0.1:8080/ws';
const MSG_INTERVAL_MS = parseInt(__ENV.MSG_INTERVAL_MS || '15');
const SESSION_DURATION_MS = parseInt(__ENV.SESSION_DURATION_MS || '10000');

// Text payload (~256 bytes)
const TEXT_PAYLOAD = 'T'.repeat(256);
// Binary payload (~512 bytes)
const BINARY_PAYLOAD = new Uint8Array(512);
for (let i = 0; i < BINARY_PAYLOAD.length; i++) {
  BINARY_PAYLOAD[i] = i % 256;
}

export const options = {
  scenarios: {
    mix_text_binary: {
      executor: 'constant-vus',
      vus: parseInt(__ENV.VUS || '50'),
      duration: __ENV.DURATION || '30s',
    },
  },
  thresholds: {
    ws_echo_rtt_ms: ['p(95)<150', 'p(99)<750'],
  },
};

export default function() {
  let msgCount = 0;

  const res = ws.connect(BASE_URL, {}, function(socket) {
    socket.on('open', () => {
      socket.setInterval(() => {
        socket.locals = socket.locals || {};
        socket.locals.lastSend = Date.now();

        if (msgCount % 2 === 0) {
          socket.send(TEXT_PAYLOAD);
        } else {
          socket.sendBinary(BINARY_PAYLOAD.buffer);
        }
        msgCount++;
        msgSent.add(1);
      }, MSG_INTERVAL_MS);
    });

    socket.on('message', (_data) => {
      msgRecv.add(1);
      if (socket.locals && socket.locals.lastSend) {
        rtt.add(Date.now() - socket.locals.lastSend);
      }
    });

    socket.on('binaryMessage', (_data) => {
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
