import type { MessageStyle } from "@/lib/protocol";

export type SentimentLabel = "positive" | "negative" | "neutral";

export type DeliveryStatus = "sending" | "persisted" | "delivered" | "failed";

export interface ChatMessage {
  id: number;
  timestamp: number;
  username: string;
  text: string;
  style: MessageStyle;
  sentiment?: SentimentLabel;
  intensity?: number;
  clientId?: string;
  delivery?: DeliveryStatus;
}

export interface PresenceUser {
  id: string;
  name: string;
}

export interface RoomInfo {
  slug: string;
  ownerId: string;
  chaosEnabled: boolean;
}

export interface ChaosProfile {
  enabled: boolean;
  autoCycle: boolean;
  minDelayMs: number;
  maxDelayMs: number;
  reorderWindowMs: number;
  duplicatePercent: number;
  cycleMinMs: number;
  cycleMaxMs: number;
}

export type ConnectionStatus =
  | "idle"
  | "connecting"
  | "authenticating"
  | "connected"
  | "auth-failed"
  | "disconnected";

export interface ActivityEntry {
  id: number;
  at: number;
  label: string;
  detail?: string;
}

export interface ChallengeItem {
  id: string;
  requesterId: string;
  requesterName: string;
  targetId: string;
  targetName: string;
  action: "incoming" | "requested";
}

export interface PvPGame {
  gameId: string;
  role: "host" | "guest";
  opponentId: string;
  opponentName: string;
}

export interface GameResult {
  id?: number;
  gameId: string;
  playerOne: string;
  playerOneScore: number;
  playerTwo: string;
  playerTwoScore: number;
  winner: string;
  createdAt?: number;
}

export interface PvPState {
  gameId: string;
  leftY: number;
  rightY: number;
  ballX: number;
  ballY: number;
  ballVX: number;
  ballVY: number;
  leftScore: number;
  rightScore: number;
  complete: boolean;
}

export interface SignalEvent {
  targetId: string;
  from?: string;
  kind: "offer" | "answer" | "ice" | "reject" | "error";
  sdp?: string;
  candidate?: RTCIceCandidateInit;
  msg?: string;
}

export interface PendingUpdate {
  sentiment?: SentimentLabel;
  intensity?: number;
  style?: MessageStyle;
}

export interface ChatState {
  status: ConnectionStatus;
  authenticated: boolean;
  myUsername: string;
  error: string | null;
  currentRoom: string;
  isRoomOwner: boolean;
  chaos: ChaosProfile;
  rooms: RoomInfo[];
  users: PresenceUser[];
  messages: ChatMessage[];
  activity: ActivityEntry[];
  challenges: ChallengeItem[];
  matches: GameResult[];
  activePvp: PvPGame | null;
  pvpState: PvPState | null;
  lastGameEvent: Record<string, unknown> | null;
  signalEvent: SignalEvent | null;
  callRejected: number;
  nextActivityId: number;
  nextLocalId: number;
  pendingUpdates: Record<number, PendingUpdate>;
}

export const DEFAULT_CHAOS: ChaosProfile = {
  enabled: false,
  autoCycle: false,
  minDelayMs: 500,
  maxDelayMs: 5000,
  reorderWindowMs: 750,
  duplicatePercent: 10,
  cycleMinMs: 30000,
  cycleMaxMs: 90000,
};

export const INITIAL_CHAT_STATE: ChatState = {
  status: "idle",
  authenticated: false,
  myUsername: "",
  error: null,
  currentRoom: "lobby",
  isRoomOwner: false,
  chaos: DEFAULT_CHAOS,
  rooms: [],
  users: [],
  messages: [],
  activity: [
    {
      id: 1,
      at: Date.now(),
      label: "client ready",
      detail: "React state store initialized",
    },
  ],
  challenges: [],
  matches: [],
  activePvp: null,
  pvpState: null,
  lastGameEvent: null,
  signalEvent: null,
  callRejected: 0,
  nextActivityId: 2,
  nextLocalId: -1,
  pendingUpdates: {},
};
