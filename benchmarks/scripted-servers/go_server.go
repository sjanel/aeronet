// go_server.go - Go benchmark server for wrk testing
//
// Uses the standard library net/http package which is the common choice.
// Build: go build -o go-bench-server go_server.go
// Run: ./go-bench-server [--port N] [--threads N]

package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

var numThreads int
var staticDir string
var routeCount int

// Pattern route matchers
var userPostPattern = regexp.MustCompile(`^/users/([^/]+)/posts/([^/]+)$`)
var apiPattern = regexp.MustCompile(`^/api/v1/resources/([^/]+)/items/([^/]+)/actions/([^/]+)$`)

func main() {
	port := getPort()
	numThreads = getThreads()
	staticDir = getStaticDir()
	routeCount = getRouteCount()

	// Limit Go scheduler parallelism to the requested count.
	// GOMAXPROCS only limits goroutine parallelism; Go's runtime creates
	// additional OS threads for sysmon, GC workers, and goroutines blocked in
	// syscalls (e.g. file I/O). These typically add 3-5 threads on top of
	// GOMAXPROCS. We intentionally do not hard-cap runtime threads because too
	// small limits can trigger fatal "thread exhaustion" under load.
	if numThreads > 0 {
		// reserve ~2 slot for sysmon + GC/netpoller
		procs := numThreads - 2
		if procs < 1 {
			procs = 1
		}
		runtime.GOMAXPROCS(procs)
	}

	// Build a deterministic router: literal route map + top-level handler
	literalRoutes := make(map[string]http.HandlerFunc)

	// Register literal endpoints
	literalRoutes["/ping"] = handlePing
	literalRoutes["/headers"] = handleHeaders
	literalRoutes["/uppercase"] = handleUppercase
	literalRoutes["/body-codec"] = handleBodyCodec
	literalRoutes["/compute"] = handleCompute
	literalRoutes["/json"] = handleJSON
	literalRoutes["/delay"] = handleDelay
	literalRoutes["/body"] = handleBody
	literalRoutes["/status"] = handleStatus

	if staticDir != "" {
		// serve static via path /
		literalRoutes["/"] = handleStatic
	}

	if routeCount > 0 {
		for i := 0; i < routeCount; i++ {
			idx := i // capture
			path := fmt.Sprintf("/r%d", i)
			literalRoutes[path] = func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/plain")
				w.Write([]byte(fmt.Sprintf("route %d", idx)))
			}
		}
	}

	// Top-level handler: check exact literal match first, then pattern routes, then static prefix
	topHandler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.Path

		// Exact literal match
		if h, ok := literalRoutes[path]; ok {
			h(w, r)
			return
		}

		// If static prefix registered, handle /... when literalRoutes has /static/
		if staticDir != "" {
			handleStatic(w, r)
			return
		}

		// Pattern routes
		if userPostPattern.MatchString(path) {
			handleUserPost(w, r)
			return
		}
		if apiPattern.MatchString(path) {
			handleApiPattern(w, r)
			return
		}

		// Fallback: check if a literal route was registered with trailing slash matching
		// (e.g., /static/ may be registered). Try prefix matches in the literalRoutes map.
		for lit, h := range literalRoutes {
			if strings.HasSuffix(lit, "/") && strings.HasPrefix(path, lit) {
				h(w, r)
				return
			}
		}

		http.NotFound(w, r)
	})

	server := &http.Server{
		Addr:           fmt.Sprintf("127.0.0.1:%d", port),
		Handler:        topHandler,
		ReadTimeout:    30 * time.Second,
		WriteTimeout:   30 * time.Second,
		MaxHeaderBytes: 256 * 1024, // 256KB headers for stress tests
	}

	fmt.Printf("go benchmark server starting on port %d with %d threads\n", port, numThreads)
	if staticDir != "" {
		fmt.Printf("Static files: %s\n", staticDir)
	}
	if routeCount > 0 {
		fmt.Printf("Routes: %d literal + pattern routes\n", routeCount)
	}
	if err := server.ListenAndServe(); err != nil {
		fmt.Fprintf(os.Stderr, "Server error: %v\n", err)
		os.Exit(1)
	}
}

func handlePing(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	w.Write([]byte("pong"))
}

func handleHeaders(w http.ResponseWriter, r *http.Request) {
	count := getQueryInt(r, "count", 10)
	size := getQueryInt(r, "size", 64)

	for i := 0; i < count; i++ {
		name := fmt.Sprintf("X-Bench-Header-%d", i)
		value := randomString(size)
		w.Header().Set(name, value)
	}
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "Generated %d headers", count)
}

func handleUppercase(w http.ResponseWriter, r *http.Request) {
	data, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "Failed to read body", http.StatusInternalServerError)
		return
	}

	for i := range data {
		b := data[i]
		if 'a' <= b && b <= 'z' {
			data[i] = b - ('a' - 'A')
		}
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	_, _ = w.Write(data)
}

func handleBodyCodec(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method Not Allowed", http.StatusMethodNotAllowed)
		return
	}
	var reader io.ReadCloser = r.Body
	if strings.Contains(r.Header.Get("Content-Encoding"), "gzip") {
		gz, err := gzip.NewReader(r.Body)
		if err != nil {
			http.Error(w, "Invalid gzip body", http.StatusBadRequest)
			return
		}
		reader = gz
		defer gz.Close()
	}
	defer reader.Close()

	data, err := io.ReadAll(reader)
	if err != nil {
		http.Error(w, "Failed to read body", http.StatusInternalServerError)
		return
	}
	for i := range data {
		data[i] = data[i] + 1
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	if strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
		var buf bytes.Buffer
		gz := gzip.NewWriter(&buf)
		if _, err := gz.Write(data); err != nil {
			_ = gz.Close()
			http.Error(w, "Compression failed", http.StatusInternalServerError)
			return
		}
		if err := gz.Close(); err != nil {
			http.Error(w, "Compression failed", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Encoding", "gzip")
		w.Header().Add("Vary", "Accept-Encoding")
		_, _ = w.Write(buf.Bytes())
		return
	}
	_, _ = w.Write(data)
}

func handleCompute(w http.ResponseWriter, r *http.Request) {
	complexity := getQueryInt(r, "complexity", 30)
	hashIters := getQueryInt(r, "hash_iters", 1000)

	fibResult := fibonacci(complexity)
	hashResult := computeHash(fmt.Sprintf("benchmark-data-%d", complexity), hashIters)

	w.Header().Set("X-Fib-Result", strconv.FormatUint(fibResult, 10))
	w.Header().Set("X-Hash-Result", strconv.FormatUint(hashResult, 10))
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "fib(%d)=%d, hash=%d", complexity, fibResult, hashResult)
}

func handleJSON(w http.ResponseWriter, r *http.Request) {
	items := getQueryInt(r, "items", 10)

	type Item struct {
		ID    int    `json:"id"`
		Name  string `json:"name"`
		Value int    `json:"value"`
	}
	type Response struct {
		Items []Item `json:"items"`
	}

	resp := Response{Items: make([]Item, items)}
	for i := 0; i < items; i++ {
		resp.Items[i] = Item{
			ID:    i,
			Name:  fmt.Sprintf("item-%d", i),
			Value: i * 100,
		}
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func handleDelay(w http.ResponseWriter, r *http.Request) {
	delayMs := getQueryInt(r, "ms", 10)
	time.Sleep(time.Duration(delayMs) * time.Millisecond)
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "Delayed %d ms", delayMs)
}

func handleBody(w http.ResponseWriter, r *http.Request) {
	size := getQueryInt(r, "size", 1024)
	w.Header().Set("Content-Type", "text/plain")
	w.Write([]byte(randomString(size)))
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"server":"go","threads":%d,"status":"ok"}`, numThreads)
}

func handleStatic(w http.ResponseWriter, r *http.Request) {
	// Strip / prefix
	filePath := strings.TrimPrefix(r.URL.Path, "/")
	if filePath == "" {
		http.NotFound(w, r)
		return
	}
	decoded := filepath.Clean("/" + filePath)
	if decoded == "/" {
		http.NotFound(w, r)
		return
	}
	fullPath := filepath.Join(staticDir, decoded)

	absStatic, _ := filepath.Abs(staticDir)
	absPath, _ := filepath.Abs(fullPath)
	if !strings.HasPrefix(absPath, absStatic) {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}

	file, err := os.Open(fullPath)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || info.IsDir() {
		http.NotFound(w, r)
		return
	}

	contentType := getContentType(decoded)
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Content-Length", fmt.Sprintf("%d", info.Size()))
	http.ServeContent(w, r, info.Name(), info.ModTime(), file)
}

func handleUserPost(w http.ResponseWriter, r *http.Request) {
	matches := userPostPattern.FindStringSubmatch(r.URL.Path)
	if matches == nil {
		http.NotFound(w, r)
		return
	}
	fmt.Fprintf(w, "user %s post %s", matches[1], matches[2])
}

func handleApiPattern(w http.ResponseWriter, r *http.Request) {
	matches := apiPattern.FindStringSubmatch(r.URL.Path)
	if matches == nil {
		http.NotFound(w, r)
		return
	}
	fmt.Fprintf(w, "resource %s item %s action %s", matches[1], matches[2], matches[3])
}

func getContentType(path string) string {
	switch {
	case strings.HasSuffix(path, ".html"):
		return "text/html"
	case strings.HasSuffix(path, ".css"):
		return "text/css"
	case strings.HasSuffix(path, ".js"):
		return "application/javascript"
	case strings.HasSuffix(path, ".json"):
		return "application/json"
	default:
		return "application/octet-stream"
	}
}

func getPort() int {
	if envPort := os.Getenv("BENCH_PORT"); envPort != "" {
		if port, err := strconv.Atoi(envPort); err == nil {
			return port
		}
	}
	for i, arg := range os.Args {
		if arg == "--port" && i+1 < len(os.Args) {
			if port, err := strconv.Atoi(os.Args[i+1]); err == nil {
				return port
			}
		}
	}
	return 8083 // Different default port
}

func getThreads() int {
	if envThreads := os.Getenv("BENCH_THREADS"); envThreads != "" {
		if threads, err := strconv.Atoi(envThreads); err == nil {
			return threads
		}
	}
	for i, arg := range os.Args {
		if arg == "--threads" && i+1 < len(os.Args) {
			if threads, err := strconv.Atoi(os.Args[i+1]); err == nil {
				return threads
			}
		}
	}
	cpus := runtime.NumCPU()
	if cpus > 1 {
		return cpus / 2
	}
	return 1
}

func getStaticDir() string {
	for i, arg := range os.Args {
		if arg == "--static" && i+1 < len(os.Args) {
			return os.Args[i+1]
		}
	}
	return ""
}

func getRouteCount() int {
	for i, arg := range os.Args {
		if arg == "--routes" && i+1 < len(os.Args) {
			if count, err := strconv.Atoi(os.Args[i+1]); err == nil {
				return count
			}
		}
	}
	return 1000
}

func getQueryInt(r *http.Request, key string, defaultValue int) int {
	if val := r.URL.Query().Get(key); val != "" {
		if n, err := strconv.Atoi(val); err == nil {
			return n
		}
	}
	return defaultValue
}

func randomString(length int) string {
	b := make([]byte, length)
	for i := range b {
		b[i] = charset[rand.Intn(len(charset))]
	}
	return string(b)
}

func fibonacci(n int) uint64 {
	if n <= 1 {
		return uint64(n)
	}
	var prev, curr uint64 = 0, 1
	for i := 2; i <= n; i++ {
		prev, curr = curr, prev+curr
	}
	return curr
}

func computeHash(data string, iterations int) uint64 {
	hash := uint64(0xcbf29ce484222325) // FNV-1a offset basis
	bytes := []byte(data)
	for iter := 0; iter < iterations; iter++ {
		for _, b := range bytes {
			hash ^= uint64(b)
			hash *= 0x100000001b3 // FNV-1a prime
		}
	}
	return hash
}
