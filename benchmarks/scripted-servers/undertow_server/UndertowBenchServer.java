// UndertowBenchServer.java - Undertow benchmark server for wrk testing
//
// Implements the same endpoints as aeronet_server.cpp for fair comparison.
// Build: javac -cp undertow-core.jar:xnio-api.jar:xnio-nio.jar UndertowBenchServer.java
// Run: java -cp .:undertow-core.jar:xnio-api.jar:xnio-nio.jar UndertowBenchServer [port] [threads]

import io.undertow.Undertow;
import io.undertow.UndertowOptions;
import io.undertow.io.Receiver;
import io.undertow.server.HttpHandler;
import io.undertow.server.HttpServerExchange;
import io.undertow.server.handlers.PathHandler;
import io.undertow.server.handlers.resource.FileResourceManager;
import io.undertow.server.handlers.resource.ResourceHandler;
import io.undertow.util.Headers;
import io.undertow.util.HttpString;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.Random;
import java.util.concurrent.ThreadLocalRandom;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.GZIPInputStream;
import java.util.zip.GZIPOutputStream;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManagerFactory;

public class UndertowBenchServer {
  private static final String CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  private static final Pattern USER_POST_PATTERN = Pattern.compile("^/users/([^/]+)/posts/([^/]+)$");
  private static final Pattern API_PATTERN =
      Pattern.compile("^/api/v1/resources/([^/]+)/items/([^/]+)/actions/([^/]+)$");

  public static void main(String[] args) {
    int port = getPort(args);
    int threads = getThreads(args);
    String staticDir = getStaticDir(args);
    int routeCount = getRouteCount(args);
    boolean h2Enabled = hasFlag(args, "--h2");
    boolean tlsEnabled = hasFlag(args, "--tls");
    String certFile = getFlagValue(args, "--cert");
    String keyFile = getFlagValue(args, "--key");

    PathHandler pathHandler =
        new PathHandler()
            // Endpoint 1: /ping - Minimal latency test
            .addExactPath("/ping",
                exchange -> {
                  exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
                  exchange.getResponseSender().send("pong");
                })

            // Endpoint 2: /headers - Header stress test
            .addExactPath("/headers",
                exchange -> {
                  int count = getQueryParamInt(exchange, "count", 10);
                  int size = getQueryParamInt(exchange, "size", 64);

                  for (int i = 0; i < count; i++) {
                    String name = "X-Bench-Header-" + i;
                    String value = randomString(size);
                    exchange.getResponseHeaders().put(new HttpString(name), value);
                  }
                  exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
                  exchange.getResponseSender().send("Generated " + count + " headers");
                })

            // Endpoint 3: /uppercase - Body uppercase echo test
            .addExactPath("/uppercase",
                exchange -> {
                  exchange.getRequestReceiver().receiveFullBytes((ex, data) -> {
                    // Convert to uppercase into a new byte array
                    byte[] out = new byte[data.length];
                    for (int i = 0; i < data.length; i++) {
                      out[i] = (byte) Character.toUpperCase((char) (data[i] & 0xFF));
                    }
                    ex.getResponseHeaders().put(Headers.CONTENT_TYPE, "application/octet-stream");
                    ex.getResponseSender().send(ByteBuffer.wrap(out));
                  });
                })

            // Endpoint 4: /compute - CPU-bound test
            .addExactPath("/compute",
                exchange -> {
                  int complexity = getQueryParamInt(exchange, "complexity", 30);
                  int hashIters = getQueryParamInt(exchange, "hash_iters", 1000);

                  long fibResult = fibonacci(complexity);
                  long hashResult = computeHash("benchmark-data-" + complexity, hashIters);

                  exchange.getResponseHeaders()
                      .put(new HttpString("X-Fib-Result"), String.valueOf(fibResult))
                      .put(new HttpString("X-Hash-Result"), String.valueOf(hashResult))
                      .put(Headers.CONTENT_TYPE, "text/plain");
                  exchange.getResponseSender().send(
                      String.format("fib(%d)=%d, hash=%d", complexity, fibResult, hashResult));
                })

            // Endpoint 5: /json - JSON response test
            .addExactPath("/json",
                exchange -> {
                  int items = getQueryParamInt(exchange, "items", 10);
                  StringBuilder json = new StringBuilder("{\"items\":[");
                  for (int i = 0; i < items; i++) {
                    if (i > 0)
                      json.append(",");
                    json.append(String.format("{\"id\":%d,\"name\":\"item-%d\",\"value\":%d}", i, i, i * 100));
                  }
                  json.append("]}");

                  exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "application/json");
                  exchange.getResponseSender().send(json.toString());
                })

            // Endpoint 6: /delay - Artificial delay test
            .addExactPath("/delay",
                exchange -> {
                  int delayMs = getQueryParamInt(exchange, "ms", 10);
                  try {
                    Thread.sleep(delayMs);
                  } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                  }
                  exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
                  exchange.getResponseSender().send("Delayed " + delayMs + " ms");
                })

            // Endpoint 7: /body - Variable size body test
            .addExactPath("/body",
                exchange -> {
                  int size = getQueryParamInt(exchange, "size", 1024);
                  exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
                  exchange.getResponseSender().send(randomString(size));
                })

            // Endpoint 7b: /body-codec - Gzip decode/encode stress test
            .addExactPath("/body-codec",
                exchange -> {
                  exchange.getRequestReceiver().receiveFullBytes((ex, data) -> {
                    byte[] body = data;
                    String enc = ex.getRequestHeaders().getFirst(Headers.CONTENT_ENCODING);
                    if (enc != null && enc.equalsIgnoreCase("gzip")) {
                      try {
                        body = decompressGzip(data);
                      } catch (IOException e) {
                        ex.setStatusCode(400);
                        ex.getResponseSender().send("Invalid gzip body");
                        return;
                      }
                    }

                    byte[] out = new byte[body.length];
                    for (int i = 0; i < body.length; i++) {
                      out[i] = (byte) (body[i] + 1);
                    }

                    ex.getResponseHeaders().put(Headers.CONTENT_TYPE, "application/octet-stream");

                    String accept = ex.getRequestHeaders().getFirst(Headers.ACCEPT_ENCODING);
                    if (accept != null && accept.toLowerCase().contains("gzip")) {
                      try {
                        byte[] compressed = compressGzip(out);
                        ex.getResponseHeaders().put(Headers.CONTENT_ENCODING, "gzip");
                        ex.getResponseHeaders().put(Headers.VARY, "Accept-Encoding");
                        ex.getResponseSender().send(ByteBuffer.wrap(compressed));
                      } catch (IOException e) {
                        ex.setStatusCode(500);
                        ex.getResponseSender().send("Compression failed");
                      }
                      return;
                    }

                    ex.getResponseSender().send(ByteBuffer.wrap(out));
                  });
                })

            // Endpoint 8: /status - Health check
            .addExactPath("/status", exchange -> {
              exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "application/json");
              exchange.getResponseSender().send(
                  String.format("{\"server\":\"undertow\",\"threads\":%d,\"h2\":%b,\"tls\":%b,\"status\":\"ok\"}",
                      threads, h2Enabled, tlsEnabled));
            });

    // Endpoint 9: /* - Static file serving via Undertow ResourceHandler
    if (staticDir != null && !staticDir.isEmpty()) {
      final Path basePath = Paths.get(staticDir).toAbsolutePath().normalize();
      // FileResourceManager(rootDir, cacheMillis) - use small cache window for benchmarks
      ResourceHandler resourceHandler =
          new ResourceHandler(new FileResourceManager(basePath.toFile(), 100)).setDirectoryListingEnabled(false);

      // Mount the resource handler at root; it will handle ranges, content-type, and efficient transfer
      pathHandler.addPrefixPath("/", resourceHandler);
    }

    // Endpoint 10: /r{N} - Routing stress test (literal routes)
    for (int i = 0; i < routeCount; i++) {
      final int routeNum = i;
      pathHandler.addExactPath("/r" + i, exchange -> {
        exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
        exchange.getResponseSender().send("route " + routeNum);
      });
    }

    // Pattern routes - use prefix handlers with regex matching
    pathHandler.addPrefixPath("/users", exchange -> {
      String path = exchange.getRequestPath();
      Matcher matcher = USER_POST_PATTERN.matcher(path);
      if (matcher.matches()) {
        String userId = matcher.group(1);
        String postId = matcher.group(2);
        exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
        exchange.getResponseSender().send("user " + userId + " post " + postId);
      } else {
        exchange.setStatusCode(404);
        exchange.getResponseSender().send("Not Found");
      }
    });

    pathHandler.addPrefixPath("/api/v1/resources", exchange -> {
      String path = exchange.getRequestPath();
      Matcher matcher = API_PATTERN.matcher(path);
      if (matcher.matches()) {
        String resource = matcher.group(1);
        String item = matcher.group(2);
        String action = matcher.group(3);
        exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "text/plain");
        exchange.getResponseSender().send("resource " + resource + " item " + item + " action " + action);
      } else {
        exchange.setStatusCode(404);
        exchange.getResponseSender().send("Not Found");
      }
    });

    Undertow.Builder builder =
        Undertow.builder().setIoThreads(threads).setWorkerThreads(threads).setHandler(pathHandler);

    if (h2Enabled) {
      builder.setServerOption(UndertowOptions.ENABLE_HTTP2, true);
    }

    if (tlsEnabled && certFile != null && keyFile != null) {
      try {
        SSLContext sslContext = createSSLContext(certFile, keyFile);
        builder.addHttpsListener(port, "127.0.0.1", sslContext);
      } catch (Exception e) {
        System.err.println("Failed to configure TLS: " + e.getMessage());
        System.exit(1);
      }
    } else {
      builder.addHttpListener(port, "127.0.0.1");
    }

    Undertow server = builder.build();

    String protocol = h2Enabled ? (tlsEnabled ? "h2-tls" : "h2c") : "http/1.1";
    System.out.println(
        "undertow benchmark server starting on port " + port + " with " + threads + " threads [" + protocol + "]");
    if (staticDir != null && !staticDir.isEmpty()) {
      System.out.println("Static files: " + staticDir);
    }
    if (routeCount > 0) {
      System.out.println("Routes: " + routeCount + " literal + pattern routes");
    }
    // Write pidfile for harness to find and stop this server
    String pidfile = "/tmp/bench_undertow.pid";
    try {
      String pid = java.lang.management.ManagementFactory.getRuntimeMXBean().getName().split("@")[0];
      java.nio.file.Files.write(java.nio.file.Paths.get(pidfile), pid.getBytes());
    } catch (Exception e) {
      // ignore
    }

    server.start();

    // Keep running and ensure pidfile is removed on shutdown
    Runtime.getRuntime().addShutdownHook(new Thread(() -> {
      try {
        System.out.println("Shutting down...");
        server.stop();
        java.nio.file.Files.deleteIfExists(java.nio.file.Paths.get(pidfile));
      } catch (Exception e) {
        // ignore
      }
    }));
  }

  private static int getPort(String[] args) {
    String envPort = System.getenv("BENCH_PORT");
    if (envPort != null) {
      return Integer.parseInt(envPort);
    }
    for (int i = 0; i < args.length - 1; i++) {
      if ("--port".equals(args[i])) {
        return Integer.parseInt(args[i + 1]);
      }
    }
    return 8082; // Different default port
  }

  private static int getThreads(String[] args) {
    String envThreads = System.getenv("BENCH_THREADS");
    if (envThreads != null) {
      return Integer.parseInt(envThreads);
    }
    for (int i = 0; i < args.length - 1; i++) {
      if ("--threads".equals(args[i])) {
        return Integer.parseInt(args[i + 1]);
      }
    }
    return Math.max(1, Runtime.getRuntime().availableProcessors() / 2);
  }

  private static String getStaticDir(String[] args) {
    for (int i = 0; i < args.length - 1; i++) {
      if ("--static".equals(args[i])) {
        return args[i + 1];
      }
    }
    return null;
  }

  private static boolean hasFlag(String[] args, String flag) {
    for (String arg : args) {
      if (flag.equals(arg)) {
        return true;
      }
    }
    return false;
  }

  private static String getFlagValue(String[] args, String flag) {
    for (int i = 0; i < args.length - 1; i++) {
      if (flag.equals(args[i])) {
        return args[i + 1];
      }
    }
    return null;
  }

  private static SSLContext createSSLContext(String certPath, String keyPath) throws Exception {
    // For benchmarks, use a PKCS12 keystore generated from the cert/key PEM files.
    // The run_benchmarks.py harness will generate a .p12 file alongside the PEM files.
    String p12Path = certPath.replace(".pem", ".p12").replace(".crt", ".p12");
    if (!new File(p12Path).exists()) {
      // Fall back: try same directory with bench.p12 name
      p12Path = new File(certPath).getParent() + "/bench.p12";
    }

    char[] password = "benchmark".toCharArray();
    KeyStore keyStore = KeyStore.getInstance("PKCS12");
    try (FileInputStream fis = new FileInputStream(p12Path)) {
      keyStore.load(fis, password);
    }

    KeyManagerFactory kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
    kmf.init(keyStore, password);

    TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
    tmf.init(keyStore);

    SSLContext sslContext = SSLContext.getInstance("TLS");
    sslContext.init(kmf.getKeyManagers(), tmf.getTrustManagers(), null);
    return sslContext;
  }

  private static int getRouteCount(String[] args) {
    for (int i = 0; i < args.length - 1; i++) {
      if ("--routes".equals(args[i])) {
        return Integer.parseInt(args[i + 1]);
      }
    }
    return 1000;
  }

  private static String getContentType(String path) {
    if (path.endsWith(".html"))
      return "text/html";
    if (path.endsWith(".css"))
      return "text/css";
    if (path.endsWith(".js"))
      return "application/javascript";
    if (path.endsWith(".json"))
      return "application/json";
    return "application/octet-stream";
  }

  private static int getQueryParamInt(HttpServerExchange exchange, String key, int defaultValue) {
    var params = exchange.getQueryParameters().get(key);
    if (params != null && !params.isEmpty()) {
      try {
        return Integer.parseInt(params.getFirst());
      } catch (NumberFormatException e) {
        return defaultValue;
      }
    }
    return defaultValue;
  }

  private static String randomString(int length) {
    Random random = ThreadLocalRandom.current();
    StringBuilder sb = new StringBuilder(length);
    for (int i = 0; i < length; i++) {
      sb.append(CHARSET.charAt(random.nextInt(CHARSET.length())));
    }
    return sb.toString();
  }

  private static long fibonacci(int n) {
    if (n <= 1)
      return n;
    long prev = 0, curr = 1;
    for (int i = 2; i <= n; i++) {
      long next = prev + curr;
      prev = curr;
      curr = next;
    }
    return curr;
  }

  private static long computeHash(String data, int iterations) {
    long hash = 0xcbf29ce484222325L; // FNV-1a offset basis
    byte[] bytes = data.getBytes();
    for (int iter = 0; iter < iterations; iter++) {
      for (byte b : bytes) {
        hash ^= (b & 0xFF);
        hash *= 0x100000001b3L; // FNV-1a prime
      }
    }
    return hash;
  }

  private static byte[] decompressGzip(byte[] data) throws IOException {
    try (InputStream in = new GZIPInputStream(new java.io.ByteArrayInputStream(data));
        ByteArrayOutputStream out = new ByteArrayOutputStream()) {
      byte[] buf = new byte[8192];
      int n;
      while ((n = in.read(buf)) > 0) {
        out.write(buf, 0, n);
      }
      return out.toByteArray();
    }
  }

  private static byte[] compressGzip(byte[] data) throws IOException {
    try (ByteArrayOutputStream out = new ByteArrayOutputStream(); GZIPOutputStream gz = new GZIPOutputStream(out)) {
      gz.write(data);
      gz.finish();
      return out.toByteArray();
    }
  }
}
