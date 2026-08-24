#!/usr/bin/env bash
# Local all-in-one launcher for Chaos Chat. It never installs packages or
# uploads anything: it builds locally and starts services on 127.0.0.1.
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="$ROOT_DIR/runtime"
LOG_DIR="$RUNTIME_DIR/logs"
PID_DIR="$RUNTIME_DIR/pids"
WEB_PORT="${WEB_PORT:-3000}"
CHAT_PORT="${CHAT_PORT:-1234}"
WEB_BIND_HOST="${WEB_BIND_HOST:-127.0.0.1}"
CHAT_BIND_HOST="${CHAT_BIND_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
VLLM_PORT="${VLLM_PORT:-8000}"
EMBEDDING_PORT="${EMBEDDING_PORT:-8001}"
START_VLLM="${START_VLLM:-0}"
START_EMBEDDINGS="${START_EMBEDDINGS:-0}"
START_AI="${START_AI:-1}"
START_DEV_BOTS="${START_DEV_BOTS:-0}"
START_LOCAL_LLM="${START_LOCAL_LLM:-0}"
VLLM_GPU_MEMORY_UTILIZATION="${VLLM_GPU_MEMORY_UTILIZATION:-0.70}"
VLLM_WSL2_ENABLE_PIN_MEMORY="${VLLM_WSL2_ENABLE_PIN_MEMORY:-1}"
VLLM_MAX_MODEL_LEN="${VLLM_MAX_MODEL_LEN:-4096}"
VLLM_ENFORCE_EAGER="${VLLM_ENFORCE_EAGER:-1}"
VLLM_LOGGING_LEVEL="${VLLM_LOGGING_LEVEL:-DEBUG}"
VLLM_ATTENTION_BACKEND="${VLLM_ATTENTION_BACKEND:-}"
NCCL_P2P_DISABLE="${NCCL_P2P_DISABLE:-1}"
NCCL_SHM_DISABLE="${NCCL_SHM_DISABLE:-1}"
HF_HUB_DISABLE_XET="${HF_HUB_DISABLE_XET:-1}"

usage() {
  cat <<'EOF'
Usage: ./scripts/dev.sh <start|stop|restart|status|test|build|diagnose>

Commands:
  start    Build the backend and frontend, then start Redis, chat server,
           static web server, and (by default) the AI sidecar.
  stop     Stop only processes started by this script.
  restart  Stop, then start again.
  status   Show managed process IDs and whether their ports are reachable.
  test     Run non-destructive local health checks against a running stack.
  build    Build the C backend and TypeScript frontend without starting it.
  diagnose Show the WSL2/vLLM diagnostics and the last vLLM log lines.

Configuration:
  Copy .env.example to .env and enter the Supabase values. The AI sidecar
  reads ai/.env (copy ai/.env.example if needed). No secret is logged.

  START_VLLM=1       Also launch the configured Llama vLLM process.
  START_EMBEDDINGS=1 Also launch the configured embedding vLLM process.
  START_AI=0         Start only Redis, C backend, and web UI.
  CHAT_DEV_AUTH=1    Disable OAuth locally and permit named test guests.
  START_DEV_BOTS=1   Launch three conversational test bots (requires CHAT_DEV_AUTH=1).
  START_LOCAL_LLM=1  Use the bundled Transformers GPU server instead of vLLM.
  VLLM_GPU_MEMORY_UTILIZATION=0.70  GPU memory fraction reserved by vLLM.
  VLLM_WSL2_ENABLE_PIN_MEMORY=1     Enable vLLM pinned memory under WSL2.
  VLLM_MAX_MODEL_LEN=4096           Cap vLLM context length for smaller GPUs.
  VLLM_ENFORCE_EAGER=1              Avoid CUDA-graph compilation on constrained GPUs.
  NCCL_P2P_DISABLE=1 / NCCL_SHM_DISABLE=1  Avoid WSL2 NCCL P2P/shared-memory hangs.
  HF_HUB_DISABLE_XET=1             Avoid failed Hugging Face Xet/CAS reconstruction downloads.
  VLLM_ATTENTION_BACKEND=TRITON_ATTN  Optional vLLM attention-backend override.
  WEB_BIND_HOST=0.0.0.0 and CHAT_BIND_HOST=0.0.0.0 expose a test stack to the LAN.

Examples:
  cp .env.example .env
  ./scripts/dev.sh start
  START_VLLM=1 START_EMBEDDINGS=1 ./scripts/dev.sh start
  CHAT_DEV_AUTH=1 START_DEV_BOTS=1 ./scripts/dev.sh start
  ./scripts/dev.sh test
  ./scripts/dev.sh stop
EOF
}

note() { printf '[chaos-chat] %s\n' "$*"; }
fail() { printf '[chaos-chat] error: %s\n' "$*" >&2; exit 1; }

ensure_runtime_dirs() { mkdir -p "$LOG_DIR" "$PID_DIR"; }

load_env_file() {
  local file="$1"
  if [[ -f "$file" ]]; then
    # .env files are user-owned shell-style configuration. Do not echo them.
    set -a
    # shellcheck disable=SC1090
    source "$file"
    set +a

    # A JWKS is JSON and commonly contains double quotes. Preserve a valid
    # JSON value if it was wrapped in double quotes in a shell-style .env
    # file; sourcing such a line would otherwise consume its JSON quotes.
    local raw_jwks
    raw_jwks="$(awk '/^SUPABASE_JWKS=/{sub(/^[^=]*=/, ""); print; exit}' "$file")"
    if [[ "$raw_jwks" == \"*\" && "$raw_jwks" == *\" ]]; then
      raw_jwks="${raw_jwks:1:${#raw_jwks}-2}"
      if python3 -c 'import json, sys; json.loads(sys.argv[1])' "$raw_jwks" >/dev/null 2>&1; then
        SUPABASE_JWKS="$raw_jwks"
        export SUPABASE_JWKS
      fi
    fi
  fi
}

pid_file() { printf '%s/%s.pid' "$PID_DIR" "$1"; }
pgid_file() { printf '%s/%s.pgid' "$PID_DIR" "$1"; }

is_managed_running() {
  local file
  file="$(pid_file "$1")"
  [[ -f "$file" ]] && kill -0 "$(<"$file")" 2>/dev/null
}

port_open() {
  python3 - "$1" <<'PY'
import socket
import sys

sock = socket.socket()
sock.settimeout(0.25)
try:
    sock.connect(("127.0.0.1", int(sys.argv[1])))
except OSError:
    raise SystemExit(1)
finally:
    sock.close()
PY
}

wait_for_port() {
  local port="$1" label="$2" tries="${3:-50}"
  local attempt
  for ((attempt = 0; attempt < tries; ++attempt)); do
    port_open "$port" && return 0
    sleep 0.1
  done
  note "$label did not become reachable yet; inspect $LOG_DIR/$label.log"
  return 1
}

start_process() {
  local name="$1"
  shift
  local file group_file isolated=0
  file="$(pid_file "$name")"
  group_file="$(pgid_file "$name")"
  if is_managed_running "$name"; then
    note "$name is already running (PID $(<"$file"))."
    return 0
  fi
  rm -f "$file" "$group_file"
  # vLLM launches EngineCore children. A dedicated session makes the PID a
  # process-group leader so stop_process can reclaim every GPU-owning child.
  if command -v setsid >/dev/null 2>&1; then
    nohup setsid "$@" >"$LOG_DIR/$name.log" 2>&1 &
    isolated=1
  else
    nohup "$@" >"$LOG_DIR/$name.log" 2>&1 &
  fi
  printf '%s\n' "$!" >"$file"
  [[ "$isolated" == "1" ]] && printf '%s\n' "$!" >"$group_file"
  note "started $name (PID $!). log: runtime/logs/$name.log"
}

stop_process() {
  local name="$1" file group_file pid pgid group_alive=0
  file="$(pid_file "$name")"
  group_file="$(pgid_file "$name")"
  [[ -f "$file" ]] || return 0
  pid="$(<"$file")"
  if [[ -f "$group_file" ]]; then
    pgid="$(<"$group_file")"
  else
    # Services launched by an older version of the script have no group
    # metadata; fall back safely to their recorded parent PID.
    pgid=""
  fi
  if [[ -n "$pgid" ]] && kill -0 -- "-$pgid" 2>/dev/null; then group_alive=1; fi
  if kill -0 "$pid" 2>/dev/null || [[ "$group_alive" == "1" ]]; then
    if [[ "$group_alive" == "1" ]]; then
      kill -TERM -- "-$pgid" 2>/dev/null || true
    else
      kill "$pid" 2>/dev/null || true
    fi
    for _ in {1..30}; do
      if [[ "$group_alive" == "1" ]]; then
        kill -0 -- "-$pgid" 2>/dev/null || break
      else
        kill -0 "$pid" 2>/dev/null || break
      fi
      sleep 0.1
    done
    if { [[ "$group_alive" == "1" ]] && kill -0 -- "-$pgid" 2>/dev/null; } ||
       { [[ "$group_alive" != "1" ]] && kill -0 "$pid" 2>/dev/null; }; then
      note "$name did not exit gracefully; sending SIGKILL."
      if [[ "$group_alive" == "1" ]]; then
        kill -KILL -- "-$pgid" 2>/dev/null || true
      else
        kill -KILL "$pid" 2>/dev/null || true
      fi
    fi
    note "stopped $name (PID $pid)."
  fi
  rm -f "$file" "$group_file"
}

require_command() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }

vllm_binary() {
  if [[ -n "${VLLM_BIN:-}" ]]; then
    [[ -x "$VLLM_BIN" ]] || fail "VLLM_BIN is not executable: $VLLM_BIN"
    printf '%s' "$VLLM_BIN"
  elif command -v vllm >/dev/null 2>&1; then
    command -v vllm
  elif [[ -x "$ROOT_DIR/ai/.venv/bin/vllm" ]]; then
    printf '%s' "$ROOT_DIR/ai/.venv/bin/vllm"
  else
    fail "missing required command: vllm. Install it in ai/.venv with: ai/.venv/bin/pip install vllm"
  fi
}

build() {
  require_command cc
  require_command pkg-config
  require_command npm
  pkg-config --exists sqlite3 libsodium openssl hiredis libcjson || fail "missing C development packages (sqlite3 libsodium openssl hiredis libcjson)."
  note "building C chat server..."
  cc -std=c11 -O2 -Wall -Wextra "$ROOT_DIR/chat_server.c" -o "$ROOT_DIR/chat_server" \
    $(pkg-config --cflags --libs sqlite3 libsodium openssl hiredis libcjson)
  note "building web bundle..."
  npm --prefix "$ROOT_DIR/web" run build
}

require_backend_config() {
  if [[ "${CHAT_DEV_AUTH:-0}" == "1" ]]; then
    note "development guest auth is enabled; Supabase OAuth checks are skipped."
    return 0
  fi
  [[ -n "${SUPABASE_JWKS:-}" ]] || fail "SUPABASE_JWKS is missing. Copy .env.example to .env and configure Supabase."
  [[ -n "${SUPABASE_ISSUER:-}" ]] || fail "SUPABASE_ISSUER is missing. It should be https://<project-ref>.supabase.co/auth/v1."
  [[ -n "${CHAT_ALLOWED_ORIGIN:-}" ]] || fail "CHAT_ALLOWED_ORIGIN is missing. Use http://localhost:$WEB_PORT."
}

start_redis() {
  if port_open "$REDIS_PORT"; then
    note "Redis already listens on 127.0.0.1:$REDIS_PORT; using it."
  else
    require_command redis-server
    start_process redis redis-server --bind 127.0.0.1 --port "$REDIS_PORT" --save '' --appendonly no
    wait_for_port "$REDIS_PORT" redis || fail "Redis failed to start."
  fi
}

start_backend() {
  if port_open "$CHAT_PORT" && ! is_managed_running chat-server; then
    fail "port $CHAT_PORT is already in use by another process."
  fi
  start_process chat-server env REDIS_URL="127.0.0.1:$REDIS_PORT" CHAT_BIND_HOST="$CHAT_BIND_HOST" CHAT_DB_PATH="$ROOT_DIR/runtime/chat.db" "$ROOT_DIR/chat_server"
  wait_for_port "$CHAT_PORT" chat-server || fail "chat server failed to start."
}

start_web() {
  if port_open "$WEB_PORT" && ! is_managed_running web; then
    fail "port $WEB_PORT is already in use by another process."
  fi
  start_process web python3 -m http.server "$WEB_PORT" --bind "$WEB_BIND_HOST" --directory "$ROOT_DIR/web"
  wait_for_port "$WEB_PORT" web || fail "web server failed to start."
}

read_ai_value() {
  local key="$1" fallback="$2"
  local value
  value="$(awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$ROOT_DIR/ai/.env" 2>/dev/null || true)"
  printf '%s' "${value:-$fallback}"
}

start_vllm() {
  [[ "$START_LOCAL_LLM" != "1" ]] || return 0
  [[ "$START_VLLM" == "1" ]] || return 0
  local model api_key vllm_bin
  vllm_bin="$(vllm_binary)"
  model="$(read_ai_value VLLM_MODEL meta-llama/Llama-3.1-8B-Instruct)"
  api_key="$(read_ai_value VLLM_API_KEY local-chat)"
  if port_open "$VLLM_PORT"; then
    note "vLLM chat endpoint already listens on port $VLLM_PORT; using it."
  else
    local eager_args=() vllm_env=(
      "VLLM_WSL2_ENABLE_PIN_MEMORY=$VLLM_WSL2_ENABLE_PIN_MEMORY"
      "VLLM_LOGGING_LEVEL=$VLLM_LOGGING_LEVEL"
      "NCCL_P2P_DISABLE=$NCCL_P2P_DISABLE"
      "NCCL_SHM_DISABLE=$NCCL_SHM_DISABLE"
      "HF_HUB_DISABLE_XET=$HF_HUB_DISABLE_XET"
    )
    [[ "$VLLM_ENFORCE_EAGER" == "1" ]] && eager_args+=(--enforce-eager)
    local attention_args=()
    [[ -n "$VLLM_ATTENTION_BACKEND" ]] && attention_args+=(--attention-backend "$VLLM_ATTENTION_BACKEND")
    start_process vllm-chat env "${vllm_env[@]}" "$vllm_bin" serve "$model" --dtype auto --api-key "$api_key" --generation-config vllm --gpu-memory-utilization "$VLLM_GPU_MEMORY_UTILIZATION" --max-model-len "$VLLM_MAX_MODEL_LEN" "${eager_args[@]}" "${attention_args[@]}" --port "$VLLM_PORT"
  fi
  if [[ "$START_EMBEDDINGS" == "1" ]]; then
    local embedding_model embedding_key
    embedding_model="$(read_ai_value RAG_EMBEDDING_MODEL BAAI/bge-small-en-v1.5)"
    embedding_key="$(read_ai_value RAG_EMBEDDING_API_KEY "$api_key")"
    if port_open "$EMBEDDING_PORT"; then
      note "vLLM embedding endpoint already listens on port $EMBEDDING_PORT; using it."
    else
      start_process vllm-embeddings "$vllm_bin" serve "$embedding_model" --runner pooling --port "$EMBEDDING_PORT" --api-key "$embedding_key"
    fi
  fi
}

start_local_llm() {
  [[ "$START_LOCAL_LLM" == "1" ]] || return 0
  [[ "$START_VLLM" != "1" ]] || fail "Choose either START_VLLM=1 or START_LOCAL_LLM=1."
  local model api_key ai_python
  ai_python="$ROOT_DIR/ai/.venv/bin/python"
  [[ -x "$ai_python" ]] || fail "AI virtual environment is missing."
  model="$(read_ai_value VLLM_MODEL Qwen/Qwen2.5-1.5B-Instruct)"
  api_key="$(read_ai_value VLLM_API_KEY local-chat)"
  if port_open "$VLLM_PORT"; then
    note "LLM endpoint already listens on port $VLLM_PORT; using it."
  else
    start_process local-llm env "HF_HUB_DISABLE_XET=$HF_HUB_DISABLE_XET" "$ai_python" "$ROOT_DIR/ai/local_llm_server.py" --model "$model" --api-key "$api_key" --port "$VLLM_PORT"
    # The sidecar is useful only once the model server has bound its socket.
    # Wait here so a start immediately followed by `dev.sh test` does not
    # report a misleading failure while Transformers is loading weights.
    wait_for_port "$VLLM_PORT" local-llm 1200 || fail "local LLM failed to start."
  fi
}

start_ai() {
  [[ "$START_AI" != "0" ]] || return 0
  local ai_python="$ROOT_DIR/ai/.venv/bin/python"
  [[ -x "$ai_python" ]] || fail "AI virtual environment is missing. Run: python3 -m venv ai/.venv && ai/.venv/bin/pip install -r ai/requirements.txt"
  "$ai_python" -c 'import redis' >/dev/null 2>&1 || fail "AI dependency missing. Run: ai/.venv/bin/pip install -r ai/requirements.txt"
  [[ -f "$ROOT_DIR/ai/.env" ]] || fail "ai/.env is missing. Copy ai/.env.example to ai/.env."
  start_process ai-sidecar bash -c 'cd "$1" && set -a && source ./.env && set +a && exec "$2" intervener.py' _ "$ROOT_DIR/ai" "$ai_python"
}

start_dev_bots() {
  [[ "$START_DEV_BOTS" == "1" ]] || return 0
  [[ "${CHAT_DEV_AUTH:-0}" == "1" ]] || fail "START_DEV_BOTS requires CHAT_DEV_AUTH=1."
  local llm_base_url llm_model llm_api_key
  llm_base_url="$(read_ai_value VLLM_BASE_URL "http://127.0.0.1:$VLLM_PORT/v1")"
  llm_model="$(read_ai_value VLLM_MODEL "Qwen/Qwen2.5-1.5B-Instruct")"
  llm_api_key="$(read_ai_value VLLM_API_KEY local-chat)"
  start_process dev-bots env "LLM_BOT_BASE_URL=$llm_base_url" "LLM_BOT_MODEL=$llm_model" "LLM_BOT_API_KEY=$llm_api_key" \
    python3 "$ROOT_DIR/scripts/dev_bots.py" --host 127.0.0.1 --port "$CHAT_PORT"
}

preflight_start() {
  if [[ "$START_VLLM" == "1" ]]; then
    vllm_binary >/dev/null
  fi
  if [[ "$START_AI" != "0" ]]; then
    local ai_python="$ROOT_DIR/ai/.venv/bin/python"
    [[ -x "$ai_python" ]] || fail "AI virtual environment is missing. Run: python3 -m venv ai/.venv && ai/.venv/bin/pip install -r ai/requirements.txt"
    "$ai_python" -c 'import redis' >/dev/null 2>&1 || fail "AI dependency missing. Run: ai/.venv/bin/pip install -r ai/requirements.txt"
    [[ -f "$ROOT_DIR/ai/.env" ]] || fail "ai/.env is missing. Copy ai/.env.example to ai/.env."
  fi
}

start() {
  ensure_runtime_dirs
  load_env_file "$ROOT_DIR/.env"
  require_backend_config
  preflight_start
  build
  start_redis
  start_backend
  start_web
  start_vllm
  start_local_llm
  start_ai
  start_dev_bots
  note "stack started. Open http://localhost:$WEB_PORT and use ws://localhost:$CHAT_PORT in the app configuration."
  note "logs: $LOG_DIR  |  status: ./scripts/dev.sh status  |  health checks: ./scripts/dev.sh test"
}

status() {
  ensure_runtime_dirs
  for name in redis chat-server web vllm-chat local-llm vllm-embeddings ai-sidecar dev-bots; do
    if is_managed_running "$name"; then
      note "$name: running (PID $(<"$(pid_file "$name")"))"
    else
      note "$name: stopped"
    fi
  done
  for spec in "redis:$REDIS_PORT" "chat-server:$CHAT_PORT" "web:$WEB_PORT" "vllm-chat:$VLLM_PORT" "vllm-embeddings:$EMBEDDING_PORT"; do
    local_name="${spec%%:*}"
    local_port="${spec##*:}"
    port_open "$local_port" && note "$local_name port $local_port: reachable" || note "$local_name port $local_port: unavailable"
  done
}

test_stack() {
  load_env_file "$ROOT_DIR/.env"
  local failed=0
  check() {
    local label="$1"
    shift
    if "$@"; then note "PASS: $label"; else note "FAIL: $label"; failed=1; fi
  }
  check "Redis port $REDIS_PORT" port_open "$REDIS_PORT"
  check "chat-server port $CHAT_PORT" port_open "$CHAT_PORT"
  check "web page" curl --fail --silent --show-error -o /dev/null "http://127.0.0.1:$WEB_PORT/"
  check "chat health" curl --fail --silent --show-error -o /dev/null "http://127.0.0.1:$CHAT_PORT/healthz"
  check "chat readiness" curl --fail --silent --show-error -o /dev/null "http://127.0.0.1:$CHAT_PORT/readyz"
  check "chat metrics" curl --fail --silent --show-error -o /dev/null "http://127.0.0.1:$CHAT_PORT/metrics"
  if [[ "$START_VLLM" == "1" || "$START_LOCAL_LLM" == "1" ]]; then
    check "local LLM port $VLLM_PORT" port_open "$VLLM_PORT"
    if port_open "$VLLM_PORT"; then
      check "LLM chat models endpoint" curl --fail --silent --show-error -o /dev/null -H "Authorization: Bearer $(read_ai_value VLLM_API_KEY local-chat)" "http://127.0.0.1:$VLLM_PORT/v1/models"
    fi
  elif port_open "$VLLM_PORT"; then
    check "LLM chat models endpoint" curl --fail --silent --show-error -o /dev/null -H "Authorization: Bearer $(read_ai_value VLLM_API_KEY local-chat)" "http://127.0.0.1:$VLLM_PORT/v1/models"
  fi
  if port_open "$EMBEDDING_PORT"; then check "vLLM embedding models endpoint" curl --fail --silent --show-error -o /dev/null -H "Authorization: Bearer $(read_ai_value RAG_EMBEDDING_API_KEY local-chat)" "http://127.0.0.1:$EMBEDDING_PORT/v1/models"; fi
  [[ "$failed" == "0" ]] || exit 1
  if [[ "${CHAT_DEV_AUTH:-0}" == "1" ]]; then
    note "Local service checks passed. Open http://localhost:$WEB_PORT/?dev=1 to join the active test bots."
  else
    note "Local service checks passed. Complete OAuth manually in the browser, then send messages to verify the auth and AI paths."
  fi
}

diagnose_vllm() {
  ensure_runtime_dirs
  load_env_file "$ROOT_DIR/.env"
  note "vLLM configuration: pin-memory=$VLLM_WSL2_ENABLE_PIN_MEMORY, eager=$VLLM_ENFORCE_EAGER, VRAM fraction=$VLLM_GPU_MEMORY_UTILIZATION, context=$VLLM_MAX_MODEL_LEN"
  note "NCCL: P2P disabled=$NCCL_P2P_DISABLE, SHM disabled=$NCCL_SHM_DISABLE; attention=${VLLM_ATTENTION_BACKEND:-default}; Xet disabled=$HF_HUB_DISABLE_XET"
  df -h /dev/shm || true
  if command -v nvidia-smi >/dev/null 2>&1; then
    nvidia-smi --query-gpu=name,memory.total,memory.used,memory.free --format=csv,noheader || true
    nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader || true
  else
    note "nvidia-smi is unavailable in this shell."
  fi
  if [[ -f "$LOG_DIR/vllm-chat.log" ]]; then
    note "last vLLM log lines:"
    tail -n 80 "$LOG_DIR/vllm-chat.log"
  else
    note "no vLLM log exists yet. Start with START_VLLM=1 ./scripts/dev.sh start."
  fi
}

stop() {
  ensure_runtime_dirs
  for name in dev-bots ai-sidecar vllm-embeddings local-llm vllm-chat web chat-server redis; do stop_process "$name"; done
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  restart) stop; start ;;
  status) status ;;
  test) test_stack ;;
  diagnose) diagnose_vllm ;;
  build) build ;;
  -h|--help|help|'') usage ;;
  *) usage; exit 2 ;;
esac
