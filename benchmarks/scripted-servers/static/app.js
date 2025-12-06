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
