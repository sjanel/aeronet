# Kubernetes probe examples for aeronet

This page provides a small, copy-pasteable Kubernetes `Deployment` snippet that configures
liveness, readiness and startup probes for an aeronet-based container.

Notes:

- The example assumes the aeronet server exposes the built-in probe endpoints at `/livez`,
  `/readyz` and `/startupz`. Enable them in your application with `HttpServerConfig::enableBuiltinProbes(true)`.
- Tune `initialDelaySeconds`, `periodSeconds`, `timeoutSeconds` and `failureThreshold` for your environment.

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: aeronet-example
  labels:
    app: aeronet
spec:
  replicas: 2
  selector:
    matchLabels:
      app: aeronet
  template:
    metadata:
      labels:
        app: aeronet
    spec:
      containers:
        - name: aeronet
          image: your-registry/aeronet:latest
          ports:
            - containerPort: 8080
          livenessProbe:
            httpGet:
              path: /livez
              port: 8080
            initialDelaySeconds: 10
            periodSeconds: 10
            timeoutSeconds: 2
            failureThreshold: 3
          readinessProbe:
            httpGet:
              path: /readyz
              port: 8080
            initialDelaySeconds: 5
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 3
          # Optional: Kubernetes startupProbe (useful for longer init paths)
          startupProbe:
            httpGet:
              path: /startupz
              port: 8080
            initialDelaySeconds: 2
            periodSeconds: 5
            failureThreshold: 30
          resources:
            requests:
              cpu: "100m"
              memory: "128Mi"
            limits:
              cpu: "500m"
              memory: "256Mi"

```

Operational tips

- Use the liveness probe to detect a hung process; keep it simple and fast.
- Use the readiness probe to control traffic routing: aeronet flips readiness to false early in graceful drain,
  allowing load-balancers to stop sending new requests while existing connections drain.
- If your service performs a lengthy startup (database migrations, caches), prefer `startupProbe` to avoid
  confusing liveness checks during initialization.

See also: [built-in Kubernetes-style probes](FEATURES.md#built-in-kubernetes-style-probes) for details on configuration and behavior.
