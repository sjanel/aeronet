#!/bin/bash
# setup_bench_resources.sh - Generate certificates and static files for benchmarks
#
# Creates:
#   - TLS certificates (self-signed, for testing only)
#   - Static files of various sizes for file serving benchmarks
#
# Usage: ./setup_bench_resources.sh [output_dir]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${1:-$SCRIPT_DIR}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ============================================================
# TLS Certificate Generation
# ============================================================
generate_certificates() {
  local cert_dir="$OUTPUT_DIR/certs"
  
  if [ -f "$cert_dir/server.crt" ] && [ -f "$cert_dir/server.key" ]; then
    log_info "Certificates already exist in $cert_dir"
    return 0
  fi
  
  if ! command -v openssl &> /dev/null; then
    log_error "openssl not found - cannot generate certificates"
    return 1
  fi
  
  log_info "Generating TLS certificates in $cert_dir..."
  mkdir -p "$cert_dir"
  
  # Generate private key
  openssl genrsa -out "$cert_dir/server.key" 2048 2>/dev/null
  
  # Generate self-signed certificate
  openssl req -new -x509 \
    -key "$cert_dir/server.key" \
    -out "$cert_dir/server.crt" \
    -days 365 \
    -subj "/C=XX/ST=Benchmark/L=Benchmark/O=Benchmark/CN=localhost" \
    2>/dev/null
  
  # Set permissions
  chmod 600 "$cert_dir/server.key"
  chmod 644 "$cert_dir/server.crt"
  
  log_info "Certificates generated:"
  log_info "  Certificate: $cert_dir/server.crt"
  log_info "  Private key: $cert_dir/server.key"
  
  return 0
}

# ============================================================
# Static Files Generation
# ============================================================
generate_static_files() {
  local static_dir="$OUTPUT_DIR/static"
  
  log_info "Generating static files in $static_dir..."
  mkdir -p "$static_dir"
  
  # index.html (~1KB) - Small HTML file
  cat > "$static_dir/index.html" << 'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Benchmark Test Page</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <header>
    <h1>HTTP Server Benchmark</h1>
    <nav>
      <a href="/">Home</a>
      <a href="/about">About</a>
      <a href="/contact">Contact</a>
    </nav>
  </header>
  <main>
    <section id="content">
      <h2>Welcome to the Benchmark Suite</h2>
      <p>This page is used to test static file serving performance.</p>
      <p>The server should deliver this content efficiently using sendfile() or similar zero-copy mechanisms.</p>
    </section>
  </main>
  <footer>
    <p>&copy; 2025 Benchmark Suite</p>
  </footer>
  <script src="app.js"></script>
</body>
</html>
EOF

  # style.css (~8KB) - Medium CSS file
  cat > "$static_dir/style.css" << 'EOF'
/* Benchmark Test Stylesheet */
:root {
  --primary-color: #3498db;
  --secondary-color: #2ecc71;
  --background-color: #f5f5f5;
  --text-color: #333;
  --border-radius: 8px;
  --spacing-unit: 16px;
}

* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
  line-height: 1.6;
  color: var(--text-color);
  background-color: var(--background-color);
}

header {
  background: linear-gradient(135deg, var(--primary-color), var(--secondary-color));
  color: white;
  padding: calc(var(--spacing-unit) * 2);
  box-shadow: 0 2px 10px rgba(0,0,0,0.1);
}

header h1 {
  font-size: 2.5rem;
  margin-bottom: var(--spacing-unit);
}

nav {
  display: flex;
  gap: var(--spacing-unit);
}

nav a {
  color: white;
  text-decoration: none;
  padding: calc(var(--spacing-unit) / 2) var(--spacing-unit);
  border-radius: var(--border-radius);
  transition: background-color 0.3s ease;
}

nav a:hover {
  background-color: rgba(255,255,255,0.2);
}

main {
  max-width: 1200px;
  margin: 0 auto;
  padding: calc(var(--spacing-unit) * 2);
}

section {
  background: white;
  padding: calc(var(--spacing-unit) * 2);
  border-radius: var(--border-radius);
  box-shadow: 0 2px 5px rgba(0,0,0,0.05);
  margin-bottom: calc(var(--spacing-unit) * 2);
}

h2 {
  color: var(--primary-color);
  margin-bottom: var(--spacing-unit);
  border-bottom: 2px solid var(--secondary-color);
  padding-bottom: calc(var(--spacing-unit) / 2);
}

p {
  margin-bottom: var(--spacing-unit);
}

footer {
  text-align: center;
  padding: var(--spacing-unit);
  background-color: #333;
  color: white;
  margin-top: calc(var(--spacing-unit) * 2);
}

/* Responsive design */
@media (max-width: 768px) {
  header h1 { font-size: 1.8rem; }
  nav { flex-direction: column; gap: calc(var(--spacing-unit) / 2); }
  main { padding: var(--spacing-unit); }
}

/* Animation classes */
.fade-in {
  animation: fadeIn 0.5s ease-in;
}

@keyframes fadeIn {
  from { opacity: 0; transform: translateY(-10px); }
  to { opacity: 1; transform: translateY(0); }
}

/* Utility classes */
.text-center { text-align: center; }
.text-left { text-align: left; }
.text-right { text-align: right; }
.mt-1 { margin-top: var(--spacing-unit); }
.mt-2 { margin-top: calc(var(--spacing-unit) * 2); }
.mb-1 { margin-bottom: var(--spacing-unit); }
.mb-2 { margin-bottom: calc(var(--spacing-unit) * 2); }
.p-1 { padding: var(--spacing-unit); }
.p-2 { padding: calc(var(--spacing-unit) * 2); }

/* Button styles */
.btn {
  display: inline-block;
  padding: calc(var(--spacing-unit) / 2) var(--spacing-unit);
  background-color: var(--primary-color);
  color: white;
  border: none;
  border-radius: var(--border-radius);
  cursor: pointer;
  transition: all 0.3s ease;
}

.btn:hover {
  background-color: var(--secondary-color);
  transform: translateY(-2px);
  box-shadow: 0 4px 10px rgba(0,0,0,0.2);
}

.btn-secondary {
  background-color: var(--secondary-color);
}

.btn-secondary:hover {
  background-color: var(--primary-color);
}
EOF

  # app.js (~16KB) - Medium JavaScript file
  cat > "$static_dir/app.js" << 'EOF'
/**
 * Benchmark Test Application
 * This JavaScript file is used to test static file serving performance.
 */

(function() {
  'use strict';

  // Configuration
  const CONFIG = {
    apiEndpoint: '/api',
    refreshInterval: 5000,
    maxRetries: 3,
    debug: false
  };

  // Utility functions
  const Utils = {
    log: function(message, level = 'info') {
      if (CONFIG.debug || level === 'error') {
        console[level](`[Benchmark] ${message}`);
      }
    },

    formatBytes: function(bytes) {
      if (bytes === 0) return '0 Bytes';
      const k = 1024;
      const sizes = ['Bytes', 'KB', 'MB', 'GB'];
      const i = Math.floor(Math.log(bytes) / Math.log(k));
      return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    },

    formatTime: function(ms) {
      if (ms < 1000) return `${ms.toFixed(2)}ms`;
      return `${(ms / 1000).toFixed(2)}s`;
    },

    generateId: function() {
      return Math.random().toString(36).substr(2, 9);
    },

    debounce: function(func, wait) {
      let timeout;
      return function executedFunction(...args) {
        const later = () => {
          clearTimeout(timeout);
          func(...args);
        };
        clearTimeout(timeout);
        timeout = setTimeout(later, wait);
      };
    },

    throttle: function(func, limit) {
      let inThrottle;
      return function(...args) {
        if (!inThrottle) {
          func.apply(this, args);
          inThrottle = true;
          setTimeout(() => inThrottle = false, limit);
        }
      };
    }
  };

  // Event Emitter class
  class EventEmitter {
    constructor() {
      this.events = {};
    }

    on(event, callback) {
      if (!this.events[event]) {
        this.events[event] = [];
      }
      this.events[event].push(callback);
      return this;
    }

    off(event, callback) {
      if (this.events[event]) {
        this.events[event] = this.events[event].filter(cb => cb !== callback);
      }
      return this;
    }

    emit(event, ...args) {
      if (this.events[event]) {
        this.events[event].forEach(callback => callback.apply(this, args));
      }
      return this;
    }
  }

  // HTTP Client
  const HttpClient = {
    async request(url, options = {}) {
      const startTime = performance.now();
      
      const defaultOptions = {
        method: 'GET',
        headers: {
          'Content-Type': 'application/json'
        }
      };

      const mergedOptions = { ...defaultOptions, ...options };
      
      try {
        const response = await fetch(url, mergedOptions);
        const endTime = performance.now();
        
        Utils.log(`Request to ${url} completed in ${Utils.formatTime(endTime - startTime)}`);
        
        if (!response.ok) {
          throw new Error(`HTTP error! status: ${response.status}`);
        }
        
        return await response.json();
      } catch (error) {
        Utils.log(`Request failed: ${error.message}`, 'error');
        throw error;
      }
    },

    get(url) {
      return this.request(url);
    },

    post(url, data) {
      return this.request(url, {
        method: 'POST',
        body: JSON.stringify(data)
      });
    }
  };

  // Benchmark Statistics
  class BenchmarkStats extends EventEmitter {
    constructor() {
      super();
      this.reset();
    }

    reset() {
      this.requests = 0;
      this.errors = 0;
      this.totalLatency = 0;
      this.minLatency = Infinity;
      this.maxLatency = 0;
      this.latencies = [];
      this.startTime = null;
    }

    start() {
      this.startTime = performance.now();
      this.emit('start');
    }

    recordRequest(latency, success = true) {
      this.requests++;
      this.totalLatency += latency;
      this.latencies.push(latency);
      
      if (latency < this.minLatency) this.minLatency = latency;
      if (latency > this.maxLatency) this.maxLatency = latency;
      
      if (!success) this.errors++;
      
      this.emit('request', { latency, success });
    }

    getStats() {
      const duration = this.startTime ? (performance.now() - this.startTime) / 1000 : 0;
      const avgLatency = this.requests > 0 ? this.totalLatency / this.requests : 0;
      
      // Calculate percentiles
      const sorted = [...this.latencies].sort((a, b) => a - b);
      const p50 = sorted[Math.floor(sorted.length * 0.5)] || 0;
      const p95 = sorted[Math.floor(sorted.length * 0.95)] || 0;
      const p99 = sorted[Math.floor(sorted.length * 0.99)] || 0;

      return {
        requests: this.requests,
        errors: this.errors,
        duration: duration,
        rps: duration > 0 ? this.requests / duration : 0,
        avgLatency: avgLatency,
        minLatency: this.minLatency === Infinity ? 0 : this.minLatency,
        maxLatency: this.maxLatency,
        p50: p50,
        p95: p95,
        p99: p99
      };
    }
  }

  // Application initialization
  class App {
    constructor() {
      this.stats = new BenchmarkStats();
      this.initialized = false;
    }

    init() {
      if (this.initialized) return;
      
      Utils.log('Application initialized');
      this.setupEventListeners();
      this.initialized = true;
    }

    setupEventListeners() {
      document.addEventListener('DOMContentLoaded', () => {
        Utils.log('DOM loaded');
      });

      window.addEventListener('load', () => {
        Utils.log('Page fully loaded');
      });
    }

    getVersion() {
      return '1.0.0';
    }
  }

  // Create and expose global instance
  window.BenchmarkApp = new App();
  window.BenchmarkApp.init();

  // Export utilities for testing
  window.BenchmarkUtils = Utils;
  window.BenchmarkStats = BenchmarkStats;

})();
EOF

  # data.json (~32KB) - JSON data file
  python3 -c "
import json
data = {
    'metadata': {
        'version': '1.0.0',
        'generated': '2025-01-01T00:00:00Z',
        'description': 'Benchmark test data'
    },
    'items': []
}
for i in range(200):
    data['items'].append({
        'id': i,
        'name': f'Item {i}',
        'description': f'This is a description for item {i}. It contains some text to make the JSON file larger.',
        'value': i * 100,
        'enabled': i % 2 == 0,
        'tags': [f'tag{j}' for j in range(5)],
        'nested': {
            'level1': {
                'level2': {
                    'value': f'nested-value-{i}'
                }
            }
        }
    })
print(json.dumps(data, indent=2))
" > "$static_dir/data.json" 2>/dev/null || {
    # Fallback if python3 is not available
    log_warn "python3 not available, generating smaller data.json"
    echo '{"items":[]}' > "$static_dir/data.json"
  }

  # image.bin (~64KB) - Binary file
  dd if=/dev/urandom of="$static_dir/image.bin" bs=1024 count=64 2>/dev/null
  
  log_info "Static files generated:"
  for f in "$static_dir"/*; do
    local size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f" 2>/dev/null)
    log_info "  $(basename "$f"): $(echo "$size" | numfmt --to=iec 2>/dev/null || echo "${size} bytes")"
  done
  
  return 0
}

# ============================================================
# Main
# ============================================================
main() {
  log_info "Setting up benchmark resources in $OUTPUT_DIR"
  
  local cert_ok=0
  local static_ok=0
  
  generate_certificates && cert_ok=1
  generate_static_files && static_ok=1
  
  echo ""
  if [ $cert_ok -eq 1 ] && [ $static_ok -eq 1 ]; then
    log_info "All resources generated successfully!"
  else
    log_warn "Some resources could not be generated"
    [ $cert_ok -eq 0 ] && log_warn "  - Certificates: FAILED (openssl required)"
    [ $static_ok -eq 0 ] && log_warn "  - Static files: FAILED"
  fi
  
  return 0
}

main "$@"
