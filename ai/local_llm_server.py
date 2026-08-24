#!/usr/bin/env python3
"""Small OpenAI-compatible local GPU server used when vLLM is unavailable.

It intentionally implements only the endpoints the Intervener sidecar uses:
``/v1/models`` and ``/v1/chat/completions``. Requests are serialized so a
6 GB development GPU stays responsive.
"""
from __future__ import annotations

import argparse
import asyncio
import time
import uuid
from contextlib import asynccontextmanager
from typing import Any

import torch
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse
from transformers import AutoModelForCausalLM, AutoTokenizer


class LocalModel:
    def __init__(self, model_name: str) -> None:
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is required for the local model server")
        self.model_name = model_name
        print(f"[local-llm] loading {model_name} onto CUDA...", flush=True)
        self.tokenizer = AutoTokenizer.from_pretrained(model_name)
        self.model = AutoModelForCausalLM.from_pretrained(
            model_name, torch_dtype=torch.float16, low_cpu_mem_usage=True
        ).to("cuda").eval()
        self.lock = asyncio.Lock()

    async def complete(self, messages: list[dict[str, Any]], max_tokens: int, temperature: float) -> str:
        normalized = [
            {"role": str(item.get("role", "user")), "content": str(item.get("content", ""))}
            for item in messages
        ]
        if not normalized:
            raise ValueError("messages must not be empty")
        async with self.lock:
            return await asyncio.to_thread(self._generate, normalized, max_tokens, temperature)

    def _generate(self, messages: list[dict[str, str]], max_tokens: int, temperature: float) -> str:
        inputs = self.tokenizer.apply_chat_template(
            messages, add_generation_prompt=True, tokenize=True, return_dict=True, return_tensors="pt"
        ).to("cuda")
        prompt_length = inputs["input_ids"].shape[-1]
        sampling = temperature > 0.01
        generation_kwargs: dict[str, Any] = {
            "max_new_tokens": min(max(1, max_tokens), 160),
            "do_sample": sampling,
            "pad_token_id": self.tokenizer.eos_token_id,
        }
        if sampling:
            generation_kwargs.update({"temperature": max(temperature, 0.01), "top_p": 0.9})
        with torch.inference_mode():
            output = self.model.generate(**inputs, **generation_kwargs)
        return self.tokenizer.decode(output[0][prompt_length:], skip_special_tokens=True).strip()


def make_app(model_name: str, api_key: str) -> FastAPI:
    state: dict[str, LocalModel] = {}

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        state["model"] = LocalModel(model_name)
        print(f"[local-llm] ready: {model_name}", flush=True)
        yield
        state.clear()
        torch.cuda.empty_cache()

    app = FastAPI(lifespan=lifespan)

    def authorize(request: Request) -> None:
        if api_key and request.headers.get("Authorization") != f"Bearer {api_key}":
            raise HTTPException(status_code=401, detail="invalid API key")

    @app.get("/v1/models")
    async def models(request: Request) -> JSONResponse:
        authorize(request)
        return JSONResponse({"object": "list", "data": [{"id": model_name, "object": "model"}]})

    @app.post("/v1/chat/completions")
    async def completions(request: Request) -> JSONResponse:
        authorize(request)
        payload = await request.json()
        messages = payload.get("messages")
        if not isinstance(messages, list):
            raise HTTPException(status_code=400, detail="messages must be an array")
        try:
            content = await state["model"].complete(
                messages, int(payload.get("max_tokens", 128)), float(payload.get("temperature", 0.2))
            )
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        return JSONResponse({
            "id": f"chatcmpl-{uuid.uuid4().hex}",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": model_name,
            "choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}],
        })

    return app


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--api-key", default="local-chat")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    import uvicorn
    uvicorn.run(make_app(args.model, args.api_key), host="127.0.0.1", port=args.port, log_level="info")


if __name__ == "__main__":
    main()
