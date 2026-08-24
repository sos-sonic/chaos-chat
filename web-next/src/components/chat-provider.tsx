"use client";

import {
  createContext,
  useContext,
  useCallback,
  useEffect,
  useReducer,
  useRef,
  type ReactNode,
} from "react";
import {
  buildFrame,
  buildSignalFrame,
  MSG_AUTH_FAIL,
  MSG_AUTH_OK,
  MSG_CHAT,
  MSG_CHAT_ACK,
  MSG_CHAOS_UPDATE,
  MSG_CALL_REJECT,
  MSG_CHALLENGE,
  MSG_GAME_EVENT,
  MSG_GAME_HISTORY,
  MSG_GAME_RESULT,
  MSG_HISTORY_REQUEST,
  MSG_HISTORY_RESPONSE,
  MSG_JOIN,
  MSG_LEAVE,
  MSG_OAUTH_LOGIN,
  MSG_ROOM_JOIN,
  MSG_ROOM_LIST,
  MSG_SENTIMENT_UPDATE,
  MSG_SIGNAL,
  MSG_STYLE_UPDATE,
  MSG_USERS_LIST,
  parseFrame,
  type MessageStyle,
  type ParsedFrame,
  type SignalPayload,
} from "@/lib/protocol";
import {
  DEFAULT_CHAOS,
  INITIAL_CHAT_STATE,
  type ActivityEntry,
  type ChatMessage,
  type ChatState,
  type ChaosProfile,
  type ConnectionStatus,
  type GameResult,
  type PendingUpdate,
  type PresenceUser,
  type PvPGame,
  type RoomInfo,
  type SignalEvent,
  type SentimentLabel,
} from "@/lib/chat-types";


const ROOM_MESSAGE_CAP = 50;

type ChatAction =
  | { type: "connection"; status: ConnectionStatus; detail?: string }
  | { type: "auth-ok"; username: string }
  | { type: "auth-fail"; error: string }
  | { type: "presence"; users: PresenceUser[] }
  | { type: "system"; text: string }
  | { type: "chat"; message: ChatMessage; history: boolean }
  | { type: "optimistic-message"; message: ChatMessage }
  | { type: "chat-ack"; clientId: string; id: number; timestamp: number; ok: boolean }
  | { type: "style-update"; id: number; style: MessageStyle }
  | { type: "sentiment-update"; id: number; sentiment: SentimentLabel; intensity: number }
  | { type: "rooms"; rooms: RoomInfo[] }
  | { type: "room-metadata"; payload: RoomMetadata; source: "join" | "chaos" }
  | { type: "challenge"; event: Record<string, unknown> }
  | { type: "game-event"; event: Record<string, unknown> }
  | { type: "game-result"; result: GameResult }
  | { type: "game-history"; results: GameResult[] }
  | { type: "signal"; event: SignalEvent }
  | { type: "call-rejected" }
  | { type: "activity"; label: string; detail?: string };

interface RoomMetadata {
  room?: string;
  owner?: boolean;
  chaos?: Partial<ChaosProfile>;
  error?: string;
}

function activity(state: ChatState, label: string, detail?: string): ChatState {
  const entry: ActivityEntry = { id: state.nextActivityId, at: Date.now(), label, detail };
  return { ...state, activity: [...state.activity, entry].slice(-40), nextActivityId: state.nextActivityId + 1 };
}

function systemMessage(state: ChatState, text: string): ChatState {
  const message: ChatMessage = {
    id: state.nextLocalId,
    timestamp: Date.now() / 1000,
    username: "system",
    text,
    style: 0,
  };
  return {
    ...state,
    nextLocalId: state.nextLocalId - 1,
    messages: trimMessages([...state.messages, message]),
  };
}

function parseRoomMetadata(raw: string): RoomMetadata | null {
  try {
    const value: unknown = JSON.parse(raw);
    if (!value || typeof value !== "object") return null;
    return value as RoomMetadata;
  } catch {
    return null;
  }
}

function normalizedChaos(input?: Partial<ChaosProfile>): ChaosProfile {
  return {
    enabled: input?.enabled === true,
    autoCycle: input?.autoCycle === true,
    minDelayMs: Number(input?.minDelayMs ?? DEFAULT_CHAOS.minDelayMs),
    maxDelayMs: Number(input?.maxDelayMs ?? DEFAULT_CHAOS.maxDelayMs),
    reorderWindowMs: Number(input?.reorderWindowMs ?? DEFAULT_CHAOS.reorderWindowMs),
    duplicatePercent: Number(input?.duplicatePercent ?? DEFAULT_CHAOS.duplicatePercent),
    cycleMinMs: Number(input?.cycleMinMs ?? DEFAULT_CHAOS.cycleMinMs),
    cycleMaxMs: Number(input?.cycleMaxMs ?? DEFAULT_CHAOS.cycleMaxMs),
  };
}

function insertHistory(messages: ChatMessage[], incoming: ChatMessage): ChatMessage[] {
  const insertionIndex = messages.findIndex((message) => message.id > 0 && message.id > incoming.id);
  if (insertionIndex === -1) return [...messages, incoming];
  return [...messages.slice(0, insertionIndex), incoming, ...messages.slice(insertionIndex)];
}

function validGameResult(value: unknown): value is GameResult {
  if (!value || typeof value !== "object") return false;
  const result = value as Partial<GameResult>;
  return typeof result.gameId === "string" && typeof result.playerOne === "string" &&
    typeof result.playerTwo === "string" && typeof result.playerOneScore === "number" &&
    typeof result.playerTwoScore === "number" && typeof result.winner === "string";
}

function trimMessages(messages: ChatMessage[]): ChatMessage[] {
  return messages.length > ROOM_MESSAGE_CAP ? messages.slice(-ROOM_MESSAGE_CAP) : messages;
}

function applyPendingToMessage(
  message: ChatMessage,
  pending: PendingUpdate,
): ChatMessage {
  const updated = { ...message };
  if (pending.sentiment !== undefined) updated.sentiment = pending.sentiment;
  if (pending.intensity !== undefined) updated.intensity = pending.intensity;
  if (pending.style !== undefined) updated.style = pending.style;
  return updated;
}

function reducer(state: ChatState, action: ChatAction): ChatState {
  switch (action.type) {
    case "connection":
      return activity(
        { ...state, status: action.status, authenticated: action.status === "connected" },
        action.status,
        action.detail,
      );

    case "auth-ok":
      return activity(
        { ...state, status: "connected", authenticated: true, myUsername: action.username, error: null },
        "AUTH_OK",
        `identity: ${action.username}`,
      );

    case "auth-fail":
      return activity(
        { ...state, status: "auth-failed", authenticated: false, error: action.error },
        "AUTH_FAIL",
        action.error,
      );

    case "presence":
      return activity({ ...state, users: action.users }, "USERS_LIST", `${action.users.length} users`);

    case "system":
      return activity(systemMessage(state, action.text), "system message", action.text);

    case "optimistic-message":
      return activity(
        { ...state, messages: trimMessages([...state.messages, action.message]) },
        "local message",
        "optimistic render before server acknowledgement",
      );

    case "chat-ack": {
      const pending = state.pendingUpdates[action.id];
      const messages = state.messages.map((message) => {
        if (message.clientId !== action.clientId) return message;
        const updated = {
          ...message,
          id: action.id || message.id,
          timestamp: action.timestamp || message.timestamp,
          delivery: action.ok ? ("persisted" as const) : ("failed" as const),
        };
        return pending ? applyPendingToMessage(updated, pending) : updated;
      });
      const remainingPending = { ...state.pendingUpdates };
      delete remainingPending[action.id];
      return activity({ ...state, messages, pendingUpdates: remainingPending }, "CHAT_ACK", action.ok ? "message persisted" : "message rejected");
    }

    case "chat": {
      if (action.message.id <= 0) return state;
      const existing = state.messages.find((message) => message.id === action.message.id);
      if (existing) {
        const messages = state.messages.map((message) =>
          message.id === action.message.id && message.username === state.myUsername
            ? { ...message, timestamp: action.message.timestamp, style: action.message.style, delivery: "delivered" as const }
            : message,
        );
        return activity({ ...state, messages }, "MSG_CHAT duplicate", `server id ${action.message.id}`);
      }
      const messages = trimMessages(
        action.history
          ? insertHistory(state.messages, action.message)
          : [...state.messages, action.message],
      );
      return activity(
        { ...state, messages },
        action.history ? "HISTORY_RESPONSE" : "MSG_CHAT",
        `${action.message.username}: ${action.message.text.slice(0, 48)}`,
      );
    }

    case "style-update": {
      const hasMatch = state.messages.some((message) => message.id === action.id);
      if (hasMatch) {
        return activity(
          { ...state, messages: state.messages.map((message) => message.id === action.id ? { ...message, style: action.style } : message) },
          "STYLE_UPDATE",
          `message id ${action.id}`,
        );
      }
      const existing = state.pendingUpdates[action.id] ?? {};
      return activity(
        { ...state, pendingUpdates: { ...state.pendingUpdates, [action.id]: { ...existing, style: action.style } } },
        "STYLE_UPDATE",
        `queued for pending id ${action.id}`,
      );
    }

    case "sentiment-update": {
      const hasMatch = state.messages.some((message) => message.id === action.id);
      if (hasMatch) {
        return activity(
          {
            ...state,
            messages: state.messages.map((message) =>
              message.id === action.id ? { ...message, sentiment: action.sentiment, intensity: action.intensity } : message,
            ),
          },
          "SENTIMENT_UPDATE",
          `${action.sentiment} / ${action.intensity.toFixed(2)}`,
        );
      }
      const existing = state.pendingUpdates[action.id] ?? {};
      return activity(
        {
          ...state,
          pendingUpdates: {
            ...state.pendingUpdates,
            [action.id]: { ...existing, sentiment: action.sentiment, intensity: action.intensity },
          },
        },
        "SENTIMENT_UPDATE",
        `queued for pending id ${action.id}`,
      );
    }

    case "rooms":
      return activity({ ...state, rooms: action.rooms }, "ROOM_LIST", `${action.rooms.length} rooms`);

    case "room-metadata": {
      if (action.payload.error) return activity(systemMessage(state, action.payload.error), "room error", action.payload.error);
      if (!action.payload.room) return state;
      const roomChanged = state.currentRoom !== action.payload.room;
      const chaos = normalizedChaos(action.payload.chaos);
      const rooms = state.rooms.some((room) => room.slug === action.payload.room)
        ? state.rooms.map((room) => room.slug === action.payload.room ? { ...room, chaosEnabled: chaos.enabled } : room)
        : [{ slug: action.payload.room, ownerId: "", chaosEnabled: chaos.enabled }, ...state.rooms];
      const next = {
        ...state,
        currentRoom: action.payload.room,
        isRoomOwner: action.payload.owner === true,
        chaos,
        rooms,
        messages: roomChanged && action.source === "join" ? [] : state.messages,
        pendingUpdates: roomChanged && action.source === "join" ? {} : state.pendingUpdates,
      };
      return activity(next, action.source === "join" ? "ROOM_JOIN" : "CHAOS_UPDATE", `#${action.payload.room}`);
    }

    case "challenge": {
      const event = action.event;
      if (typeof event.error === "string") return activity(systemMessage(state, `-- challenge: ${event.error} --`), "CHALLENGE error", event.error);
      if (event.action === "accepted" && typeof event.gameId === "string" && (event.role === "host" || event.role === "guest") && typeof event.opponentId === "string" && typeof event.opponentName === "string") {
        const activePvp: PvPGame = { gameId: event.gameId, role: event.role, opponentId: event.opponentId, opponentName: event.opponentName };
        return activity({ ...state, activePvp, pvpState: null, challenges: [] }, "CHALLENGE accepted", `Pong vs ${event.opponentName}`);
      }
      if (typeof event.id !== "string") return state;
      if (event.action === "declined" || event.action === "cancelled") {
        return activity({ ...state, challenges: state.challenges.filter((challenge) => challenge.id !== event.id) }, `challenge ${event.action}`);
      }
      if ((event.action === "incoming" || event.action === "requested") && typeof event.requesterId === "string" && typeof event.requesterName === "string" && typeof event.targetId === "string" && typeof event.targetName === "string") {
        const challenge = { id: event.id, requesterId: event.requesterId, requesterName: event.requesterName, targetId: event.targetId, targetName: event.targetName, action: event.action } as const;
        const challenges = [...state.challenges.filter((item) => item.id !== challenge.id), challenge];
        return activity({ ...state, challenges }, `challenge ${event.action}`, challenge.action === "incoming" ? `${challenge.requesterName} challenged you` : `challenge sent to ${challenge.targetName}`);
      }
      return state;
    }

    case "game-event":
      return activity({ ...state, lastGameEvent: action.event, activePvp: action.event.kind === "quit" || action.event.kind === "abandoned" ? null : state.activePvp }, "GAME_EVENT", typeof action.event.kind === "string" ? action.event.kind : "relay");

    case "game-result":
      return activity(systemMessage({ ...state, matches: [action.result, ...state.matches.filter((item) => item.gameId !== action.result.gameId)], activePvp: state.activePvp?.gameId === action.result.gameId ? null : state.activePvp }, `Pong final: ${action.result.playerOne} ${action.result.playerOneScore} - ${action.result.playerTwoScore} ${action.result.playerTwo}. Winner: ${action.result.winner}`), "GAME_RESULT", action.result.winner);

    case "game-history":
      return activity({ ...state, matches: action.results }, "GAME_HISTORY", `${action.results.length} saved matches`);

    case "signal":
      return activity({ ...state, signalEvent: action.event }, "SIGNAL", action.event.kind);

    case "call-rejected":
      return activity({ ...state, callRejected: state.callRejected + 1 }, "CALL_REJECT");

    case "activity":
      return activity(state, action.label, action.detail);
  }
}

interface ChatContextValue {
  state: ChatState;
  sendChat: (text: string) => boolean;
  loadHistory: () => void;
  joinRoom: (room: string) => boolean;
  updateChaos: (profile: ChaosProfile) => boolean;
  requestRoomList: () => void;
  requestChallenge: (targetId: string) => boolean;
  respondToChallenge: (id: string, action: "accept" | "decline" | "cancel") => boolean;
  sendGameEvent: (event: Record<string, unknown>) => boolean;
  sendSignal: (type: typeof MSG_SIGNAL | typeof MSG_CALL_REJECT, targetId: string, payload: Record<string, unknown>) => boolean;
}

const ChatContext = createContext<ChatContextValue | null>(null);

interface ChatProviderProps {
  children: ReactNode;
  serverUrl: string;

  accessToken?: string;
  devName?: string;
  enabled?: boolean;
}

export function ChatProvider({ children, serverUrl, accessToken = "", devName = "", enabled = true }: ChatProviderProps) {
  const [state, dispatch] = useReducer(reducer, INITIAL_CHAT_STATE);
  const socketRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    if (!enabled || !serverUrl) return;

    let active = true;
    let reconnectTimer: ReturnType<typeof setTimeout> | undefined;
    let preventReconnect = false;

    const connect = () => {
      if (!active) return;
      dispatch({ type: "connection", status: "connecting", detail: serverUrl });
      const socket = new WebSocket(serverUrl);
      socket.binaryType = "arraybuffer";
      socketRef.current = socket;

      socket.onopen = () => {
        dispatch({ type: "connection", status: "authenticating", detail: devName ? "development identity" : "Supabase token" });
        socket.send(buildFrame(MSG_OAUTH_LOGIN, devName, accessToken));
      };

      socket.onmessage = (event: MessageEvent<ArrayBuffer>) => {
        if (!active || !(event.data instanceof ArrayBuffer)) return;
        let frame: ParsedFrame;
        try {
          frame = parseFrame(event.data);
        } catch {
          dispatch({ type: "activity", label: "invalid frame", detail: "could not decode binary payload" });
          return;
        }
        handleFrame(frame);
      };

      socket.onerror = () => dispatch({ type: "activity", label: "socket error", detail: "check the C server and WebSocket URL" });
      socket.onclose = () => {
        if (!active) return;
        socketRef.current = null;
        dispatch({ type: "connection", status: "disconnected" });
        if (!preventReconnect) reconnectTimer = setTimeout(connect, 2000);
      };
    };

    const handleFrame = (frame: ParsedFrame) => {
      const { type, id = 0, timestamp = 0, style, username, text } = frame;
      if (type === MSG_AUTH_OK) dispatch({ type: "auth-ok", username });
      else if (type === MSG_AUTH_FAIL) {
        preventReconnect = true;
        dispatch({ type: "auth-fail", error: text });
        socketRef.current?.close();
      } else if (type === MSG_JOIN) dispatch({ type: "system", text: `* ${username} joined *` });
      else if (type === MSG_LEAVE) dispatch({ type: "system", text: `* ${username} left *` });
      else if (type === MSG_USERS_LIST) {
        const users = text.split("\n").flatMap((line) => {
          const separator = line.indexOf("\t");
          if (separator < 1) return [];
          return [{ id: line.slice(0, separator), name: line.slice(separator + 1) }];
        });
        dispatch({ type: "presence", users });
      } else if (type === MSG_ROOM_LIST) {
        try {
          const parsed: unknown = JSON.parse(text);
          const rooms = Array.isArray(parsed) ? parsed.flatMap((room) => {
            if (!room || typeof room !== "object") return [];
            const record = room as Record<string, unknown>;
            if (typeof record.slug !== "string") return [];
            return [{ slug: record.slug, ownerId: typeof record.ownerId === "string" ? record.ownerId : "", chaosEnabled: record.chaosEnabled === true }];
          }) : [];
          dispatch({ type: "rooms", rooms });
        } catch {
          dispatch({ type: "activity", label: "invalid ROOM_LIST", detail: "server sent malformed JSON" });
        }
      } else if (type === MSG_ROOM_JOIN || type === MSG_CHAOS_UPDATE) {
        const payload = parseRoomMetadata(text);
        if (payload) {
          dispatch({ type: "room-metadata", payload, source: type === MSG_ROOM_JOIN ? "join" : "chaos" });
          const socket = socketRef.current;
          if (type === MSG_ROOM_JOIN && !payload.error && socket?.readyState === WebSocket.OPEN) {
            socket.send(buildFrame(MSG_GAME_HISTORY, "", ""));
            socket.send(buildFrame(MSG_HISTORY_REQUEST, "", "after:0"));
          }
        }
      } else if (type === MSG_CHAT_ACK) {
        try {
          const ack: unknown = JSON.parse(text);
          const record = ack && typeof ack === "object" ? ack as Record<string, unknown> : null;
          dispatch({ type: "chat-ack", clientId: username, id, timestamp, ok: record?.status === "persisted" });
        } catch {
          dispatch({ type: "chat-ack", clientId: username, id, timestamp, ok: false });
        }
      } else if (type === MSG_STYLE_UPDATE) dispatch({ type: "style-update", id, style });
      else if (type === MSG_SENTIMENT_UPDATE) {
        try {
          const update: unknown = JSON.parse(text);
          const record = update && typeof update === "object" ? update as Record<string, unknown> : null;
          if (record && (record.sentiment === "positive" || record.sentiment === "negative" || record.sentiment === "neutral") && typeof record.intensity === "number") {
            dispatch({ type: "sentiment-update", id, sentiment: record.sentiment, intensity: Math.max(0, Math.min(1, record.intensity)) });
          }
        } catch {
          dispatch({ type: "activity", label: "invalid SENTIMENT_UPDATE" });
        }
      } else if (type === MSG_CHALLENGE) {
        try {
          const event: unknown = JSON.parse(text);
          if (event && typeof event === "object") dispatch({ type: "challenge", event: event as Record<string, unknown> });
        } catch {
          dispatch({ type: "activity", label: "invalid CHALLENGE" });
        }
      } else if (type === MSG_GAME_EVENT) {
        try {
          const event: unknown = JSON.parse(text);
          if (event && typeof event === "object") dispatch({ type: "game-event", event: event as Record<string, unknown> });
        } catch {
          dispatch({ type: "activity", label: "invalid GAME_EVENT" });
        }
      } else if (type === MSG_GAME_RESULT) {
        try {
          const result: unknown = JSON.parse(text);
          if (validGameResult(result)) dispatch({ type: "game-result", result });
        } catch {
          dispatch({ type: "activity", label: "invalid GAME_RESULT" });
        }
      } else if (type === MSG_GAME_HISTORY) {
        try {
          const results: unknown = JSON.parse(text);
          if (Array.isArray(results)) dispatch({ type: "game-history", results: results.filter(validGameResult) });
        } catch {
          dispatch({ type: "activity", label: "invalid GAME_HISTORY" });
        }
      } else if (type === MSG_SIGNAL) {
        try {
          const payload: unknown = JSON.parse(text);
          if (payload && typeof payload === "object" && (payload as SignalPayload).kind) {
            dispatch({ type: "signal", event: { ...(payload as SignalPayload), targetId: "", from: username } });
          }
        } catch {
          dispatch({ type: "activity", label: "invalid SIGNAL" });
        }
      } else if (type === MSG_CALL_REJECT) {
        dispatch({ type: "call-rejected" });
      } else if (type === MSG_CHAT || type === MSG_HISTORY_RESPONSE) {
        if (id > 0) dispatch({ type: "chat", history: type === MSG_HISTORY_RESPONSE, message: { id, timestamp, username, text, style } });
      }
    };

    connect();
    return () => {
      active = false;
      preventReconnect = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      if (socketRef.current) {
        socketRef.current.onclose = null;
        socketRef.current.close();
        socketRef.current = null;
      }
    };
  }, [accessToken, devName, enabled, serverUrl]);

  const send = useCallback((type: Parameters<typeof buildFrame>[0], username: string, text: string) => {
    const socket = socketRef.current;
    if (!socket || socket.readyState !== WebSocket.OPEN || !state.authenticated) return false;
    socket.send(buildFrame(type, username, text));
    return true;
  }, [state.authenticated]);

  const sendChat = (text: string) => {
    const cleanText = text.trim();
    if (!cleanText) return false;
    const clientId = typeof crypto.randomUUID === "function"
      ? crypto.randomUUID().replace(/[^A-Za-z0-9_-]/g, "")
      : `msg${Date.now()}${Math.random().toString(36).slice(2)}`;
    const sent = send(MSG_CHAT, clientId, cleanText);
    if (!sent) return false;
    dispatch({
      type: "optimistic-message",
      message: { id: state.nextLocalId, timestamp: Date.now() / 1000, username: state.myUsername, text: cleanText, style: 0, clientId, delivery: "sending" },
    });
    return true;
  };

  const loadHistory = () => {
    const ids = state.messages.filter((message) => message.id > 0).map((message) => message.id);
    send(MSG_HISTORY_REQUEST, "", ids.length ? String(Math.min(...ids)) : "0");
  };

  const joinRoom = (room: string) => {
    const normalized = room.trim().toLowerCase();
    if (!/^[a-z0-9_-]{1,48}$/.test(normalized)) return false;
    return send(MSG_ROOM_JOIN, normalized, "");
  };

  const updateChaos = (profile: ChaosProfile) => send(MSG_CHAOS_UPDATE, "", JSON.stringify(profile));
  const requestRoomList = () => { send(MSG_ROOM_LIST, "", ""); };
  const requestChallenge = (targetId: string) => send(MSG_CHALLENGE, targetId, JSON.stringify({ action: "request", game: "pong" }));
  const respondToChallenge = (id: string, action: "accept" | "decline" | "cancel") => send(MSG_CHALLENGE, "", JSON.stringify({ action, id }));
  const sendGameEvent = useCallback((event: Record<string, unknown>) => state.activePvp ? send(MSG_GAME_EVENT, "", JSON.stringify({ gameId: state.activePvp.gameId, ...event })) : false, [send, state.activePvp]);
  const sendSignal = useCallback((type: typeof MSG_SIGNAL | typeof MSG_CALL_REJECT, targetId: string, payload: Record<string, unknown>) => {
    const socket = socketRef.current;
    if (!socket || socket.readyState !== WebSocket.OPEN || !state.authenticated) return false;
    socket.send(buildSignalFrame(type, targetId, payload as Omit<SignalPayload, "from">));
    return true;
  }, [state.authenticated]);

  return <ChatContext.Provider value={{ state, sendChat, loadHistory, joinRoom, updateChaos, requestRoomList, requestChallenge, respondToChallenge, sendGameEvent, sendSignal }}>{children}</ChatContext.Provider>;
}

export function useChat() {
  const context = useContext(ChatContext);
  if (!context) throw new Error("useChat must be used inside ChatProvider");
  return context;
}
