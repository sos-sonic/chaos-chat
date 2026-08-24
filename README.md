# Chaos Chat

A real-time chat application built as a systems programming exercise. Features a single-threaded C WebSocket server, a Next.js frontend, and an asynchronous Python AI sidecar.

## Architecture

```text
Browser (Next.js / TypeScript)
        │
        ├── WebSocket ──> C chat server ──> SQLite (WAL)
        │                        │
        │                        ├── Redis pub/sub (multi-room fan-out)
        │                        └── HTTP health / metrics
        │
        ├── Supabase OAuth ── JWT/JWKS verification
        │
        └── WebRTC P2P (server relays signaling)

AI sidecar (Python) <── Redis events ──> vLLM (OpenAI-compatible)
                                      └── bundled Transformers server (fallback)
```

**Backend (`chat_server.c`)** — A single-threaded C server using Linux `epoll` for TCP multiplexing. Handles a custom raw binary protocol and WebSocket, Supabase ES256 JWT verification, multi-room state with Redis Pub/Sub, durable SQLite (WAL) persistence, owner-controlled chaos delivery, PvP game relay, WebRTC signaling, and health/metrics endpoints. No application framework.

**Frontend (`web-next/`)** — A Next.js 16 / React 19 TypeScript client with Tailwind CSS. Parses the binary wire protocol client-side, integrates Supabase OAuth, and renders sentiment-aware message cards with real-time AI refinements.

**AI Sidecar (`ai/`)** — An asynchronous Python process that subscribes to Redis room events. Performs sentiment-aware style classification and RAG-grounded contextual commentary (Vibe Bot) using a local vLLM endpoint, with semantic embedding retrieval and a bundled Transformers fallback.

## Features

- Real-time multi-room chat over a custom binary protocol
- Sentiment-aware message cards: reactions, sarcasm, questions, celebrations, confessions, and shouts
- **Vibe Bot** — an AI commentator that uses conversation history and RAG retrieval for contextual replies
- Owner-controlled chaos delivery: randomized delay, reordering, intentional duplicates, and auto-pulse mode
- Player-vs-player Pong challenges with server-authoritative relay and a durable match ledger
- WebRTC P2P audio calls with server-relayed signaling
- `/healthz`, `/readyz`, and Prometheus-style `/metrics` endpoints

## Prerequisites

- Linux with `epoll` support
- C compiler and `pkg-config`
- Development packages: SQLite, libsodium, OpenSSL, hiredis, cJSON
- Node.js 18+ and npm
- Python 3.10+ and Redis
- A Supabase project (for OAuth)

## Quick Start

```bash
# 1. Configure environment
cp .env.example .env
cp ai/.env.example ai/.env
cp web-next/.env.local.example web-next/.env.local
# Edit the files above with your Supabase and Redis values

# 2. Set up the AI sidecar
python3 -m venv ai/.venv
ai/.venv/bin/pip install -r ai/requirements.txt

# 3. Install frontend dependencies
cd web-next && npm install && cd ..

# 4. Build the server
gcc -O2 -o chat_server chat_server.c \
  $(pkg-config --cflags --libs sqlite3 libsodium openssl hiredis) \
  -lcjson -lpthread

# 5. Start Redis, then the server
redis-server &
./chat_server

# 6. Start the AI sidecar (separate terminal)
set -a && . ai/.env && set +a
ai/.venv/bin/python ai/intervener.py

# 7. Start the frontend (separate terminal)
cd web-next && npm run dev
```

Open `http://localhost:3000` and complete OAuth.

## Development with Scripts

```bash
# Build, launch all services, run diagnostics
./scripts/dev.sh start

# Check service status
./scripts/dev.sh status

# Run basic health probes
./scripts/dev.sh test

# Stop all launched processes
./scripts/dev.sh stop
```

## Repository Structure

```text
chat_server.c          C server: epoll, WebSocket, auth, rooms, games, chaos, metrics
web-next/              Next.js frontend (TypeScript, Tailwind CSS)
ai/intervener.py       Redis-backed sentiment classifier and Vibe Bot sidecar
ai/local_llm_server.py OpenAI-compatible Transformers fallback endpoint
scripts/dev.sh         Build, launch, status, diagnostics, and health checks
```

## Environment Variables

### Server (`.env`)

| Variable | Description |
|----------|-------------|
| `CHAT_ALLOWED_ORIGIN` | CORS origin (exact match or comma-separated list) |
| `SUPABASE_ISSUER` | Supabase project URL |
| `SUPABASE_JWKS` | Supabase JWKS endpoint URL |
| `CHAT_CHAOS_ENABLED` | Enable chaos mode globally (`1`/`true`) |
| `CHAT_CHAOS_MIN_DELAY_MS` | Minimum chaos delay (default: 500) |
| `CHAT_CHAOS_MAX_DELAY_MS` | Maximum chaos delay (default: 5000) |

### Frontend (`web-next/.env.local`)

| Variable | Description |
|----------|-------------|
| `NEXT_PUBLIC_CHAT_SERVER_URL` | WebSocket URL (default: `ws://localhost:1234`) |
| `NEXT_PUBLIC_SUPABASE_URL` | Supabase project URL |
| `NEXT_PUBLIC_SUPABASE_ANON_KEY` | Supabase anon public key |

### AI Sidecar (`ai/.env`)

| Variable | Description |
|----------|-------------|
| `REDIS_URL` | Redis connection URL |
| `VLLM_BASE_URL` | vLLM OpenAI-compatible endpoint |
| `VLLM_MODEL` | Chat model name |
| `RAG_EMBEDDING_BASE_URL` | Embedding model endpoint (optional) |

See `ai/README.md` for full sidecar configuration.

## Testing

```bash
# Health probes
curl http://127.0.0.1:1234/healthz
curl http://127.0.0.1:1234/readyz
curl http://127.0.0.1:1234/metrics
```

## License

MIT
