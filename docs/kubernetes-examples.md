# Kubernetes Deployment Guide for aeronet

This guide provides end-to-end Kubernetes examples for deploying an aeronet server with:

- a ConfigMap-managed server configuration
- built-in probe endpoints (`/livez`, `/readyz`, `/startupz`)
- liveness, readiness, and startup probes in `Deployment`
- an optional `Service`

You will find two configuration styles:

1. ConfigMap with YAML config and comments
2. ConfigMap with JSON config without comments

These examples are templates. Replace image names, command/args, ports, and resource values for your workload.

## Prerequisites

1. A container image that runs your aeronet server.
2. Your server process reads config from a file path (examples use `/etc/aeronet/server.yaml` or `/etc/aeronet/server.json`).
3. Built-in probes enabled in the config.

For probe behavior and options, see [FEATURES.md](FEATURES.md#built-in-kubernetes-style-probes).

## Generate a Baseline Config File

The `examples` directory includes a small CLI tool that dumps aeronet's default full configuration
(`server` + `router`) in JSON or YAML. This is useful as a starting point for clients before turning
the content into a Kubernetes ConfigMap.

Build and run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_ENABLE_GLAZE=ON
cmake --build build --target aeronet-config-dump

# Print YAML to stdout
./build/examples/aeronet-config-dump --format yaml

# Write JSON to a file
./build/examples/aeronet-config-dump --format json --output server.json
```

Then place the generated file content in the ConfigMap (`server.yaml` or `server.json`) and tune values
for your environment (ports, limits, TLS, telemetry, probes, and routing defaults).

## Option A: ConfigMap with YAML Config and Comments

The manifest below contains:

- one `ConfigMap` with a commented YAML server config
- one `Deployment` mounting that config file
- one `Service`

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: aeronet-config-yaml
data:
  # This file is mounted into the container and read by your server process.
  server.yaml: |
    server:
      # Listen port used by the pod.
      port: 8080

      # Enable built-in Kubernetes-style probes.
      builtinProbes:
        enabled: true
        livenessPath: /livez
        readinessPath: /readyz
        startupPath: /startupz
        contentType: text/plain

      # Optional HTTP behavior tuning.
      enableKeepAlive: true
      keepAliveTimeout: 5000ms

      # Optional telemetry example.
      telemetry:
        otelEnabled: false
        dogStatsDEnabled: false

---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: aeronet-example-yaml
  labels:
    app: aeronet-example-yaml
spec:
  replicas: 2
  selector:
    matchLabels:
      app: aeronet-example-yaml
  template:
    metadata:
      labels:
        app: aeronet-example-yaml
    spec:
      containers:
        - name: aeronet
          image: your-registry/aeronet:latest
          imagePullPolicy: IfNotPresent

          # Adjust command/args to your image entrypoint contract.
          args: ["--config", "/etc/aeronet/server.yaml"]

          ports:
            - name: http
              containerPort: 8080

          volumeMounts:
            - name: aeronet-config
              mountPath: /etc/aeronet
              readOnly: true

          # Liveness checks process health.
          livenessProbe:
            httpGet:
              path: /livez
              port: http
            initialDelaySeconds: 10
            periodSeconds: 10
            timeoutSeconds: 2
            failureThreshold: 3

          # Readiness controls whether pod receives traffic.
          readinessProbe:
            httpGet:
              path: /readyz
              port: http
            initialDelaySeconds: 5
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 3

          # Startup probe protects slow initialization paths.
          startupProbe:
            httpGet:
              path: /startupz
              port: http
            initialDelaySeconds: 2
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 30

          resources:
            requests:
              cpu: "100m"
              memory: "128Mi"
            limits:
              cpu: "500m"
              memory: "256Mi"

      volumes:
        - name: aeronet-config
          configMap:
            name: aeronet-config-yaml

---
apiVersion: v1
kind: Service
metadata:
  name: aeronet-example-yaml
spec:
  selector:
    app: aeronet-example-yaml
  ports:
    - name: http
      port: 80
      targetPort: http
  type: ClusterIP
```

Apply it with:

```bash
kubectl apply -f aeronet-yaml-example.yaml
```

## Option B: ConfigMap with JSON Config Without Comments

JSON does not support comments. The example below intentionally keeps the JSON payload comment-free.

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: aeronet-config-json
data:
  server.json: |
    {
      "server": {
        "port": 8080,
        "enableKeepAlive": true,
        "keepAliveTimeout": "5000ms",
        "builtinProbes": {
          "enabled": true,
          "livenessPath": "/livez",
          "readinessPath": "/readyz",
          "startupPath": "/startupz",
          "contentType": "text/plain"
        },
        "telemetry": {
          "otelEnabled": false,
          "dogStatsDEnabled": false
        }
      }
    }
```

Pair it with this `Deployment`:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: aeronet-example-json
  labels:
    app: aeronet-example-json
spec:
  replicas: 2
  selector:
    matchLabels:
      app: aeronet-example-json
  template:
    metadata:
      labels:
        app: aeronet-example-json
    spec:
      containers:
        - name: aeronet
          image: your-registry/aeronet:latest
          imagePullPolicy: IfNotPresent
          args: ["--config", "/etc/aeronet/server.json"]
          ports:
            - name: http
              containerPort: 8080
          volumeMounts:
            - name: aeronet-config
              mountPath: /etc/aeronet
              readOnly: true
          livenessProbe:
            httpGet:
              path: /livez
              port: http
            initialDelaySeconds: 10
            periodSeconds: 10
            timeoutSeconds: 2
            failureThreshold: 3
          readinessProbe:
            httpGet:
              path: /readyz
              port: http
            initialDelaySeconds: 5
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 3
          startupProbe:
            httpGet:
              path: /startupz
              port: http
            initialDelaySeconds: 2
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 30
      volumes:
        - name: aeronet-config
          configMap:
            name: aeronet-config-json
```

Apply it with:

```bash
kubectl apply -f aeronet-json-configmap.yaml
kubectl apply -f aeronet-json-deployment.yaml
```

## Probe Tuning Guidance

1. Keep liveness checks strict enough to catch hangs, but avoid false positives under transient load.
2. Keep readiness checks representative of traffic readiness, not just process up/down.
3. Use startup probe for slow warmup paths so liveness does not restart pods too early.
4. Prefer named ports (`port: http`) in probes to avoid drift when container ports change.

## Common Validation Commands

```bash
kubectl get pods
kubectl describe pod <pod-name>
kubectl logs <pod-name>
kubectl get endpoints aeronet-example-yaml
```

## Related Docs

- [README.md](README.md)
- [FEATURES.md](FEATURES.md#built-in-kubernetes-style-probes)
