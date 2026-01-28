//! Rust benchmark server using Axum framework
//!
//! Implements standard benchmark endpoints for comparison with other frameworks.

use axum::{
    body::{Body, Bytes},
    extract::{Path, Query, State},
    http::{header::CONTENT_TYPE, HeaderMap, HeaderName, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Router,
};
use flate2::{read::GzDecoder, write::GzEncoder, Compression};
use rand::{distributions::Alphanumeric, Rng};
use serde::{Deserialize, Serialize};
use std::{
    env,
    io::{Read, Write},
    net::SocketAddr,
    path::{Component, Path as StdPath, PathBuf},
    time::Duration,
};
use tokio::{fs, time::sleep};

/// CPU-bound Fibonacci computation
fn fibonacci(n: u32) -> u64 {
    if n <= 1 {
        return n as u64;
    }
    let mut prev = 0u64;
    let mut curr = 1u64;
    for _ in 2..=n {
        let next = prev.wrapping_add(curr);
        prev = curr;
        curr = next;
    }
    curr
}

/// FNV-1a hash computation for CPU stress
fn compute_hash(data: &str, iterations: u32) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for _ in 0..iterations {
        for byte in data.bytes() {
            hash ^= byte as u64;
            hash = hash.wrapping_mul(0x100000001b3u64);
        }
    }
    hash
}

/// Generate random alphanumeric string
fn random_string(length: usize) -> String {
    rand::thread_rng()
        .sample_iter(&Alphanumeric)
        .take(length)
        .map(char::from)
        .collect()
}

/// GET /ping - Simple availability check
async fn ping() -> &'static str {
    "pong"
}

#[derive(Deserialize)]
struct HeadersParams {
    count: Option<usize>,
    size: Option<usize>,
}

/// GET /headers - Response with custom headers
async fn headers(Query(params): Query<HeadersParams>) -> Response {
    let count = params.count.unwrap_or(10);
    let size = params.size.unwrap_or(64);

    let mut headers = HeaderMap::new();
    for i in 0..count {
        let name = format!("x-bench-header-{}", i);
        if let (Ok(header_name), Ok(header_value)) = (
            HeaderName::try_from(name),
            HeaderValue::try_from(random_string(size)),
        ) {
            headers.insert(header_name, header_value);
        }
    }

    let body = format!("Generated {} headers", count);
    (StatusCode::OK, headers, body).into_response()
}

/// POST /uppercase - Echo request body back (force allocate a new string)
async fn uppercase(body: String) -> String {
    // Force a new allocation and some work so benchmarks are comparable:
    // convert to upper-case which returns a new String
    body.to_uppercase()
}

#[derive(Deserialize)]
struct ComputeParams {
    complexity: Option<u32>,
    hash_iters: Option<u32>,
}

/// GET /compute - CPU-bound computation endpoint
async fn compute(Query(params): Query<ComputeParams>) -> Response {
    let complexity = params.complexity.unwrap_or(30);
    let hash_iters = params.hash_iters.unwrap_or(1000);

    let fib_result = fibonacci(complexity);
    let data = format!("benchmark-data-{}", complexity);
    let hash_result = compute_hash(&data, hash_iters);

    let mut headers = HeaderMap::new();
    if let Ok(v) = HeaderValue::try_from(fib_result.to_string()) {
        headers.insert("x-fib-result", v);
    }
    if let Ok(v) = HeaderValue::try_from(hash_result.to_string()) {
        headers.insert("x-hash-result", v);
    }

    let body = format!("fib({})={}, hash={}", complexity, fib_result, hash_result);
    (StatusCode::OK, headers, body).into_response()
}

#[derive(Deserialize)]
struct JsonParams {
    items: Option<usize>,
}

#[derive(Serialize)]
struct JsonItem {
    id: usize,
    name: String,
    value: usize,
}

#[derive(Serialize)]
struct JsonResponse {
    items: Vec<JsonItem>,
}

/// GET /json - JSON response generation
async fn json_endpoint(Query(params): Query<JsonParams>) -> axum::Json<JsonResponse> {
    let count = params.items.unwrap_or(10);

    let items: Vec<JsonItem> = (0..count)
        .map(|i| JsonItem {
            id: i,
            name: format!("item-{}", i),
            value: i * 100,
        })
        .collect();

    axum::Json(JsonResponse { items })
}

#[derive(Deserialize)]
struct DelayParams {
    ms: Option<u64>,
}

/// GET /delay - Artificial delay endpoint
async fn delay(Query(params): Query<DelayParams>) -> String {
    let delay_ms = params.ms.unwrap_or(10);
    sleep(Duration::from_millis(delay_ms)).await;
    format!("Delayed {} ms", delay_ms)
}

#[derive(Deserialize)]
struct BodyParams {
    size: Option<usize>,
}

/// GET /body - Variable-size response body
async fn body(Query(params): Query<BodyParams>) -> String {
    let size = params.size.unwrap_or(1024);
    random_string(size)
}

/// POST /body-codec - Gzip decode/encode stress test
async fn body_codec(headers: HeaderMap, body: Bytes) -> Response {
    let mut data = if let Some(enc) = headers.get("content-encoding") {
        let enc = enc.to_str().unwrap_or("");
        if enc.to_ascii_lowercase().contains("gzip") {
            let mut decoder = GzDecoder::new(body.as_ref());
            let mut decoded = Vec::new();
            if decoder.read_to_end(&mut decoded).is_err() {
                return Response::builder()
                    .status(StatusCode::BAD_REQUEST)
                    .body(Body::from("Invalid gzip body"))
                    .unwrap();
            }
            decoded
        } else {
            body.to_vec()
        }
    } else {
        body.to_vec()
    };

    for byte in data.iter_mut() {
        *byte = byte.wrapping_add(1);
    }

    let mut response = Response::builder()
        .status(StatusCode::OK)
        .header(CONTENT_TYPE, "application/octet-stream");

    let accept = headers
        .get("accept-encoding")
        .and_then(|v| v.to_str().ok())
        .unwrap_or("");
    if accept.to_ascii_lowercase().contains("gzip") {
        let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
        if encoder.write_all(&data).is_err() {
            return Response::builder()
                .status(StatusCode::INTERNAL_SERVER_ERROR)
                .body(Body::from("Compression failed"))
                .unwrap();
        }
        let compressed = match encoder.finish() {
            Ok(buf) => buf,
            Err(_) => {
                return Response::builder()
                    .status(StatusCode::INTERNAL_SERVER_ERROR)
                    .body(Body::from("Compression failed"))
                    .unwrap();
            }
        };
        response = response
            .header("content-encoding", "gzip")
            .header("vary", "Accept-Encoding");
        return response.body(Body::from(compressed)).unwrap();
    }

    response.body(Body::from(data)).unwrap()
}

/// GET /status - Server status endpoint
async fn status() -> axum::Json<serde_json::Value> {
    axum::Json(serde_json::json!({
        "server": "rust-axum",
        "status": "ok"
    }))
}

/// Route handler for /r{N} literal routes
/// Pattern route: /users/{id}/posts/{post}
async fn user_post(Path((user_id, post_id)): Path<(String, String)>) -> String {
    format!("user {} post {}", user_id, post_id)
}

/// Pattern route: /api/v1/resources/{resource}/items/{item}/actions/{action}
async fn api_pattern(Path((resource, item, action)): Path<(String, String, String)>) -> String {
    format!("resource {} item {} action {}", resource, item, action)
}

/// Shared application state
#[derive(Clone)]
struct AppState {
    static_dir: Option<PathBuf>,
}

fn get_port() -> u16 {
    env::var("BENCH_PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(8086)
}

fn get_threads() -> usize {
    env::var("BENCH_THREADS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4))
}

fn main() {
    let threads = get_threads();
    
    // Build a multi-threaded runtime with the specified number of worker threads
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(threads)
        .max_blocking_threads(threads)
        .enable_all()
        .build()
        .expect("Failed to create Tokio runtime");
    
    runtime.block_on(async_main(threads));
}

async fn async_main(threads: usize) {
    let port = get_port();

    // Parse command line arguments
    let args: Vec<String> = env::args().collect();
    let mut port_override = None;
    let mut static_dir: Option<PathBuf> = None;
    let mut route_count: usize = 0;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" if i + 1 < args.len() => {
                port_override = args[i + 1].parse().ok();
                i += 2;
            }
            "--static" if i + 1 < args.len() => {
                static_dir = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--routes" if i + 1 < args.len() => {
                route_count = args[i + 1].parse().unwrap_or(1000);
                i += 2;
            }
            "--help" | "-h" => {
                println!(
                    "Usage: {} [options]\n\
                     Options:\n  \
                       --port N      Listen port (default: 8086, env: BENCH_PORT)\n  \
                       --static DIR  Static files directory\n  \
                       --routes N    Number of /r{{N}} routes\n  \
                       --help        Show this help",
                    args[0]
                );
                return;
            }
            _ => i += 1,
        }
    }

    let port = port_override.unwrap_or(port);
    let addr = SocketAddr::from(([127, 0, 0, 1], port));

    let app_state = AppState { static_dir: static_dir.clone() };

    let mut app = Router::new()
        .route("/ping", get(ping))
        .route("/headers", get(headers))
        .route("/uppercase", post(uppercase))
        .route("/body-codec", post(body_codec))
        .route("/compute", get(compute))
        .route("/json", get(json_endpoint))
        .route("/delay", get(delay))
        .route("/body", get(body))
        .route("/status", get(status));

    // Add static file serving if configured
    if static_dir.is_some() {
        app = app.route("/*file", get(static_file));
    }

    // Add routing stress routes if configured
    if route_count > 0 {
        for i in 0..route_count {
            let path = format!("/r{}", i);
            app = app.route(&path, get(move || async move { format!("route {}", i) }));
        }
        app = app
            .route("/users/:user_id/posts/:post_id", get(user_post))
            .route("/api/v1/resources/:resource/items/:item/actions/:action", get(api_pattern));
    }

    let app = app.with_state(app_state);

    println!("rust-axum benchmark server starting on port {} with {} threads", port, threads);
    if let Some(ref dir) = static_dir {
        println!("Static files: {:?}", dir);
    }
    if route_count > 0 {
        println!("Routes: {} literal + pattern routes", route_count);
    }

    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}

async fn static_file(
    State(state): State<AppState>,
    Path(file_path): Path<String>,
) -> Response {
    let base_dir = match &state.static_dir {
        Some(dir) => dir.clone(),
        None => return StatusCode::NOT_FOUND.into_response(),
    };

    let rel_path = StdPath::new(&file_path);
    let mut sanitized = base_dir.clone();
    for component in rel_path.components() {
        match component {
            Component::Normal(part) => sanitized.push(part),
            Component::CurDir | Component::RootDir => continue,
            _ => return StatusCode::FORBIDDEN.into_response(),
        }
    }

    if !sanitized.starts_with(&base_dir) {
        return StatusCode::FORBIDDEN.into_response();
    }


    match fs::read(&sanitized).await {
        Ok(content) => {
            let mime = match sanitized.extension().and_then(|ext| ext.to_str()) {
                Some("html") => "text/html",
                Some("css") => "text/css",
                Some("js") => "application/javascript",
                Some("json") => "application/json",
                _ => "application/octet-stream",
            };

            Response::builder()
                .status(StatusCode::OK)
                .header(CONTENT_TYPE, mime)
                .body(Body::from(content))
                .unwrap()
        }
        Err(_) => StatusCode::NOT_FOUND.into_response(),
    }
}
