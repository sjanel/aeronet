#!/usr/bin/env python3
"""
python_server.py - Python benchmark server for wrk testing

Uses uvicorn + starlette which is the common high-performance choice for Python.
Install: pip install uvicorn starlette aiofiles
Run: python python_server.py [--port N] [--threads N] [--static DIR] [--routes N]
     or: uvicorn python_server:app --host 127.0.0.1 --port 8084 --workers N
"""

import argparse
import json
import os
import random
import re
import string
import time
from pathlib import Path
from typing import Optional
import sys

try:
    from starlette.applications import Starlette
    from starlette.responses import (
        PlainTextResponse,
        JSONResponse,
        Response,
        FileResponse,
    )
    from starlette.routing import Route, Mount
    from starlette.requests import Request
    from starlette.staticfiles import StaticFiles
except ImportError:
    print("ERROR: starlette not installed. Run: pip install starlette uvicorn")
    exit(1)

CHARSET = string.ascii_letters + string.digits
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
num_threads = 1
static_dir = ""
route_count = 0


def random_string(length: int) -> str:
    """Generate random alphanumeric string."""
    return "".join(random.choices(CHARSET, k=length))


def fibonacci(n: int) -> int:
    """Compute nth Fibonacci number iteratively."""
    if n <= 1:
        return n
    prev, curr = 0, 1
    for _ in range(2, n + 1):
        prev, curr = curr, prev + curr
    return curr


def compute_hash(data: str, iterations: int) -> int:
    """FNV-1a hash computation."""
    hash_val = 0xCBF29CE484222325  # FNV-1a offset basis
    data_bytes = data.encode("utf-8")
    for _ in range(iterations):
        for b in data_bytes:
            hash_val ^= b
            hash_val *= 0x100000001B3  # FNV-1a prime
            hash_val &= 0xFFFFFFFFFFFFFFFF  # Keep 64-bit
    return hash_val


def get_query_int(request: Request, key: str, default: int) -> int:
    """Get integer query parameter with default."""
    try:
        return int(request.query_params.get(key, default))
    except (ValueError, TypeError):
        return default


# Endpoint handlers
async def ping(request: Request) -> Response:
    """Endpoint 1: /ping - Minimal latency test"""
    return PlainTextResponse("pong")


async def headers(request: Request) -> Response:
    """Endpoint 2: /headers - Header stress test"""
    count = get_query_int(request, "count", 10)
    size = get_query_int(request, "size", 64)

    response_headers = {}
    for i in range(count):
        name = f"X-Bench-Header-{i}"
        value = random_string(size)
        response_headers[name] = value

    return PlainTextResponse(f"Generated {count} headers", headers=response_headers)


async def uppercase(request: Request) -> Response:
    """Endpoint 3: /uppercase - Body uppercase test"""
    body = await request.body()
    try:
        text = body.decode("utf-8")
        out = text.upper().encode("utf-8")
    except Exception:
        # If not valid UTF-8, fallback to returning original bytes
        out = body
    return Response(content=out, media_type="application/octet-stream")


async def compute(request: Request) -> Response:
    """Endpoint 4: /compute - CPU-bound test"""
    complexity = get_query_int(request, "complexity", 30)
    hash_iters = get_query_int(request, "hash_iters", 1000)

    fib_result = fibonacci(complexity)
    hash_result = compute_hash(f"benchmark-data-{complexity}", hash_iters)

    return PlainTextResponse(
        f"fib({complexity})={fib_result}, hash={hash_result}",
        headers={"X-Fib-Result": str(fib_result), "X-Hash-Result": str(hash_result)},
    )


async def json_endpoint(request: Request) -> Response:
    """Endpoint 5: /json - JSON response test"""
    items = get_query_int(request, "items", 10)

    data = {
        "items": [
            {"id": i, "name": f"item-{i}", "value": i * 100} for i in range(items)
        ]
    }

    return JSONResponse(data)


async def delay(request: Request) -> Response:
    """Endpoint 6: /delay - Artificial delay test"""
    delay_ms = get_query_int(request, "ms", 10)
    time.sleep(delay_ms / 1000.0)
    return PlainTextResponse(f"Delayed {delay_ms} ms")


async def body(request: Request) -> Response:
    """Endpoint 7: /body - Variable size body test"""
    size = get_query_int(request, "size", 1024)
    return PlainTextResponse(random_string(size))


async def status(request: Request) -> Response:
    """Endpoint 8: /status - Health check"""
    threads = int(os.environ.get("BENCH_THREADS", str(num_threads)))
    return JSONResponse({"server": "python", "threads": threads, "status": "ok"})


async def route_handler(request: Request) -> Response:
    """Endpoint 10: /r{N} - Routing stress test literal routes"""
    route_num = request.path_params.get("num", "0")
    return PlainTextResponse(f"route {route_num}")


async def user_post_handler(request: Request) -> Response:
    """Pattern route: /users/{id}/posts/{post}"""
    user_id = request.path_params.get("user_id", "")
    post_id = request.path_params.get("post_id", "")
    return PlainTextResponse(f"user {user_id} post {post_id}")


async def api_pattern_handler(request: Request) -> Response:
    """Pattern route: /api/v1/resources/{resource}/items/{item}/actions/{action}"""
    resource = request.path_params.get("resource", "")
    item = request.path_params.get("item", "")
    action = request.path_params.get("action", "")
    return PlainTextResponse(f"resource {resource} item {item} action {action}")


# Create Starlette app with routes
def create_routes():
    """Build route list dynamically based on configuration."""
    base_routes = [
        Route("/ping", ping, methods=["GET"]),
        Route("/headers", headers, methods=["GET"]),
        Route("/uppercase", uppercase, methods=["POST"]),
        Route("/compute", compute, methods=["GET"]),
        Route("/json", json_endpoint, methods=["GET"]),
        Route("/delay", delay, methods=["GET"]),
        Route("/body", body, methods=["GET"]),
        Route("/status", status, methods=["GET"]),
    ]

    # Add static file serving if configured
    env_static = os.environ.get("BENCH_STATIC_DIR", "")
    if env_static and Path(env_static).is_dir():
        base_routes.append(Mount("/", StaticFiles(directory=env_static), name="static"))

    # Add routing stress routes if configured
    env_routes = int(os.environ.get("BENCH_ROUTE_COUNT", "0"))
    if env_routes > 0:
        for i in range(env_routes):
            base_routes.append(Route(f"/r{i}", route_handler, methods=["GET"]))
        base_routes.append(
            Route(
                "/users/{user_id}/posts/{post_id}", user_post_handler, methods=["GET"]
            )
        )
        base_routes.append(
            Route(
                "/api/v1/resources/{resource}/items/{item}/actions/{action}",
                api_pattern_handler,
                methods=["GET"],
            )
        )

    return base_routes


routes = create_routes()

app = Starlette(routes=routes)


def get_port() -> int:
    """Get port from environment or arguments."""
    if env_port := os.environ.get("BENCH_PORT"):
        return int(env_port)
    return 8084  # Different default port


def get_threads() -> int:
    """Get thread count from environment or arguments."""
    if env_threads := os.environ.get("BENCH_THREADS"):
        return int(env_threads)
    cpu_count = os.cpu_count() or 2
    return max(1, cpu_count // 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Python benchmark server")
    parser.add_argument("--port", type=int, default=None, help="Listen port")
    parser.add_argument("--threads", type=int, default=None, help="Worker threads")
    parser.add_argument(
        "--static", type=str, default=None, help="Static files directory"
    )
    parser.add_argument(
        "--routes", type=int, default=None, help="Number of /r{N} routes"
    )
    args = parser.parse_args()

    port = args.port or get_port()
    num_threads = args.threads or get_threads()
    static_dir = args.static or ""
    route_count = args.routes or 0

    try:
        import uvicorn
    except ImportError:
        print("ERROR: uvicorn not installed. Run: pip install uvicorn")
        exit(1)

    print(f"python benchmark server starting on port {port} with {num_threads} workers")
    if static_dir:
        print(f"Static files: {static_dir}")
    if route_count > 0:
        print(f"Routes: {route_count} literal + pattern routes")

    # Set environment variables for worker processes and app configuration
    os.environ["BENCH_THREADS"] = str(num_threads)
    os.environ["BENCH_PORT"] = str(port)
    if static_dir:
        os.environ["BENCH_STATIC_DIR"] = static_dir
    if route_count > 0:
        os.environ["BENCH_ROUTE_COUNT"] = str(route_count)

    # PID file so harness can find and stop the server
    pidfile = f"/tmp/bench_python.pid"
    try:
        with open(pidfile, "w") as f:
            f.write(str(os.getpid()))
    except Exception:
        pass

    # Ensure pidfile is removed on exit
    import atexit

    def _cleanup_pidfile():
        try:
            if os.path.exists(pidfile):
                os.remove(pidfile)
        except Exception:
            pass

    atexit.register(_cleanup_pidfile)

    # Install basic signal handlers to allow graceful shutdown
    import signal

    def _handle_term(signum, frame):
        try:
            _cleanup_pidfile()
        finally:
            # allow default handling to exit
            sys.exit(0)

    signal.signal(signal.SIGINT, _handle_term)
    signal.signal(signal.SIGTERM, _handle_term)

    # If multiple workers are requested, run the uvicorn CLI with an import string
    # so worker mode works correctly. When uvicorn imports this module it will
    # pick up environment variables so routes are configured correctly.
    if int(num_threads) > 1:
        print("launching uvicorn CLI (will replace this process)")
        pythonpath = os.environ.get("PYTHONPATH", "")
        script_path = str(SCRIPT_DIR)
        paths = [p for p in pythonpath.split(os.pathsep) if p] if pythonpath else []
        if script_path not in paths:
            paths.insert(0, script_path)
            os.environ["PYTHONPATH"] = os.pathsep.join(paths)
        os.chdir(script_path)
        os.execvp(
            sys.executable,
            [
                sys.executable,
                "-m",
                "uvicorn",
                "python_server:app",
                "--host",
                "127.0.0.1",
                "--port",
                str(port),
                "--workers",
                str(num_threads),
                "--log-level",
                "warning",
            ],
        )

    uvicorn.run(app, host="127.0.0.1", port=port, log_level="warning", access_log=False)
