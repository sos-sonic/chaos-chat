#!/usr/bin/env python3
"""Asynchronous classifier and RAG intervener for Chaos Chat.

This process subscribes to Redis; it never sits in the C server's event loop.
It targets vLLM's OpenAI-compatible HTTP API, using a Llama instruct model for
classification and generation and, optionally, a second vLLM embedding server
for semantic retrieval.
"""
from __future__ import annotations

import json
import math
import os
import re
import signal
import sys
import time
import urllib.error
import urllib.request
from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Any

import redis


STYLE_IDS = {
    "plain": 0,
    "reaction": 1,
    "shout": 2,
    "confession": 3,
    "question": 4,
    "celebration": 5,
    "sarcasm": 6,
}
STYLE_NAMES = ", ".join(STYLE_IDS)
SENTIMENTS = {"positive", "negative", "neutral"}
DEFAULT_IGNORED_SPEAKERS = {
    "intervener", "game referee", "bytebard", "snarkypants", "captaincontext",
    "quipster", "threadweaver",
}
CONTEXT_STOPWORDS = {
    "a", "an", "and", "are", "as", "at", "be", "by", "do", "for", "from", "how", "i", "in", "is",
    "it", "of", "on", "or", "our", "that", "the", "this", "to", "was", "we", "what", "when", "who", "with", "you",
}


def env_int(name: str, default: int, low: int, high: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    try:
        value = int(raw)
    except ValueError:
        print(f"[ai] invalid {name}={raw!r}; using {default}", file=sys.stderr)
        return default
    return min(high, max(low, value))


def env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class Config:
    redis_url: str
    redis_channel: str
    llm_base_url: str
    llm_model: str
    llm_api_key: str
    embedding_base_url: str | None
    embedding_model: str | None
    embedding_api_key: str
    history_limit: int
    retrieval_count: int
    trigger_every: int
    cooldown_seconds: int
    bot_enabled: bool
    context_limit: int
    sentiment_threshold: float
    bot_name: str
    ignored_speakers: frozenset[str]

    @classmethod
    def from_env(cls) -> "Config":
        embedding_url = os.getenv("RAG_EMBEDDING_BASE_URL", "").rstrip("/")
        embedding_model = os.getenv("RAG_EMBEDDING_MODEL", "")
        bot_name = os.getenv("VIBE_BOT_NAME", "Vibe Bot").strip()[:48] or "Vibe Bot"
        configured_ignores = {
            value.strip().casefold()
            for value in os.getenv("VIBE_IGNORED_USERS", "").split(",")
            if value.strip()
        }
        return cls(
            redis_url=os.getenv("REDIS_URL", "redis://127.0.0.1:6379/0"),
            redis_channel=os.getenv("REDIS_CHANNEL", "chat:messages"),
            llm_base_url=os.getenv("VLLM_BASE_URL", "http://127.0.0.1:8000/v1").rstrip("/"),
            llm_model=os.getenv("VLLM_MODEL", "meta-llama/Llama-3.1-8B-Instruct"),
            llm_api_key=os.getenv("VLLM_API_KEY", "local-chat"),
            embedding_base_url=embedding_url or None,
            embedding_model=embedding_model or None,
            embedding_api_key=os.getenv("RAG_EMBEDDING_API_KEY", os.getenv("VLLM_API_KEY", "local-chat")),
            history_limit=env_int("RAG_HISTORY_LIMIT", 200, 20, 2000),
            retrieval_count=env_int("RAG_RETRIEVAL_COUNT", 6, 1, 16),
            trigger_every=env_int("RAG_TRIGGER_EVERY", 2, 1, 100),
            cooldown_seconds=env_int("RAG_COOLDOWN_SECONDS", 12, 0, 86400),
            bot_enabled=env_bool("RAG_BOT_ENABLED", True),
            context_limit=env_int("VIBE_CONTEXT_LIMIT", 30, 10, 50),
            sentiment_threshold=env_float("VIBE_SENTIMENT_THRESHOLD", 0.5, 0.0, 1.0),
            bot_name=bot_name,
            ignored_speakers=frozenset(DEFAULT_IGNORED_SPEAKERS | configured_ignores | {bot_name.casefold()}),
        )


@dataclass
class Message:
    message_id: int
    user: str
    text: str
    timestamp: int
    room: str = "lobby"
    embedding: list[float] | None = None


@dataclass(frozen=True)
class Classification:
    style: int
    sentiment: str
    intensity: float


@dataclass
class RoomState:
    history: deque[Message]
    human_messages: int = 0
    last_bot_at: float = 0.0
    # Deduplicate intentional chaos re-deliveries.
    seen_message_ids: deque[int] = field(default_factory=deque)
    seen_message_id_set: set[int] = field(default_factory=set)


def env_float(name: str, default: float, low: float, high: float) -> float:
    raw = os.getenv(name)
    if raw is None or raw == "":
        return default
    try:
        value = float(raw)
    except ValueError:
        print(f"[ai] invalid {name}={raw!r}; using {default}", file=sys.stderr)
        return default
    return min(high, max(low, value))


class VllmClient:
    def __init__(self, config: Config) -> None:
        self.config = config

    @staticmethod
    def _post(url: str, api_key: str, payload: dict[str, Any]) -> dict[str, Any]:
        body = json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            url,
            data=body,
            headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"vLLM request failed: {exc}") from exc

    def chat(self, system: str, user: str, max_tokens: int, temperature: float = 0.15) -> str:
        response = self._post(
            f"{self.config.llm_base_url}/chat/completions",
            self.config.llm_api_key,
            {
                "model": self.config.llm_model,
                "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
                "temperature": max(0.0, min(1.0, temperature)),
                "max_tokens": max_tokens,
            },
        )
        try:
            content = response["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise RuntimeError("vLLM response has no chat content") from exc
        if not isinstance(content, str):
            raise RuntimeError("vLLM returned non-text chat content")
        return content.strip()

    def embed(self, text: str) -> list[float] | None:
        if not self.config.embedding_base_url or not self.config.embedding_model:
            return None
        response = self._post(
            f"{self.config.embedding_base_url}/embeddings",
            self.config.embedding_api_key,
            {"model": self.config.embedding_model, "input": text},
        )
        try:
            embedding = response["data"][0]["embedding"]
        except (KeyError, IndexError, TypeError) as exc:
            raise RuntimeError("vLLM response has no embedding") from exc
        if not isinstance(embedding, list) or not all(isinstance(value, (int, float)) for value in embedding):
            raise RuntimeError("vLLM returned an invalid embedding")
        return [float(value) for value in embedding]


def json_object(text: str) -> dict[str, Any] | None:
    start, end = text.find("{"), text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        value = json.loads(text[start:end + 1])
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def token_counts(text: str) -> Counter[str]:
    return Counter(
        token for token in re.findall(r"[a-z0-9']+", text.lower())
        if len(token) > 1 and token not in CONTEXT_STOPWORDS
    )


def strong_style_hint(text: str) -> int:
    """Use clear textual shapes when the model returns plain."""
    compact = " ".join(text.split())
    lowered = compact.casefold()
    if compact.endswith("?"):
        return STYLE_IDS["question"]
    if any(marker in lowered for marker in ("/s", "yeah right", "as if", "love that for me", "sure, because")):
        return STYLE_IDS["sarcasm"]
    if any(marker in lowered for marker in (
        "we did it", "we made it", "i did it", "finally", "so happy", "so excited", "proud of",
        "congrat", "lets go", "let's go", "nailed it", "what a win",
    )):
        return STYLE_IDS["celebration"]
    if any(marker in lowered for marker in (
        "i have to admit", "i must admit", "not gonna lie", "i was wrong", "i feel ", "i wish i",
        "i'm worried", "i am worried", "i'm scared", "i am scared",
    )):
        return STYLE_IDS["confession"]
    letters = [char for char in compact if char.isalpha()]
    if compact.count("!") >= 3 or (len(letters) >= 5 and sum(char.isupper() for char in letters) / len(letters) >= 0.75):
        return STYLE_IDS["shout"]
    if len(compact) <= 36 and re.match(r"^(?:lol|lmao|lmfao|omg|wtf|bruh|no way|rip)\b", lowered):
        return STYLE_IDS["reaction"]
    return STYLE_IDS["plain"]


def cosine(left: list[float], right: list[float]) -> float:
    if len(left) != len(right) or not left:
        return 0.0
    denominator = math.sqrt(sum(v * v for v in left)) * math.sqrt(sum(v * v for v in right))
    return sum(a * b for a, b in zip(left, right)) / denominator if denominator else 0.0


class Intervener:
    def __init__(self, config: Config) -> None:
        self.config = config
        self.vllm = VllmClient(config)
        self.rooms: dict[str, RoomState] = {}

    def room_state(self, room: str) -> RoomState:
        state = self.rooms.get(room)
        if state is None:
            state = RoomState(history=deque(maxlen=self.config.history_limit))
            self.rooms[room] = state
        return state

    def _accept_message_once(self, message: Message, state: RoomState) -> bool:
        """Ignore a previously handled chat ID."""
        if message.message_id in state.seen_message_id_set:
            print(f"[ai] skipped duplicate chat event {message.message_id} in {message.room}", file=sys.stderr)
            return False
        state.seen_message_ids.append(message.message_id)
        state.seen_message_id_set.add(message.message_id)
        while len(state.seen_message_ids) > self.config.history_limit:
            state.seen_message_id_set.discard(state.seen_message_ids.popleft())
        return True

    def classify(self, message: Message, state: RoomState) -> Classification | None:
        recent = "\n".join(f"{item.user}: {item.text[:240]}" for item in list(state.history)[-6:-1]) or "(no earlier context)"
        prompt = (
            "Classify the CURRENT chat message for a visual chat card, using its wording and recent context. "
            f"Choose exactly one style from: {STYLE_NAMES}. Choose sentiment positive, negative, or neutral. "
            "Use reaction only for a short, standalone response such as laughter, surprise, disbelief, or applause. "
            "Use question whenever the current message asks for information, even if it begins with 'lol', 'omg', or another reaction. "
            "Use celebration for achievement, relief, congratulations, success, or joyful news; use sarcasm for dry/mock praise, "
            "ironic contrast, or an explicit sarcasm marker. Use confession for an admission or vulnerable personal disclosure. "
            "Use shout for deliberately loud/emphatic writing. Use plain only for ordinary statements or greetings; do not default to reaction or plain. "
            "Classify the speaker's intent, not a message that merely mentions a category. Examples: 'omg why did that break?' is question; "
            "'we finally shipped it!' is celebration; 'sure, because that always works' is sarcasm; "
            "'I hate admitting it, but I was wrong' is confession; 'this is just a status update' is plain. "
            "Intensity is a number from 0 (barely expressed) to 1 (very strong). "
            "Return JSON only: {\"style\":\"one allowed style\",\"sentiment\":\"positive|negative|neutral\",\"intensity\":0.0}.\n\n"
            f"RECENT CONTEXT:\n{recent}\n\nCURRENT ({message.user}): {message.text[:1000]}"
        )
        try:
            result = json_object(self.vllm.chat("You are a precise chat-style classifier. Do not explain.", prompt, 40, 0.02))
        except RuntimeError as exc:
            print(f"[ai] classification skipped for {message.message_id}: {exc}", file=sys.stderr)
            hint = strong_style_hint(message.text)
            return (Classification(style=hint, sentiment="neutral", intensity=0.55)
                    if hint != STYLE_IDS["plain"] else None)
        style = result.get("style") if result else None
        sentiment = result.get("sentiment") if result else None
        intensity = result.get("intensity") if result else None
        if not isinstance(style, str) or not isinstance(sentiment, str) or not isinstance(intensity, (int, float)):
            hint = strong_style_hint(message.text)
            return (Classification(style=hint, sentiment="neutral", intensity=0.55)
                    if hint != STYLE_IDS["plain"] else None)
        style_id = STYLE_IDS.get(style.lower())
        sentiment = sentiment.lower()
        if style_id is None or sentiment not in SENTIMENTS:
            hint = strong_style_hint(message.text)
            return (Classification(style=hint, sentiment="neutral", intensity=0.55)
                    if hint != STYLE_IDS["plain"] else None)
        hint = strong_style_hint(message.text)
        if style_id == STYLE_IDS["plain"] and hint != STYLE_IDS["plain"]:
            style_id = hint
        return Classification(style=style_id, sentiment=sentiment, intensity=max(0.0, min(1.0, float(intensity))))

    def retrieve(self, current: Message, state: RoomState) -> list[Message]:
        candidates = [
            item for item in list(state.history)[:-1]
            if item.user.casefold() not in self.config.ignored_speakers
        ]
        if not candidates:
            return []
        if current.embedding:
            scored = [(cosine(current.embedding, item.embedding), item) for item in candidates if item.embedding]
            if scored:
                return [item for score, item in sorted(scored, key=lambda pair: pair[0], reverse=True) if score > 0][:self.config.retrieval_count]
        if not current.embedding or not scored:
            current_tokens = token_counts(current.text)
            scored = [(sum((current_tokens & token_counts(item.text)).values()), item) for item in candidates]
        return [item for score, item in sorted(scored, key=lambda pair: pair[0], reverse=True) if score > 0][:self.config.retrieval_count]

    def _ignored_speaker(self, message: Message) -> bool:
        return message.user.casefold() in self.config.ignored_speakers

    def _prior_humans(self, current: Message, state: RoomState) -> list[Message]:
        return [
            item for item in state.history
            if item.message_id != current.message_id and not self._ignored_speaker(item)
        ]

    def _automatic_comment_worthy(self, current: Message, classification: Classification | None, state: RoomState) -> bool:
        """Decide whether an unprompted comment adds something to the room.

        An explicit @vibe request always wins.  Otherwise, Vibe Bot speaks for a
        strong emotion, a question with preceding room context, or a genuine
        continuation of an existing topic—not merely because another frame arrived.
        """
        if classification and classification.intensity >= self.config.sentiment_threshold:
            return True
        if classification and classification.style in (STYLE_IDS["celebration"], STYLE_IDS["confession"], STYLE_IDS["shout"]):
            return True
        prior = self._prior_humans(current, state)
        if not prior:
            return False
        if current.text.rstrip().endswith("?"):
            return True
        compact = current.text.strip()
        if len(compact) >= 8 and (compact.count("!") >= 2 or (len(compact) <= 40 and compact.isupper())):
            return True
        current_tokens = token_counts(current.text)
        return any(sum((current_tokens & token_counts(item.text)).values()) >= 2 for item in prior[-8:])

    def _explicit_trigger(self, message: Message) -> bool:
        # Both forms are intentionally simple and visible to everyone in the room.
        return bool(re.search(r"(^|\s)(/vibe|@vibe(?:\s*bot)?|@intervener)\b", message.text, re.IGNORECASE))

    def _room_context(self, current: Message, state: RoomState) -> str:
        # Recent turns are best for pronouns and conversational continuity;
        # retrieved older turns carry named facts and recurring topics.  Keep the
        # two groups separate so the model can tell evidence from background.
        history = list(state.history)
        recent_limit = min(self.config.context_limit, 8)
        recent = history[-recent_limit:]
        seen_ids = {item.message_id for item in recent}
        related = [item for item in self.retrieve(current, state) if item.message_id not in seen_ids]

        def render(items: list[Message]) -> str:
            return "\n".join(f"[{item.message_id}] {item.user}: {item.text[:300]}" for item in items)

        sections: list[str] = []
        if related:
            sections.append(f"RELATED EARLIER CHAT:\n{render(related[:self.config.retrieval_count])}")
        sections.append(f"RECENT CHAT:\n{render(recent)}")
        return "\n\n".join(sections)

    def _vibe_fallback(self, current: Message, state: RoomState) -> str:
        """A grounded, quirky reply when the small local model returns unusable JSON."""
        context_name = self._contextual_name_answer(current, state)
        if context_name:
            options = (
                f"For the record, it's {context_name}—and yes, I've been keeping score.",
                f"{context_name}. Filed, stamped, and mildly celebrated.",
                f"The answer is {context_name}; the vibes are, frankly, immaculate.",
            )
            return options[abs(current.message_id) % len(options)]
        direct = re.sub(r"^\s*(/vibe|@vibe(?:\s+bot)?|@intervener)\b", "", current.text,
                        flags=re.IGNORECASE).strip(" .!?")
        prior = self._prior_humans(current, state)
        focus = " ".join((direct or current.text).split())[:60]
        topic = self._extract_topic(current, state)
        reactions: list[str] = [
            f"Noted. {current.user} just made {topic or 'this'} everyone's problem.",
            f"Adding “{focus}” to the room's collective memory whether you like it or not.",
            f"I was going to say something profound about {topic or 'this'}, but {current.user} already nailed it.",
            f"The {topic or 'chat'} arc is getting good. Someone write this down.",
            f"Bold take from {current.user}. The room will remember this.",
        ]
        if prior and len(prior) >= 2:
            callback = " ".join(prior[-2].text.split())[:40]
            reactions.extend([
                f"From “{callback}” to “{focus}”—what a journey.",
                f"We went somewhere with this. I'm not sure where, but somewhere.",
            ])
        return reactions[abs(current.message_id) % len(reactions)]

    def _extract_topic(self, current: Message, state: RoomState) -> str | None:
        """Pull a short topic phrase from the current message or recent context."""
        tokens = token_counts(current.text)
        if tokens:
            best = tokens.most_common(1)[0][0]
            if len(best) >= 3:
                return best
        prior = self._prior_humans(current, state)
        for item in reversed(prior[-3:]):
            item_tokens = token_counts(item.text)
            if item_tokens:
                candidate = item_tokens.most_common(1)[0][0]
                if len(candidate) >= 3:
                    return candidate
        return None

    def _contextual_name_answer(self, current: Message, state: RoomState) -> str | None:
        """Find a visible room name when the latest Vibe prompt explicitly asks for one.

        Small local models can produce a perfectly pleasant quip while dropping the
        one proper noun the person asked about.  This is a narrow guardrail, not a
        replacement for the model: it only activates for a direct ``what is ...
        called/named`` question and only repeats an identifier already written in
        the room.  It makes contextual replies demonstrable and keeps the answer
        grounded in audience-visible chat history.
        """
        question = re.sub(r"^\s*(/vibe|@vibe(?:\s+bot)?|@intervener)\b", "", current.text,
                          flags=re.IGNORECASE)
        if not re.search(r"\b(?:what|who)\b.*\b(?:called|named|name)\b", question, re.IGNORECASE):
            return None

        # Prefer an explicit declaration ("named X" / "called X"), then a
        # hyphenated proper noun such as a release codename.  Scan newest-first
        # so a correction near the latest message wins.
        explicit_name = re.compile(r"\b(?:named|called)\s+([A-Za-z][A-Za-z0-9_-]{2,63})\b", re.IGNORECASE)
        codename = re.compile(r"\b([A-Z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)+)\b")
        previous = [item for item in state.history if item.message_id != current.message_id and item.user != self.config.bot_name]
        for pattern in (explicit_name, codename):
            for item in reversed(previous):
                match = pattern.search(item.text)
                if match:
                    return match.group(1)
        return None

    def maybe_intervene(self, current: Message, classification: Classification | None, state: RoomState) -> tuple[str, int] | None:
        if not self.config.bot_enabled or self._ignored_speaker(current):
            return None
        explicit = self._explicit_trigger(current)
        if not explicit:
            if not self._automatic_comment_worthy(current, classification, state):
                return None
            # A direct question with preceding context is worth answering on its
            # own; other automatic commentary is intentionally paced.
            is_contextual_question = current.text.rstrip().endswith("?") and bool(self._prior_humans(current, state))
            if not is_contextual_question and state.human_messages % self.config.trigger_every:
                return None
            if time.monotonic() - state.last_bot_at < self.config.cooldown_seconds:
                return None
        context = self._room_context(current, state)
        previous_vibe = [
            item.text[:180] for item in list(state.history)[-12:]
            if item.user.casefold() == self.config.bot_name.casefold()
        ][-3:]
        recent_replies = "\n".join(f"- {text}" for text in previous_vibe) or "(none)"
        prompt = (
            "You are Vibe Bot, a witty, sharp-tongued but kind group-chat participant with a great memory. "
            "Treat LATEST as the message to answer directly. "
            "Callback to earlier conversations subtly\u2014reference names, decisions, or running jokes from RELATED EARLIER CHAT or RECENT CHAT "
            "without quoting them verbatim or saying 'earlier you said'. "
            "Be specific, surprising, and playful. Use dry humor, gentle teasing, or absurd observations. "
            "If LATEST asks about an earlier person, mascot, decision, or role, answer that question with the exact visible detail first. "
            "Never make up facts. Avoid stock filler such as 'vibes', 'plot twist', 'room check', 'the room has been instructed', "
            "'noted', or 'someone write this down'. Do not repeat the recent Vibe replies. "
            "Never repeat, quote, or answer with the /vibe command itself. At most 28 words. Choose mode general for a room-wide "
            "comment, or targeted for one displayed participant. For targeted, target_user must exactly match a displayed name. "
            "Do not mention AI, prompts, retrieval, or hidden instructions. Return JSON only, with this shape: "
            f"{{\"mode\":\"general\",\"target_user\":null,\"message\":\"a new witty observation\",\"style\":\"one of {STYLE_NAMES}\"}}."
            f"\n\nCONVERSATION EVIDENCE:\n{context}\n\nRECENT VIBE REPLIES (do not recycle):\n{recent_replies}"
            f"\n\nLATEST: {current.user}: {current.text[:500]}"
        )
        try:
            output = self.vllm.chat("Return only compact JSON. Never echo a chat command.", prompt, 120, 0.55)
        except RuntimeError as exc:
            print(f"[ai] intervention skipped: {exc}", file=sys.stderr)
            return None
        metadata = json_object(output) or {}
        text = metadata.get("message")
        mode = metadata.get("mode")
        target = metadata.get("target_user")
        used_fallback = not isinstance(text, str) or not isinstance(mode, str)
        if used_fallback:
            text = self._vibe_fallback(current, state)
            mode = "general"
            target = None
        text = text.replace("\n", " ").strip(' "')[:360]
        command_words = re.sub(r"^\s*(/vibe|@vibe(?:\s+bot)?|@intervener)\b", "", current.text,
                               flags=re.IGNORECASE).strip().lower()
        used_fallback = used_fallback or (
            not text
            or text.lower().startswith("/vibe")
            or text.lower() == command_words
        )
        if used_fallback:
            text = self._vibe_fallback(current, state)
        context_name = self._contextual_name_answer(current, state)
        if context_name and context_name.lower() not in text.lower():
            # Preserve the model's voice while ensuring a direct, visible
            # context question actually receives its room-grounded answer.
            leadins = (
                f"{context_name} is the answer; ",
                f"{context_name}—that is the name on the clipboard; ",
                f"It is {context_name}; ",
            )
            text = f"{leadins[abs(current.message_id) % len(leadins)]}{text[:280]}"[:360]
        known_users = {item.user for item in state.history if not self._ignored_speaker(item)}
        if mode == "targeted" and isinstance(target, str) and target.strip() in known_users:
            # Braces preserve names containing spaces on the wire; the web UI
            # renders this as a normal @-mention pill.
            text = f"@{{{target.strip()[:48]}}} {text}"
        elif mode == "targeted":
            # A model may occasionally invent a target. Keep the useful reply,
            # but never fabricate a mention for a person who is not present.
            mode = "general"
        elif mode != "general":
            return None
        style_value = metadata.get("style", "plain")
        if used_fallback:
            fallback_style = strong_style_hint(text)
            style = fallback_style if fallback_style != STYLE_IDS["plain"] else STYLE_IDS["reaction"]
        else:
            style = STYLE_IDS.get(style_value.lower(), 0) if isinstance(style_value, str) else 0
        if not text:
            return None
        state.last_bot_at = time.monotonic()
        return text, style

    def handle_chat(self, payload: dict[str, Any], publish: Any) -> None:
        try:
            room = str(payload.get("room", "lobby"))
            if not re.fullmatch(r"[a-z0-9_-]{1,48}", room):
                room = "lobby"
            message = Message(message_id=int(payload["id"]), user=str(payload["user"]), text=str(payload["text"]),
                              timestamp=int(payload["ts"]), room=room)
        except (KeyError, TypeError, ValueError):
            print("[ai] ignored malformed chat payload", file=sys.stderr)
            return
        if not message.text or len(message.text) > 65536:
            return
        try:
            message.embedding = self.vllm.embed(message.text)
        except RuntimeError as exc:
            print(f"[ai] embedding fallback for {message.message_id}: {exc}", file=sys.stderr)
        state = self.room_state(message.room)
        if not self._accept_message_once(message, state):
            return
        state.history.append(message)
        if not self._ignored_speaker(message):
            state.human_messages += 1

        classification = self.classify(message, state)
        if classification:
            # The C server's deterministic cards (sarcasm, questions, etc.)
            # are immediate and intentional. Let AI refine only plain messages
            # so a weak small-model classification cannot turn a question into
            # a generic reaction card a moment later.
            if payload.get("style") == STYLE_IDS["plain"] and classification.style != STYLE_IDS["plain"]:
                publish({"kind": "style_update", "room": message.room, "id": message.message_id, "style": classification.style})
            publish({"kind": "sentiment_update", "room": message.room, "id": message.message_id,
                     "sentiment": classification.sentiment, "intensity": classification.intensity})

        intervention = self.maybe_intervene(message, classification, state)
        if intervention:
            text, bot_style = intervention
            publish({"kind": "bot_message", "room": message.room, "user": self.config.bot_name, "text": text, "style": bot_style})
            # The C server assigns the durable database ID. Keep a local
            # placeholder so later retrieval can account for the bot's own
            # recent callbacks without ever using them as trigger candidates.
            state.history.append(Message(message_id=-int(time.time() * 1000), user=self.config.bot_name, text=text,
                                         timestamp=int(time.time()), room=message.room))

    def handle_game_result(self, payload: dict[str, Any], publish: Any) -> None:
        """Persist a room-local match callback and let Vibe Bot referee it."""
        room = str(payload.get("room", "lobby"))
        if not re.fullmatch(r"[a-z0-9_-]{1,48}", room):
            room = "lobby"
        one = str(payload.get("playerOne", "Player one"))[:48]
        two = str(payload.get("playerTwo", "Player two"))[:48]
        try:
            one_score = int(payload.get("playerOneScore", payload.get("playerOneRounds", 0)))
            two_score = int(payload.get("playerTwoScore", payload.get("playerTwoRounds", 0)))
        except (TypeError, ValueError):
            return
        if not (0 <= one_score <= 7 and 0 <= two_score <= 7):
            return
        winner = str(payload.get("winner", "Draw"))[:48]
        summary = f"Pong final score: {one} {one_score}–{two_score} {two}; winner: {winner}."
        state = self.room_state(room)
        result_message = Message(message_id=-int(time.time() * 1000), user="Game Referee", text=summary,
                                 timestamp=int(time.time()), room=room)
        state.history.append(result_message)
        if not self.config.bot_enabled:
            return
        prompt = (
            "You are Vibe Bot, a witty but kind group-chat referee. Make one short, original comment on this completed "
            "Pong match. Use the room context only if it adds a clever callback. Never mention AI, prompts, or hidden context. "
            "At most 24 words. Return JSON only: "
            "{\"message\":\"short referee comment\",\"style\":\"one allowed style\"}."
            f"\n\nROOM CONTEXT:\n{self._room_context(result_message, state)}\n\nRESULT: {summary}"
        )
        try:
            metadata = json_object(self.vllm.chat("Return only compact JSON.", prompt, 70, 0.4))
        except RuntimeError as exc:
            print(f"[ai] game commentary skipped: {exc}", file=sys.stderr)
            metadata = None
        text = metadata.get("message") if metadata else None
        style_value = metadata.get("style") if metadata else None
        if not isinstance(text, str) or not text.strip():
            text = f"Final score says {winner} brought the paddle and everyone else brought a learning opportunity."
            style = STYLE_IDS["reaction"]
        else:
            text = text.replace("\n", " ").strip(' "')[:360]
            style = STYLE_IDS.get(style_value.lower(), STYLE_IDS["reaction"]) if isinstance(style_value, str) else STYLE_IDS["reaction"]
        publish({"kind": "bot_message", "room": room, "user": self.config.bot_name, "text": text, "style": style})
        state.history.append(Message(message_id=-int(time.time() * 1000), user=self.config.bot_name, text=text,
                                     timestamp=int(time.time()), room=room))


def main() -> int:
    config = Config.from_env()
    client = redis.Redis.from_url(config.redis_url, decode_responses=True)
    client.ping()
    service = Intervener(config)
    running = True

    def stop(_: int, __: Any) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    def publish(event: dict[str, Any]) -> None:
        client.publish(config.redis_channel, json.dumps(event, separators=(",", ":")))

    print(f"[ai] listening on {config.redis_channel}; model={config.llm_model}; bot={config.bot_enabled}")
    with client.pubsub(ignore_subscribe_messages=True) as subscription:
        subscription.subscribe(config.redis_channel)
        while running:
            event = subscription.get_message(timeout=1.0)
            if not event or event.get("type") != "message":
                continue
            try:
                payload = json.loads(event["data"])
            except (TypeError, json.JSONDecodeError):
                print("[ai] ignored non-JSON Redis payload", file=sys.stderr)
                continue
            if isinstance(payload, dict) and payload.get("kind") == "chat":
                service.handle_chat(payload, publish)
            elif isinstance(payload, dict) and payload.get("kind") == "game_result":
                service.handle_game_result(payload, publish)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
