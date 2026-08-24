# Intervener sidecar

This process runs the asynchronous AI work for Chaos Chat: contextual style
classification and the RAG-based `Intervener` bot. It uses vLLM's
OpenAI-compatible `/v1/chat/completions` endpoint and can use a separate vLLM
embedding endpoint for semantic retrieval.

## Start model servers

Serve an instruction-tuned chat model for classification and Vibe Bot
generation. Qwen2.5-1.5B is a small starting point; choose a model and memory
settings that fit the hardware available to you:

```bash
vllm serve Qwen/Qwen2.5-1.5B-Instruct --dtype auto --api-key local-chat \
  --generation-config vllm
```

For semantic retrieval, serve an embedding model separately:

```bash
vllm serve BAAI/bge-small-en-v1.5 \
  --runner pooling --port 8001 --api-key local-chat
```

The sidecar falls back to token-overlap retrieval if the embedding service is
not configured or unavailable. When vLLM remains unavailable, set
`START_LOCAL_LLM=1` in the root `.env`; the bundled Transformers GPU server
keeps the same OpenAI-compatible endpoint for the sidecar.

## Run the sidecar

```bash
cd ai
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
set -a; . ./.env; set +a
python3 intervener.py
```

Copy `.env.example` to `.env` first and adjust it for your Redis/vLLM hosts.
The C server and Redis must already be running. `RAG_TRIGGER_EVERY`,
`RAG_COOLDOWN_SECONDS`, `VIBE_CONTEXT_LIMIT`, and `VIBE_SENTIMENT_THRESHOLD`
control automatic Vibe Bot replies. `/vibe`, `@vibe`, and `@intervener`
bypass automatic-trigger gating. Run `./scripts/dev.sh diagnose` after a
vLLM stall to inspect model, GPU, and recent log information. The bundled
Transformers endpoint remains available when vLLM is not a good fit.
