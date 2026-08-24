// Wire protocol shared with chat_server.c:
//   client -> server: [type][user_len][username][msg_len][message]
//   server -> client: [type][id][timestamp][style][user_len][username][msg_len][message]
// All integers are big-endian (network byte order).
//
// OAUTH_LOGIN repurposes the "message" field to carry a Supabase JWT
// access token instead of chat text. The "username" field is unused for
// that type -- the server derives identity entirely from the verified
// token, never from a client-supplied field.
//
// MSG_SIGNAL / MSG_CALL_REJECT repurpose the "username" field to carry
// the TARGET user_id (Supabase UUID). The server injects "from" into the
// JSON payload and forwards to the named recipient.

export const MSG_JOIN = 1;
export const MSG_LEAVE = 2;
export const MSG_CHAT = 3;
export const MSG_OAUTH_LOGIN = 4;
export const MSG_AUTH_OK = 6;
export const MSG_AUTH_FAIL = 7;
export const MSG_USERS_LIST = 8;
export const MSG_HISTORY_REQUEST = 9;
export const MSG_HISTORY_RESPONSE = 10;
export const MSG_SIGNAL = 11;      // WebRTC: offer / answer / ICE candidate
export const MSG_CALL_REJECT = 12; // Call declined or hung up
export const MSG_STYLE_UPDATE = 13; // async style refinement for an existing message
// async sentiment refinement: text is JSON { sentiment, intensity }
export const MSG_SENTIMENT_UPDATE = 14;
// Public room discovery/switching and owner-only chaos controls.
export const MSG_ROOM_LIST = 15;
export const MSG_ROOM_JOIN = 16;
export const MSG_CHAOS_UPDATE = 17;
// A durable acknowledgement for a client-generated outgoing-message ID.
export const MSG_CHAT_ACK = 18;
// Challenge flow and real-time player-vs-player game relay.
export const MSG_CHALLENGE = 19;
export const MSG_GAME_EVENT = 20;
export const MSG_GAME_RESULT = 21;
// Room-scoped, durable game-results ledger.
export const MSG_GAME_HISTORY = 22;

export const MESSAGE_STYLE_PLAIN = 0;
export const MESSAGE_STYLE_REACTION = 1;
export const MESSAGE_STYLE_SHOUT = 2;
export const MESSAGE_STYLE_CONFESSION = 3;
export const MESSAGE_STYLE_QUESTION = 4;
export const MESSAGE_STYLE_CELEBRATION = 5;
export const MESSAGE_STYLE_SARCASM = 6;

export type MessageStyle =
  | typeof MESSAGE_STYLE_PLAIN
  | typeof MESSAGE_STYLE_REACTION
  | typeof MESSAGE_STYLE_SHOUT
  | typeof MESSAGE_STYLE_CONFESSION
  | typeof MESSAGE_STYLE_QUESTION
  | typeof MESSAGE_STYLE_CELEBRATION
  | typeof MESSAGE_STYLE_SARCASM;

export type MessageType =
  | typeof MSG_JOIN
  | typeof MSG_LEAVE
  | typeof MSG_CHAT
  | typeof MSG_OAUTH_LOGIN
  | typeof MSG_AUTH_OK
  | typeof MSG_AUTH_FAIL
  | typeof MSG_USERS_LIST
  | typeof MSG_HISTORY_REQUEST
  | typeof MSG_HISTORY_RESPONSE
  | typeof MSG_SIGNAL
  | typeof MSG_CALL_REJECT
  | typeof MSG_STYLE_UPDATE
  | typeof MSG_SENTIMENT_UPDATE
  | typeof MSG_ROOM_LIST
  | typeof MSG_ROOM_JOIN
  | typeof MSG_CHAOS_UPDATE
  | typeof MSG_CHAT_ACK
  | typeof MSG_CHALLENGE
  | typeof MSG_GAME_EVENT
  | typeof MSG_GAME_RESULT
  | typeof MSG_GAME_HISTORY;

export interface ParsedFrame {
  type: MessageType;
  id?: number;
  timestamp?: number;
  style: MessageStyle;
  username: string;
  text: string;
}

// Payload carried in the msg field of a MSG_SIGNAL frame.
export interface SignalPayload {
  kind: 'offer' | 'answer' | 'ice' | 'reject' | 'error';
  from?: string;                    // server-injected caller user_id
  sdp?: string;                     // offer / answer SDP
  candidate?: RTCIceCandidateInit;  // ICE candidate
  msg?: string;                     // human-readable error text
}

export function buildFrame(type: MessageType, username: string, text: string): ArrayBuffer {
  const enc = new TextEncoder();
  const userBytes = enc.encode(username);
  const textBytes = enc.encode(text);

  const buf = new ArrayBuffer(1 + 4 + userBytes.length + 4 + textBytes.length);
  const view = new DataView(buf);
  let offset = 0;

  view.setUint8(offset, type);
  offset += 1;

  view.setUint32(offset, userBytes.length, false); // false = big-endian
  offset += 4;
  new Uint8Array(buf, offset, userBytes.length).set(userBytes);
  offset += userBytes.length;

  view.setUint32(offset, textBytes.length, false);
  offset += 4;
  new Uint8Array(buf, offset, textBytes.length).set(textBytes);

  return buf;
}

// Convenience wrapper for WebRTC signaling frames.
// targetUserId goes in the username field (server routes by this).
// payload is serialised to JSON in the message field.
export function buildSignalFrame(
  type: typeof MSG_SIGNAL | typeof MSG_CALL_REJECT,
  targetUserId: string,
  payload: Omit<SignalPayload, 'from'>
): ArrayBuffer {
  return buildFrame(type, targetUserId, JSON.stringify(payload));
}

export function parseFrame(buf: ArrayBuffer): ParsedFrame {
  const view = new DataView(buf);
  const dec = new TextDecoder();
  let offset = 0;

  const type = view.getUint8(offset) as MessageType;
  offset += 1;

  const idBig = view.getBigUint64(offset, false);
  const id = Number(idBig);
  offset += 8;

  const timestampBig = view.getBigUint64(offset, false);
  const timestamp = Number(timestampBig);
  offset += 8;

  const style = view.getUint8(offset) as MessageStyle;
  offset += 1;

  const userLen = view.getUint32(offset, false);
  offset += 4;
  const username = dec.decode(new Uint8Array(buf, offset, userLen));
  offset += userLen;

  const msgLen = view.getUint32(offset, false);
  offset += 4;
  const text = dec.decode(new Uint8Array(buf, offset, msgLen));

  return { type, id, timestamp, style, username, text };
}
