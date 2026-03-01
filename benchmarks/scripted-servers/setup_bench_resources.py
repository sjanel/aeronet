#!/usr/bin/env python3
"""Generate TLS certificates and static assets for scripted benchmark scenarios."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path

RESET = "\033[0m"
GREEN = "\033[0;32m"
YELLOW = "\033[1;33m"
RED = "\033[0;31m"


def _color(text: str, color: str) -> str:
    return f"{color}{text}{RESET}"


def log_info(message: str) -> None:
    print(f"{_color('[INFO]', GREEN)} {message}")


def log_warn(message: str) -> None:
    print(f"{_color('[WARN]', YELLOW)} {message}")


def log_error(message: str) -> None:
    print(f"{_color('[ERROR]', RED)} {message}")


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def format_size(num_bytes: int) -> str:
    units = ["bytes", "KB", "MB", "GB"]
    value = float(num_bytes)
    for unit in units:
        if value < 1024 or unit == units[-1]:
            if unit == "bytes":
                return f"{int(value)} {unit}"
            return f"{value:.2f} {unit}"
        value /= 1024
    return f"{num_bytes} bytes"


INDEX_HTML = """<!DOCTYPE html>
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
"""


STYLE_CSS = """/* Benchmark Test Stylesheet */
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
"""


APP_JS = """/**
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
"""


def write_text_file(path: Path, content: str) -> None:
    path.write_text(content.strip() + "\n", encoding="utf-8")


def generate_data_json(path: Path) -> None:
    data = {
        "metadata": {
            "version": "1.0.0",
            "generated": "2025-01-01T00:00:00Z",
            "description": "Benchmark test data",
        },
        "items": [],
    }
    for i in range(200):
        data["items"].append(
            {
                "id": i,
                "name": f"Item {i}",
                "description": f"This is a description for item {i}. It contains some text to make the JSON file larger.",
                "value": i * 100,
                "enabled": i % 2 == 0,
                "tags": [f"tag{j}" for j in range(5)],
                "nested": {"level1": {"level2": {"value": f"nested-value-{i}"}}},
            }
        )
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def generate_binary_blob(path: Path, size_kb: int = 64) -> None:
    path.write_bytes(os.urandom(size_kb * 1024))


def generate_static_files(output_dir: Path) -> bool:
    log_info(f"Generating static files in {output_dir / 'static'}")
    static_dir = output_dir / "static"
    ensure_dir(static_dir)
    try:
        write_text_file(static_dir / "index.html", INDEX_HTML)
        write_text_file(static_dir / "style.css", STYLE_CSS)
        write_text_file(static_dir / "app.js", APP_JS)
        generate_data_json(static_dir / "data.json")
        generate_binary_blob(static_dir / "image.bin")
    except Exception as exc:  # pragma: no cover - unexpected I/O failures
        log_error(f"Failed to generate static files: {exc}")
        return False

    log_info("Static files generated:")
    for entry in sorted(static_dir.iterdir()):
        if entry.is_file():
            size = entry.stat().st_size
            log_info(f"  {entry.name}: {format_size(size)}")
    return True


def generate_certificates(output_dir: Path) -> bool:
    cert_dir = output_dir / "certs"
    ensure_dir(cert_dir)
    cert_path = cert_dir / "server.crt"
    key_path = cert_dir / "server.key"
    p12_path = cert_dir / "server.p12"
    if cert_path.exists() and key_path.exists():
        log_info(f"Certificates already exist in {cert_dir}")
        # Still generate PKCS12 if missing (needed by Undertow/Java)
        if not p12_path.exists():
            _generate_pkcs12(cert_path, key_path, p12_path)
        return True
    openssl = shutil.which("openssl")
    if not openssl:
        log_error("openssl not found - cannot generate certificates")
        return False

    log_info(f"Generating TLS certificates in {cert_dir}...")
    try:
        subprocess.run(
            [openssl, "genrsa", "-out", str(key_path), "2048"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            [
                openssl,
                "req",
                "-new",
                "-x509",
                "-key",
                str(key_path),
                "-out",
                str(cert_path),
                "-days",
                "365",
                "-subj",
                "/C=XX/ST=Benchmark/L=Benchmark/O=Benchmark/CN=localhost",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        os.chmod(key_path, 0o600)
        os.chmod(cert_path, 0o644)
    except subprocess.CalledProcessError as exc:
        log_error(f"Failed to generate certificates: {exc}")
        return False

    _generate_pkcs12(cert_path, key_path, p12_path)
    log_info(f"Certificates generated in {cert_dir}")
    return True


def _generate_pkcs12(cert_path: Path, key_path: Path, p12_path: Path) -> None:
    """Generate a PKCS12 keystore from PEM cert/key (needed by Java/Undertow)."""
    openssl = shutil.which("openssl")
    if not openssl:
        log_error("openssl not found - cannot generate PKCS12 keystore")
        return
    try:
        subprocess.run(
            [
                openssl, "pkcs12", "-export",
                "-in", str(cert_path),
                "-inkey", str(key_path),
                "-out", str(p12_path),
                "-passout", "pass:benchmark",
                "-name", "benchmark",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        log_info(f"PKCS12 keystore generated: {p12_path}")
    except subprocess.CalledProcessError as exc:
        log_error(f"Failed to generate PKCS12 keystore: {exc}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate TLS certs and static assets for benchmarks"
    )
    parser.add_argument(
        "path",
        nargs="?",
        type=Path,
        help="Destination directory (defaults to script directory)",
    )
    parser.add_argument(
        "--output", dest="output", type=Path, help="Explicit destination directory"
    )
    return parser.parse_args()


def resolve_target(args: argparse.Namespace) -> Path:
    if args.output:
        return args.output.resolve()
    if args.path:
        return args.path.resolve()
    return Path(__file__).resolve().parent


def main() -> int:
    args = parse_args()
    target = resolve_target(args)
    ensure_dir(target)
    log_info(f"Setting up benchmark resources in {target}")
    cert_ok = generate_certificates(target)
    static_ok = generate_static_files(target)
    if cert_ok and static_ok:
        log_info("All resources generated successfully!")
    else:
        log_warn("Some resources could not be generated")
        if not cert_ok:
            log_warn("  - Certificates: FAILED (openssl required)")
        if not static_ok:
            log_warn("  - Static files: FAILED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
