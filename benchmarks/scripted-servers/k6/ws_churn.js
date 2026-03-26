// k6 WebSocket benchmark: Connection churn
// Rapidly opens and closes WebSocket connections to stress the handshake path.

import {check} from 'k6';
import {Counter, Trend} from 'k6/metrics';
import ws from 'k6/ws';

const connOpened = new Counter('ws_connections_opened');
const connClosed = new Counter('ws_connections_closed');
const connTime = new Trend('ws_connection_lifetime_ms');

const BASE_URL = __ENV.WS_URL || 'ws://127.0.0.1:8080/ws';
// Short session: connect, send one message, verify echo, close
const SESSION_DURATION_MS = parseInt(__ENV.SESSION_DURATION_MS || '500');

export const options = {
  scenarios: {
    churn: {
      executor: 'constant-vus',
      vus: parseInt(__ENV.VUS || '100'),
      duration: __ENV.DURATION || '30s',
    },
  },
  thresholds: {
    ws_connection_lifetime_ms: ['p(95)<2000'],
  },
};

export default function() {
  const start = Date.now();

  const res = ws.connect(BASE_URL, {}, function(socket) {
    socket.on('open', () => {
      connOpened.add(1);
      socket.send('churn-test');
    });

    socket.on('message', (data) => {
      check(data, {
        'echo matches': (d) => d === 'churn-test',
      });
      // Close as soon as we get the echo back
      socket.close();
    });

    socket.on('close', () => {
      connClosed.add(1);
      connTime.add(Date.now() - start);
    });

    // Safety timeout
    socket.setTimeout(() => {
      socket.close();
    }, SESSION_DURATION_MS);
  });

  check(res, {
    'ws connected successfully': (r) => r && r.status === 101,
  });
}
