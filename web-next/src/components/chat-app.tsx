"use client";

import { createClient, type Session } from "@supabase/supabase-js";
import {
  createContext,
  startTransition,
  useContext,
  useEffect,
  useRef,
  useState,
  type FormEvent,
  type ReactNode,
} from "react";
import { ChatProvider, useChat } from "@/components/chat-provider";
import type { ChatMessage, ConnectionStatus } from "@/lib/chat-types";
import {
  MESSAGE_STYLE_CELEBRATION,
  MESSAGE_STYLE_CONFESSION,
  MESSAGE_STYLE_QUESTION,
  MESSAGE_STYLE_REACTION,
  MESSAGE_STYLE_SARCASM,
  MESSAGE_STYLE_SHOUT,
  MSG_CALL_REJECT,
  MSG_SIGNAL,
} from "@/lib/protocol";

interface ConnectionConfig {
  serverUrl: string;
  devName: string;
  accessToken: string;
  mode: "dev" | "oauth";
  supabaseUrl?: string;
  supabaseAnonKey?: string;
}

const ROOM_PRESETS: Record<string, { name: string; purpose: string }> = {
  lobby: { name: "The Atrium", purpose: "open chat · general chaos" },
  matchpoint: { name: "Matchpoint", purpose: "Pong challenges · bragging rights" },
  "signal-lab": { name: "Signal Lab", purpose: "Vibe Bot · sentiment tests" },
  "green-room": { name: "Green Room", purpose: "quiet notes · low traffic" },
  "after-hours": { name: "After Hours", purpose: "late-night conversations" },
};

const ROOM_ORDER = ["lobby", "matchpoint", "signal-lab", "green-room", "after-hours"] as const;
const CHAOTIC_ROOMS = new Set(["matchpoint", "signal-lab"]);

const ENV_SERVER_URL = process.env.NEXT_PUBLIC_CHAT_SERVER_URL ?? "ws://localhost:1234";
const ENV_SUPABASE_URL = process.env.NEXT_PUBLIC_SUPABASE_URL ?? "";
const ENV_SUPABASE_ANON_KEY = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY ?? "";

const ENV_CONFIG: ConnectionConfig | null =
  ENV_SUPABASE_URL && ENV_SUPABASE_ANON_KEY
    ? {
        serverUrl: ENV_SERVER_URL,
        devName: "",
        accessToken: "",
        mode: "oauth",
        supabaseUrl: ENV_SUPABASE_URL,
        supabaseAnonKey: ENV_SUPABASE_ANON_KEY,
      }
    : null;

export function ChatApp() {
  const [config, setConfig] = useState<ConnectionConfig | null>(ENV_CONFIG);

  if (!config) return <ConnectionScreen onConnect={setConfig} />;

  if (config.mode === "oauth" && config.supabaseUrl && config.supabaseAnonKey) {
    return <OAuthWorkspace config={config} onDisconnect={() => setConfig(ENV_CONFIG)} />;
  }

  return (
    <ChatProvider serverUrl={config.serverUrl} devName={config.devName} accessToken={config.accessToken}>
      <CallManager>
        <Workspace config={config} onDisconnect={() => setConfig(null)} />
      </CallManager>
    </ChatProvider>
  );
}

function ConnectionScreen({ onConnect }: { onConnect: (config: ConnectionConfig) => void }) {
  const [serverUrl, setServerUrl] = useState(ENV_SERVER_URL);
  const [devName, setDevName] = useState("");
  const [supabaseUrl, setSupabaseUrl] = useState(ENV_SUPABASE_URL);
  const [supabaseAnonKey, setSupabaseAnonKey] = useState(ENV_SUPABASE_ANON_KEY);
  const [mode, setMode] = useState<"dev" | "oauth">(ENV_CONFIG ? "oauth" : "dev");

  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const config: ConnectionConfig = {
      serverUrl: serverUrl.trim(),
      devName: mode === "dev" ? devName.trim() : "",
      accessToken: "",
      mode,
      supabaseUrl: mode === "oauth" ? supabaseUrl.trim() : undefined,
      supabaseAnonKey: mode === "oauth" ? supabaseAnonKey.trim() : undefined,
    };
    if (
      !config.serverUrl ||
      (mode === "dev" && !config.devName) ||
      (mode === "oauth" && (!config.supabaseUrl || !config.supabaseAnonKey))
    )
      return;
    onConnect(config);
  };

  return (
    <main className="welcome-page setup-screen">
      <div className="welcome-layout">
        <WelcomeIntro />
        <form onSubmit={submit} className="setup-card login-card">
          <div className="mb-8">
            <h2>Log in</h2>
            <p>Choose how you want to join the conversation.</p>
          </div>

          <label className="mb-1.5 block text-xs font-semibold uppercase tracking-wider text-[#6f7890]">
            Server WebSocket URL
          </label>
          <input
            value={serverUrl}
            onChange={(e) => setServerUrl(e.target.value)}
            className="field mb-5"
            placeholder="ws://localhost:1234"
          />

          <div className="mb-5 grid grid-cols-2 gap-1 rounded-lg border border-[#e1e4ef] bg-[#f6f7fc] p-1">
            <button
              type="button"
              onClick={() => setMode("dev")}
              className={`rounded-md px-3 py-2 text-sm font-semibold transition ${
                mode === "dev"
                  ? "bg-[#5d6ad8] text-white"
                  : "text-[#6f7890] hover:text-[#1d2740]"
              }`}
            >
              Local demo
            </button>
            <button
              type="button"
              onClick={() => setMode("oauth")}
              className={`rounded-md px-3 py-2 text-sm font-semibold transition ${
                mode === "oauth"
                  ? "bg-[#5d6ad8] text-white"
                  : "text-[#6f7890] hover:text-[#1d2740]"
              }`}
            >
              OAuth
            </button>
          </div>

          {mode === "dev" ? (
            <>
              <label className="mb-1.5 block text-xs font-semibold uppercase tracking-wider text-[#6f7890]">
                Display name
              </label>
              <input
                value={devName}
                onChange={(e) => setDevName(e.target.value)}
                className="field"
                placeholder="e.g. Ada"
                maxLength={48}
                autoFocus
              />
              <p className="mt-2 text-xs text-[#8c94a8]">Local development login.</p>
            </>
          ) : (
            <>
              <label className="mb-1.5 block text-xs font-semibold uppercase tracking-wider text-[#6f7890]">
                Supabase project URL
              </label>
              <input
                value={supabaseUrl}
                onChange={(e) => setSupabaseUrl(e.target.value)}
                className="field mb-4"
                placeholder="https://your-project.supabase.co"
              />
              <label className="mb-1.5 block text-xs font-semibold uppercase tracking-wider text-[#6f7890]">
                Supabase anon key
              </label>
              <textarea
                value={supabaseAnonKey}
                onChange={(e) => setSupabaseAnonKey(e.target.value)}
                className="field min-h-24 resize-y"
                placeholder="eyJ..."
              />
              <p className="mt-2 text-xs text-[#8c94a8]">Use your project&apos;s public client key.</p>
            </>
          )}

          <button
            className="mt-8 flex w-full items-center justify-center gap-2 rounded-lg bg-[#5d6ad8] px-5 py-3.5 text-sm font-semibold text-white transition hover:bg-[#5058c5] active:scale-[0.99]"
            type="submit"
          >
            Launch workspace
            <span aria-hidden="true">&rarr;</span>
          </button>
        </form>
      </div>
    </main>
  );
}

function OAuthWorkspace({ config, onDisconnect }: { config: ConnectionConfig; onDisconnect: () => void }) {
  const [supabase] = useState(() => createClient(config.supabaseUrl!, config.supabaseAnonKey!));
  const [session, setSession] = useState<Session | null>(null);
  const [loading, setLoading] = useState(true);
  const [authError, setAuthError] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    void supabase.auth.getSession().then(({ data, error }) => {
      if (!active) return;
      if (error) setAuthError(error.message);
      setSession(data.session);
      setLoading(false);
    });
    const { data: subscription } = supabase.auth.onAuthStateChange((_event, nextSession) => {
      if (!active) return;
      setSession(nextSession);
      setLoading(false);
    });
    return () => {
      active = false;
      subscription.subscription.unsubscribe();
    };
  }, [supabase]);

  if (loading) {
    return (
      <FullScreenMessage
        title="Restoring Supabase session..."
        detail="The Client Component is asking supabase-js for its browser session."
      />
    );
  }

  if (!session) {
    return (
      <OAuthGate
        error={authError}
        onSignIn={async (provider) => {
          const { error } = await supabase.auth.signInWithOAuth({
            provider,
            options: { redirectTo: window.location.origin },
          });
          if (error) setAuthError(error.message);
        }}
      />
    );
  }

  const nickname = session.user.user_metadata?.nickname;
  if (typeof nickname !== "string" || !nickname.trim()) {
    return (
      <NicknameGate
        onSave={async (nextNickname) => {
          const { error } = await supabase.auth.updateUser({ data: { nickname: nextNickname } });
          if (error) return error.message;
          const refreshed = (await supabase.auth.getSession()).data.session;
          if (refreshed) setSession(refreshed);
          return null;
        }}
        onBack={onDisconnect}
      />
    );
  }

  return (
    <ChatProvider serverUrl={config.serverUrl} accessToken={session.access_token}>
      <CallManager>
        <Workspace
          config={config}
          onDisconnect={async () => {
            await supabase.auth.signOut();
            onDisconnect();
          }}
        />
      </CallManager>
    </ChatProvider>
  );
}

function NicknameGate({
  onSave,
  onBack,
}: {
  onSave: (nickname: string) => Promise<string | null>;
  onBack: () => void;
}) {
  const [nickname, setNickname] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const submit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const clean = nickname.trim();
    if (!/^[A-Za-z0-9 _-]{1,48}$/.test(clean)) {
      setError("Use 1-48 letters, numbers, spaces, - or _.");
      return;
    }
    setSaving(true);
    setError(await onSave(clean));
    setSaving(false);
  };

  return (
    <main className="setup-screen grid min-h-screen place-items-center px-6">
      <form onSubmit={submit} className="setup-card w-full max-w-md">
        <p className="eyebrow">identity setup</p>
        <h1 className="mt-3 text-2xl font-bold">Choose your nickname</h1>
        <p className="mt-3 text-sm leading-6 text-[#6f7890]">
          This name is stored with your Supabase profile and used by the chat server for messages and presence.
        </p>
        <label className="mt-6 block text-xs font-semibold uppercase tracking-wider text-[#6f7890]">
          Nickname
          <input
            autoFocus
            value={nickname}
            onChange={(e) => setNickname(e.target.value)}
            className="field mt-2"
            maxLength={48}
            placeholder="e.g. Ada Lovelace"
          />
        </label>
        {error && <p className="mt-3 text-xs text-red-600">{error}</p>}
        <button disabled={saving} className="primary-button mt-6 w-full" type="submit">
          {saving ? "Saving profile..." : "Save nickname"}
        </button>
        <button
          type="button"
          onClick={onBack}
          className="mt-4 w-full text-xs text-[#8c94a8] hover:text-[#1d2740]"
        >
          Back to setup
        </button>
      </form>
    </main>
  );
}

function OAuthGate({
  error,
  onSignIn,
}: {
  error: string | null;
  onSignIn: (provider: "github" | "google") => Promise<void>;
}) {
  const [busy, setBusy] = useState(false);

  const signIn = async (provider: "github" | "google") => {
    setBusy(true);
    await onSignIn(provider);
    setBusy(false);
  };

  return (
    <main className="welcome-page setup-screen">
      <div className="welcome-layout">
        <WelcomeIntro />
        <section className="setup-card login-card">
          <h2>Log in</h2>
          <p>Choose how you want to join the conversation.</p>
          {error && (
            <p className="mt-5 rounded-lg border border-red-200 bg-red-50 p-3 text-xs text-red-700">
              {error}
            </p>
          )}
          <div className="mt-6 grid gap-3">
            <button
              disabled={busy}
              onClick={() => void signIn("github")}
              className="rounded-lg bg-[#1d2740] px-4 py-3 text-sm font-semibold text-white transition hover:bg-[#2a3654] disabled:opacity-50"
            >
              Continue with GitHub
            </button>
            <button
              disabled={busy}
              onClick={() => void signIn("google")}
              className="rounded-lg border border-[#e1e4ef] px-4 py-3 text-sm font-semibold text-[#1d2740] transition hover:bg-[#f6f7fc] disabled:opacity-50"
            >
              Continue with Google
            </button>
          </div>
        </section>
      </div>
    </main>
  );
}

function FullScreenMessage({ title, detail }: { title: string; detail: string }) {
  return (
    <main className="setup-screen grid min-h-screen place-items-center px-6 text-center">
      <div>
        <div className="mx-auto mb-5 h-2.5 w-2.5 animate-ping rounded-full bg-[#5d6ad8]" />
        <h1 className="text-xl font-bold">{title}</h1>
        <p className="mt-3 text-sm text-[#6f7890]">{detail}</p>
      </div>
    </main>
  );
}

function WelcomeIntro() {
  return (
    <section className="welcome-intro">
      <div className="welcome-mark">
        <span />
        Chaos Chat
      </div>
      <h1>
        Welcome to
        <br />
        <strong>Chaos Chat</strong>
      </h1>
      <p>Real-time conversations, playful challenges, and a little controlled disorder.</p>
      <div className="welcome-rule" />
    </section>
  );
}

function PracticePong() {
  const [open, setOpen] = useState(false);
  const [score, setScore] = useState({ player: 0, cpu: 0, label: "steady" });
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const gameRef = useRef({
    paddleY: 103,
    cpuY: 103,
    ballX: 200,
    ballY: 130,
    ballVX: 190,
    ballVY: 115,
    nextBurstAt: 0,
    lastFrame: 0,
    player: 0,
    cpu: 0,
    done: false,
  });

  useEffect(() => {
    if (!open) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const game = gameRef.current;
    game.paddleY = 103;
    game.cpuY = 103;
    game.ballX = 200;
    game.ballY = 130;
    game.ballVX = 190;
    game.ballVY = 115;
    game.player = 0;
    game.cpu = 0;
    game.done = false;
    game.nextBurstAt = performance.now() + 1100;
    setScore({ player: 0, cpu: 0, label: "steady" });

    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    let frame = 0;

    const draw = () => {
      ctx.fillStyle = "#17172b";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.setLineDash([7, 8]);
      ctx.strokeStyle = "#59678f";
      ctx.beginPath();
      ctx.moveTo(canvas.width / 2, 0);
      ctx.lineTo(canvas.width / 2, canvas.height);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = "#a9b8e8";
      ctx.fillRect(14, game.paddleY, 9, 54);
      ctx.fillStyle = "#d49fc5";
      ctx.fillRect(canvas.width - 23, game.cpuY, 9, 54);
      ctx.fillStyle = "rgba(170,185,245,.22)";
      ctx.beginPath();
      ctx.arc(game.ballX, game.ballY, 14, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = "#f0effc";
      ctx.beginPath();
      ctx.arc(game.ballX, game.ballY, 6, 0, Math.PI * 2);
      ctx.fill();
    };

    const resetBall = (direction: number) => {
      game.ballX = canvas.width / 2;
      game.ballY = canvas.height / 2;
      game.ballVX = direction * 190;
      game.ballVY = (Math.random() * 150 - 75) || 70;
      game.nextBurstAt = performance.now() + 850 + Math.random() * 1800;
    };

    const tick = (now: number) => {
      const dt = Math.min((now - game.lastFrame) / 1000, 0.035);
      game.lastFrame = now;
      if (!game.done) {
        game.cpuY += Math.max(-130 * dt, Math.min(130 * dt, game.ballY - 27 - game.cpuY));
        game.cpuY = Math.max(0, Math.min(canvas.height - 54, game.cpuY));
        if (now >= game.nextBurstAt) {
          const multiplier = 0.86 + Math.random() * 0.7;
          game.ballVX = Math.max(-620, Math.min(620, game.ballVX * multiplier));
          game.ballVY = Math.max(-510, Math.min(510, game.ballVY * multiplier));
          game.nextBurstAt = now + 850 + Math.random() * 1800;
          setScore({
            player: game.player,
            cpu: game.cpu,
            label: `surge x${multiplier.toFixed(2)}`,
          });
        }
        game.ballX += game.ballVX * dt;
        game.ballY += game.ballVY * dt;
        if (game.ballY < 6 || game.ballY > canvas.height - 6) {
          game.ballY = Math.max(6, Math.min(canvas.height - 6, game.ballY));
          game.ballVY *= -1;
        }
        const hitPlayer =
          game.ballVX < 0 && game.ballX - 6 <= 23 && game.ballX > 14 &&
          game.ballY >= game.paddleY && game.ballY <= game.paddleY + 54;
        const hitCpu =
          game.ballVX > 0 && game.ballX + 6 >= canvas.width - 23 && game.ballX < canvas.width - 14 &&
          game.ballY >= game.cpuY && game.ballY <= game.cpuY + 54;
        if (hitPlayer || hitCpu) {
          const paddleY = hitPlayer ? game.paddleY : game.cpuY;
          const speedup = 1.04 + Math.random() * 0.09;
          game.ballVX = Math.max(-530, Math.min(530, -game.ballVX * speedup));
          game.ballVY = Math.max(
            -430,
            Math.min(430, (game.ballVY + ((game.ballY - (paddleY + 27)) / 27) * 105) * speedup),
          );
          setScore({
            player: game.player,
            cpu: game.cpu,
            label: `${Math.round(Math.hypot(game.ballVX, game.ballVY))} px/s`,
          });
        }
        if (game.ballX < -10 || game.ballX > canvas.width + 10) {
          if (game.ballX < 0) game.cpu++;
          else game.player++;
          if (game.player >= 7 || game.cpu >= 7) {
            game.done = true;
            setScore({
              player: game.player,
              cpu: game.cpu,
              label: game.player >= 7 ? "you win" : "CPU wins",
            });
          } else {
            setScore({ player: game.player, cpu: game.cpu, label: "point" });
            resetBall(game.ballX < 0 ? -1 : 1);
          }
        }
      }
      draw();
      if (!game.done) frame = requestAnimationFrame(tick);
    };

    game.lastFrame = performance.now();
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [open]);

  const movePaddle = (clientY: number) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const bounds = canvas.getBoundingClientRect();
    gameRef.current.paddleY = Math.max(
      0,
      Math.min(canvas.height - 54, (clientY - bounds.top) * canvas.height / bounds.height - 27),
    );
  };

  const restart = () => {
    setOpen(false);
    window.setTimeout(() => setOpen(true), 0);
  };

  return (
    <>
      <button
        onClick={() => setOpen((v) => !v)}
        className="practice-button"
      >
        {open ? "Close Pong" : "Practice Pong"}
      </button>
      {open && (
        <section className="practice-pong">
          <div className="flex items-center justify-between">
            <div>
              <p className="font-mono text-[10px] uppercase tracking-widest text-[#775fc0]">
                offline mini-game
              </p>
              <h2 className="mt-1 text-sm font-bold">Solo Pong</h2>
            </div>
            <button
              onClick={() => setOpen(false)}
              className="text-xs text-[#8c94a8] hover:text-[#1d2740]"
            >
              close
            </button>
          </div>
          <p className="mt-3 font-mono text-xs text-[#6f7890]">
            You {score.player} - {score.cpu} CPU{" "}
            <span className="text-[#775fc0]">· {score.label}</span>
          </p>
          <canvas
            ref={canvasRef}
            width={400}
            height={260}
            onPointerMove={(e) => movePaddle(e.clientY)}
            onClick={() => gameRef.current.done && restart()}
            className="pong-canvas mt-3"
            aria-label="Solo Pong game"
          />
          <p className="mt-3 text-[10px] leading-4 text-[#8c94a8]">
            Move your pointer over the court. The ball randomly surges. Click after a first-to-seven game to replay.
          </p>
        </section>
      )}
    </>
  );
}

interface CallActions {
  callUser: (id: string, name: string) => void;
}

const CALL_STATUSES = ["idle", "calling", "connecting", "connected"] as const;
type CallStatus = (typeof CALL_STATUSES)[number];

const CallContext = createContext<CallActions | null>(null);

function CallManager({ children }: { children: ReactNode }) {
  const { state, sendSignal } = useChat();
  const [incoming, setIncoming] = useState<{ id: string; name: string } | null>(null);
  const [active, setActive] = useState<{ id: string; name: string } | null>(null);
  const [callStatus, setCallStatus] = useState<CallStatus>("idle");
  const [error, setError] = useState<string | null>(null);
  const peerRef = useRef<RTCPeerConnection | null>(null);
  const localStreamRef = useRef<MediaStream | null>(null);
  const remoteAudioRef = useRef<HTMLAudioElement>(null);
  const activeRef = useRef<{ id: string; name: string } | null>(null);

  const disposePeer = () => {
    peerRef.current?.close();
    peerRef.current = null;
    localStreamRef.current?.getTracks().forEach((track) => track.stop());
    localStreamRef.current = null;
    activeRef.current = null;
  };

  const cleanup = () => {
    disposePeer();
    setActive(null);
    setCallStatus("idle");
  };

  const setupPeer = async (peerId: string, peerName: string) => {
    if (peerRef.current) cleanup();
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    const peer = new RTCPeerConnection();
    stream.getTracks().forEach((track) => peer.addTrack(track, stream));
    peer.onicecandidate = (event) => {
      if (event.candidate)
        sendSignal(MSG_SIGNAL, peerId, { kind: "ice", candidate: event.candidate.toJSON() });
    };
    peer.ontrack = (event) => {
      if (remoteAudioRef.current) remoteAudioRef.current.srcObject = event.streams[0];
    };
    peer.onconnectionstatechange = () => {
      if (peer.connectionState === "connected") setCallStatus("connected");
      if (peer.connectionState === "failed" || peer.connectionState === "closed") cleanup();
    };
    peerRef.current = peer;
    localStreamRef.current = stream;
    activeRef.current = { id: peerId, name: peerName };
    setActive({ id: peerId, name: peerName });
    setCallStatus("connecting");
    return peer;
  };

  const callUser = async (peerId: string, peerName: string) => {
    if (peerRef.current || peerId === state.users.find((u) => u.name === state.myUsername)?.id) return;
    setError(null);
    try {
      const peer = await setupPeer(peerId, peerName);
      const offer = await peer.createOffer();
      await peer.setLocalDescription(offer);
      sendSignal(MSG_SIGNAL, peerId, { kind: "offer", sdp: offer.sdp });
      setCallStatus("calling");
    } catch (callError) {
      cleanup();
      setError(callError instanceof Error ? callError.message : "Microphone permission was unavailable.");
    }
  };

  useEffect(() => {
    const signal = state.signalEvent;
    if (!signal || !signal.from) return;
    if (signal.kind === "offer")
      startTransition(() =>
        setIncoming({
          id: signal.from!,
          name: state.users.find((u) => u.id === signal.from)?.name ?? "Unknown peer",
        }),
      );
    if (signal.kind === "answer" && peerRef.current && signal.sdp)
      void peerRef.current.setRemoteDescription({ type: "answer", sdp: signal.sdp });
    if (signal.kind === "ice" && peerRef.current && signal.candidate)
      void peerRef.current.addIceCandidate(signal.candidate);
    if (signal.kind === "error")
      startTransition(() => setError(signal.msg ?? "Call signaling failed."));
  }, [state.signalEvent, state.users]);

  useEffect(() => {
    if (state.callRejected > 0) {
      disposePeer();
      startTransition(() => {
        setActive(null);
        setCallStatus("idle");
        setIncoming(null);
      });
    }
  }, [state.callRejected]);

  const accept = async () => {
    if (!incoming) return;
    const caller = incoming;
    setIncoming(null);
    try {
      const peer = await setupPeer(caller.id, caller.name);
      const signal = state.signalEvent;
      if (signal?.kind === "offer" && signal.sdp) {
        await peer.setRemoteDescription({ type: "offer", sdp: signal.sdp });
        const answer = await peer.createAnswer();
        await peer.setLocalDescription(answer);
        sendSignal(MSG_SIGNAL, caller.id, { kind: "answer", sdp: answer.sdp });
      }
    } catch (callError) {
      cleanup();
      setError(callError instanceof Error ? callError.message : "Microphone permission was unavailable.");
    }
  };

  const reject = () => {
    if (incoming) sendSignal(MSG_CALL_REJECT, incoming.id, { kind: "reject" });
    setIncoming(null);
  };

  const hangUp = () => {
    if (activeRef.current) sendSignal(MSG_CALL_REJECT, activeRef.current.id, { kind: "reject" });
    cleanup();
  };

  return (
    <CallContext.Provider value={{ callUser }}>
      {children}
      <audio ref={remoteAudioRef} autoPlay />
      {incoming && (
        <div className="call-banner">
          <span>
            <strong>{incoming.name}</strong> is calling
          </span>
          <button onClick={() => void accept()} className="call-accept">
            Accept
          </button>
          <button onClick={reject} className="call-decline">
            Decline
          </button>
        </div>
      )}
      {active && (
        <div className="active-call">
          <span>
            {callStatus === "connected" ? "Audio connected" : "Calling"}{" "}
            <strong>{active.name}</strong>
          </span>
          <button onClick={hangUp}>Hang up</button>
        </div>
      )}
      {error && (
        <button onClick={() => setError(null)} className="call-error">
          {error} (dismiss)
        </button>
      )}
    </CallContext.Provider>
  );
}

function useCall() {
  const context = useContext(CallContext);
  if (!context) throw new Error("useCall must be used inside CallManager");
  return context;
}

function Workspace({ config, onDisconnect }: { config: ConnectionConfig; onDisconnect: () => void }) {
  const { state, sendChat, loadHistory, joinRoom, requestChallenge, respondToChallenge, requestRoomList } =
    useChat();
  const { callUser } = useCall();
  const [draft, setDraft] = useState("");
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const visibleRooms = ROOM_ORDER.map((slug) => ({
    slug,
    ownerId: state.rooms.find((r) => r.slug === slug)?.ownerId ?? "",
    chaosEnabled: CHAOTIC_ROOMS.has(slug),
  }));

  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [state.messages.length]);

  const submitMessage = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (sendChat(draft)) setDraft("");
  };

  return (
    <main className="light-workspace min-h-screen">
      <header className="flex min-h-16 items-center justify-between border-b border-[#e1e4ef] px-5 py-3 lg:px-8">
        <div className="flex items-center gap-3">
          <div className="grid h-9 w-9 place-items-center rounded-lg bg-[#5d6ad8] text-sm font-bold text-white">
            CC
          </div>
          <div>
            <p className="text-sm font-bold tracking-[0.15em] text-[#1d2740]">CHAOS CHAT</p>
            <p className="font-mono text-[10px] uppercase tracking-widest text-[#8c94a8]">live rooms</p>
          </div>
        </div>
        <div className="flex items-center gap-3">
          <PracticePong />
          <StatusPill status={state.status} />
          <span className="hidden text-sm text-[#6f7890] sm:inline">
            {state.myUsername || config.devName}
          </span>
          <button
            onClick={onDisconnect}
            className="rounded-lg border border-[#e1e4ef] px-3 py-1.5 text-xs font-semibold text-[#6f7890] transition hover:border-red-200 hover:text-red-500"
          >
            Exit
          </button>
        </div>
      </header>

      {state.error && (
        <div className="connection-error" role="alert">
          {state.error}
        </div>
      )}

      <div className="mx-auto grid max-w-[1600px] gap-4 p-4 lg:grid-cols-[220px_minmax(0,1fr)_280px] lg:p-6">
        <aside className="panel order-2 lg:order-1">
          <PanelHeading eyebrow="rooms" title="The map" />
          <div className="space-y-1">
            {visibleRooms.map((room) => {
              const preset = ROOM_PRESETS[room.slug];
              return (
                <button
                  key={room.slug}
                  onClick={() => joinRoom(room.slug)}
                  className={`room-button ${room.slug === state.currentRoom ? "room-active" : ""}`}
                >
                  <span className={`room-dot ${room.chaosEnabled ? "room-dot-chaos" : ""}`} />
                  <span className="room-copy">
                    <span className="room-name">{preset.name}</span>
                    <span className="room-purpose">{preset.purpose}</span>
                  </span>
                  {room.chaosEnabled && <span className="room-owner">wild</span>}
                </button>
              );
            })}
          </div>
        </aside>

        <section className="panel order-1 flex min-h-[650px] flex-col lg:order-2">
          <div className="flex items-start justify-between border-b border-[#e1e4ef] pb-4">
            <div>
              <p className="font-mono text-[10px] uppercase tracking-[0.2em] text-[#5d6ad8]">
                live channel
              </p>
              <h1 className="mt-1.5 text-xl font-bold text-[#1d2740]">#{state.currentRoom}</h1>
              <p className="mt-0.5 text-xs text-[#8c94a8]">
                {state.messages.filter((m) => m.id > 0).length} persisted messages
              </p>
            </div>
            <button
              onClick={loadHistory}
              disabled={!state.authenticated}
              className="rounded-lg border border-[#e1e4ef] px-3 py-1.5 text-xs font-semibold text-[#6f7890] transition hover:border-[#5d6ad8] hover:text-[#5d6ad8] disabled:cursor-not-allowed disabled:opacity-40"
            >
              Load older
            </button>
          </div>

          <div className="flex-1 space-y-2.5 overflow-y-auto py-5 pr-1" aria-live="polite">
            {state.messages.length === 0 ? (
              <EmptyState status={state.status} />
            ) : (
              state.messages.map((message) => (
                <MessageBox
                  key={`${message.id}-${message.clientId ?? "server"}`}
                  message={message}
                  own={message.username === state.myUsername}
                />
              ))
            )}
            <div ref={messagesEndRef} />
          </div>

          <form onSubmit={submitMessage} className="border-t border-[#e1e4ef] pt-4">
            <div className="flex gap-2 rounded-lg border border-[#e1e4ef] bg-[#f6f7fc] p-1.5 focus-within:border-[#5d6ad8]">
              <input
                value={draft}
                onChange={(e) => setDraft(e.target.value)}
                disabled={!state.authenticated}
                className="min-w-0 flex-1 bg-transparent px-3 text-sm text-[#1d2740] outline-none placeholder:text-[#8c94a8] disabled:cursor-not-allowed"
                placeholder={state.authenticated ? "Send a message..." : "Waiting for authentication..."}
              />
              <button
                disabled={!state.authenticated || !draft.trim()}
                className="rounded-md bg-[#5d6ad8] px-4 py-2 text-sm font-semibold text-white transition hover:bg-[#5058c5] disabled:cursor-not-allowed disabled:opacity-30"
                type="submit"
              >
                Send
              </button>
            </div>
          </form>
        </section>

        <aside className="order-3 space-y-4">
          <section className="panel">
            <PanelHeading eyebrow="presence" title="Active panel" />
            <div className="space-y-1.5">
              {state.users.map((user) => (
                <div key={user.id} className="user-row">
                  <span className="status-dot" />
                  <span className="truncate text-[#3f4d68]">{user.name}</span>
                  {user.name === state.myUsername ? (
                    <span className="ml-auto font-mono text-[9px] uppercase text-[#5d6ad8]">you</span>
                  ) : (
                    <span className="ml-auto flex gap-1">
                      <button
                        onClick={() => callUser(user.id, user.name)}
                        className="user-action"
                        title={`Call ${user.name}`}
                      >
                        call
                      </button>
                      <button
                        onClick={() => requestChallenge(user.id)}
                        className="user-action"
                        title={`Challenge ${user.name} to Pong`}
                      >
                        vs
                      </button>
                    </span>
                  )}
                </div>
              ))}
              {state.users.length === 0 && (
                <p className="text-xs text-[#8c94a8]">No presence frames yet.</p>
              )}
            </div>
          </section>

          <section className="panel">
            <PanelHeading eyebrow="versus" title="Challenges" />
            {state.challenges.length === 0 ? (
              <p className="text-xs leading-5 text-[#8c94a8]">
                Challenge a player with <span className="font-semibold text-[#5d6ad8]">vs</span> to
                start a first-to-seven Pong match.
              </p>
            ) : (
              <div className="space-y-2">
                {state.challenges.map((challenge) => {
                  const isIncoming = challenge.action === "incoming";
                  return (
                    <div className="challenge-card" key={challenge.id}>
                      <p>
                        {isIncoming
                          ? `${challenge.requesterName} challenged you.`
                          : `Challenge sent to ${challenge.targetName}.`}
                      </p>
                      <div className="challenge-actions">
                        {isIncoming ? (
                          <>
                            <button
                              onClick={() => respondToChallenge(challenge.id, "accept")}
                            >
                              Accept
                            </button>
                            <button
                              className="decline"
                              onClick={() => respondToChallenge(challenge.id, "decline")}
                            >
                              Decline
                            </button>
                          </>
                        ) : (
                          <button
                            className="decline"
                            onClick={() => respondToChallenge(challenge.id, "cancel")}
                          >
                            Cancel
                          </button>
                        )}
                      </div>
                    </div>
                  );
                })}
              </div>
            )}
          </section>

          <section className="panel">
            <PanelHeading eyebrow="history" title="Room matches" />
            <button
              onClick={requestRoomList}
              className="mb-3 text-[10px] font-semibold uppercase tracking-widest text-[#5d6ad8] hover:text-[#5058c5]"
            >
              Refresh rooms
            </button>
            {state.matches.length === 0 ? (
              <p className="text-xs text-[#8c94a8]">No recorded room matches yet.</p>
            ) : (
              <div className="space-y-2">
                {state.matches.slice(0, 4).map((match) => (
                  <div className="match-card" key={match.gameId}>
                    <strong>
                      {match.playerOne} {match.playerOneScore} - {match.playerTwoScore}{" "}
                      {match.playerTwo}
                    </strong>
                    <span>{match.winner} takes the bragging rights.</span>
                  </div>
                ))}
              </div>
            )}
          </section>
        </aside>
      </div>
      <PvPong />
    </main>
  );
}

function PvPong() {
  const { state, sendGameEvent } = useChat();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const gameRef = useRef({
    leftY: 128,
    rightY: 128,
    ballX: 260,
    ballY: 160,
    ballVX: 220,
    ballVY: 105,
    leftScore: 0,
    rightScore: 0,
    nextBurstAt: 0,
    lastFrame: 0,
    done: false,
  });
  const [notice, setNotice] = useState("");
  const [score, setScore] = useState({ left: 0, right: 0 });
  const game = state.activePvp;

  useEffect(() => {
    if (!game) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const local = gameRef.current;
    local.leftY = 128;
    local.rightY = 128;
    local.ballX = 260;
    local.ballY = 160;
    local.ballVX = 220;
    local.ballVY = 105;
    local.leftScore = 0;
    local.rightScore = 0;
    local.done = false;
    local.nextBurstAt = performance.now() + 1000;
    setScore({ left: 0, right: 0 });
    setNotice(`Pong vs ${game.opponentName}. First to seven wins.`);

    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    let animation = 0;

    const draw = () => {
      ctx.fillStyle = "#17172b";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.setLineDash([8, 9]);
      ctx.strokeStyle = "#59678f";
      ctx.beginPath();
      ctx.moveTo(canvas.width / 2, 0);
      ctx.lineTo(canvas.width / 2, canvas.height);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = "#a9b8e8";
      ctx.fillRect(16, local.leftY, 10, 64);
      ctx.fillStyle = "#d49fc5";
      ctx.fillRect(canvas.width - 26, local.rightY, 10, 64);
      ctx.fillStyle = "rgba(170,185,245,.2)";
      ctx.beginPath();
      ctx.arc(local.ballX, local.ballY, 18, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = "#f0effc";
      ctx.beginPath();
      ctx.arc(local.ballX, local.ballY, 6, 0, Math.PI * 2);
      ctx.fill();
    };

    const resetBall = (direction: number) => {
      local.ballX = canvas.width / 2;
      local.ballY = canvas.height / 2;
      local.ballVX = direction * 220;
      local.ballVY = (Math.random() * 150 - 75) || 72;
      local.nextBurstAt = performance.now() + 900 + Math.random() * 1800;
    };

    const tick = (now: number) => {
      if (game.role !== "host" || local.done) {
        draw();
        return;
      }
      const dt = Math.min((now - local.lastFrame) / 1000, 0.035);
      local.lastFrame = now;
      local.ballX += local.ballVX * dt;
      local.ballY += local.ballVY * dt;
      if (now >= local.nextBurstAt) {
        const multiplier = 0.86 + Math.random() * 0.7;
        local.ballVX = Math.max(-680, Math.min(680, local.ballVX * multiplier));
        local.ballVY = Math.max(-540, Math.min(540, local.ballVY * multiplier));
        local.nextBurstAt = now + 900 + Math.random() * 1800;
        setNotice(`Ball surge x${multiplier.toFixed(2)} - stay awake.`);
      }
      if (local.ballY < 6 || local.ballY > canvas.height - 6) {
        local.ballY = Math.max(6, Math.min(canvas.height - 6, local.ballY));
        local.ballVY *= -1;
      }
      const hitLeft =
        local.ballVX < 0 && local.ballX - 6 <= 26 && local.ballX > 16 &&
        local.ballY >= local.leftY && local.ballY <= local.leftY + 64;
      const hitRight =
        local.ballVX > 0 && local.ballX + 6 >= canvas.width - 26 && local.ballX < canvas.width - 16 &&
        local.ballY >= local.rightY && local.ballY <= local.rightY + 64;
      if (hitLeft || hitRight) {
        const paddle = hitLeft ? local.leftY : local.rightY;
        const speedup = 1.035 + Math.random() * 0.07;
        local.ballVX = Math.max(-560, Math.min(560, -local.ballVX * speedup));
        local.ballVY = Math.max(
          -450,
          Math.min(450, (local.ballVY + ((local.ballY - (paddle + 32)) / 32) * 110) * speedup),
        );
      }
      if (local.ballX < -10 || local.ballX > canvas.width + 10) {
        if (local.ballX < 0) local.rightScore++;
        else local.leftScore++;
        setScore({ left: local.leftScore, right: local.rightScore });
        if (local.leftScore >= 7 || local.rightScore >= 7) {
          local.done = true;
          setNotice(`${local.leftScore >= 7 ? "Host" : game.opponentName} wins. Recording result...`);
          sendGameEvent({ kind: "finish", hostScore: local.leftScore, guestScore: local.rightScore });
        } else {
          resetBall(local.ballX < 0 ? -1 : 1);
        }
      }
      draw();
      if (!local.done) {
        sendGameEvent({
          kind: "state",
          leftY: local.leftY,
          rightY: local.rightY,
          ballX: local.ballX,
          ballY: local.ballY,
          ballVX: local.ballVX,
          ballVY: local.ballVY,
          leftScore: local.leftScore,
          rightScore: local.rightScore,
          complete: local.done,
        });
        animation = requestAnimationFrame(tick);
      }
    };

    local.lastFrame = performance.now();
    animation = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(animation);
  }, [game, sendGameEvent]);

  useEffect(() => {
    const event = state.lastGameEvent;
    if (!game || game.role !== "guest" || !event || event.gameId !== game.gameId || event.kind !== "state")
      return;
    const local = gameRef.current;
    for (const key of [
      "leftY", "rightY", "ballX", "ballY", "ballVX", "ballVY", "leftScore", "rightScore",
    ] as const) {
      if (typeof event[key] === "number") local[key] = event[key] as never;
    }
    local.done = event.complete === true;
    setScore({ left: local.leftScore, right: local.rightScore });
    setNotice(`${local.leftScore} - ${local.rightScore} · ${Math.round(Math.hypot(local.ballVX, local.ballVY))} px/s`);
    const canvas = canvasRef.current;
    const ctx = canvas?.getContext("2d");
    if (canvas && ctx) {
      ctx.fillStyle = "#17172b";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = "#a9b8e8";
      ctx.fillRect(16, local.leftY, 10, 64);
      ctx.fillStyle = "#d49fc5";
      ctx.fillRect(canvas.width - 26, local.rightY, 10, 64);
      ctx.fillStyle = "#f0effc";
      ctx.beginPath();
      ctx.arc(local.ballX, local.ballY, 6, 0, Math.PI * 2);
      ctx.fill();
    }
  }, [game, state.lastGameEvent]);

  if (!game) return null;

  const move = (clientY: number) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const bounds = canvas.getBoundingClientRect();
    const y = Math.max(
      0,
      Math.min(canvas.height - 64, (clientY - bounds.top) * canvas.height / bounds.height - 32),
    );
    const local = gameRef.current;
    if (game.role === "host") {
      local.leftY = y;
    } else {
      local.rightY = y;
      sendGameEvent({ kind: "input", paddleY: y });
    }
  };

  return (
    <section className="pvp-pong">
      <div className="flex items-center justify-between">
        <div>
          <p className="font-mono text-[10px] uppercase tracking-widest text-[#775fc0]">live match</p>
          <h2 className="mt-1 text-sm font-bold">Challenge Pong</h2>
        </div>
        <button
          onClick={() => sendGameEvent({ kind: "quit" })}
          className="text-xs text-[#8c94a8] hover:text-white"
        >
          forfeit
        </button>
      </div>
      <div className="pvp-score">
        <span>{game.role === "host" ? state.myUsername : game.opponentName}</span>
        <strong>{score.left} - {score.right}</strong>
        <span>{game.role === "host" ? game.opponentName : state.myUsername}</span>
      </div>
      <p className="mb-3 text-xs text-[#775fc0]">{notice}</p>
      <canvas
        ref={canvasRef}
        width={520}
        height={320}
        onPointerMove={(e) => move(e.clientY)}
        className="pong-canvas"
        aria-label="Player versus player Pong game"
      />
      <p className="mt-3 text-[10px] leading-4 text-[#8c94a8]">
        Move your paddle with the pointer. The host simulates the match and relays state through GAME_EVENT frames.
      </p>
    </section>
  );
}

function PanelHeading({ eyebrow, title }: { eyebrow: string; title: string }) {
  return (
    <div className="mb-4">
      <p className="font-mono text-[10px] uppercase tracking-[0.2em] text-[#8c94a8]">{eyebrow}</p>
      <h2 className="mt-1.5 text-sm font-bold uppercase tracking-wider text-[#1d2740]">{title}</h2>
    </div>
  );
}

function StatusPill({ status }: { status: ConnectionStatus }) {
  const connected = status === "connected";
  return (
    <span
      className={`flex items-center gap-2 rounded-full border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ${
        connected
          ? "border-emerald-200 bg-emerald-50 text-emerald-700"
          : status === "auth-failed"
            ? "border-red-200 bg-red-50 text-red-600"
            : "border-amber-200 bg-amber-50 text-amber-700"
      }`}
    >
      <span
        className={`h-1.5 w-1.5 rounded-full ${
          connected ? "bg-emerald-500" : "bg-amber-500"
        }`}
      />
      {status}
    </span>
  );
}

function EmptyState({ status }: { status: ConnectionStatus }) {
  return (
    <div className="grid flex-1 place-items-center text-center">
      <div>
        <div className="mx-auto mb-4 grid h-14 w-14 place-items-center rounded-xl border border-[#e1e4ef] bg-[#f6f7fc] text-lg text-[#8c94a8]">
          {"//"}
        </div>
        <p className="font-mono text-xs uppercase tracking-widest text-[#8c94a8]">
          {status === "connected" ? "channel is quiet" : "waiting for connection"}
        </p>
        <p className="mt-2 max-w-xs text-sm leading-6 text-[#6f7890]">
          {status === "connected"
            ? "Send a message to start the conversation."
            : "Connect to the chat server to enter this room."}
        </p>
      </div>
    </div>
  );
}

const styleNames: Record<number, string> = {
  [MESSAGE_STYLE_REACTION]: "REACTS",
  [MESSAGE_STYLE_SHOUT]: "LOUD",
  [MESSAGE_STYLE_CONFESSION]: "CONFESSION",
  [MESSAGE_STYLE_QUESTION]: "QUESTION",
  [MESSAGE_STYLE_CELEBRATION]: "WIN",
  [MESSAGE_STYLE_SARCASM]: "SARCASM",
};

function isVibeBot(username: string) {
  return username === "Vibe Bot" || username === "Intervener";
}

function MessageBox({ message, own }: { message: ChatMessage; own: boolean }) {
  const system = message.username === "system";
  const vibe = isVibeBot(message.username);
  const targeted = vibe ? /^@\{([^}]{1,48})\}\s+/.exec(message.text) : null;
  const content = targeted ? message.text.slice(targeted[0].length) : message.text;
  const styleClass =
    message.style === MESSAGE_STYLE_REACTION ? "message-reaction" :
      message.style === MESSAGE_STYLE_SHOUT ? "message-shout" :
        message.style === MESSAGE_STYLE_CONFESSION ? "message-confession" :
          message.style === MESSAGE_STYLE_QUESTION ? "message-question" :
            message.style === MESSAGE_STYLE_CELEBRATION ? "message-celebration" :
              message.style === MESSAGE_STYLE_SARCASM ? "message-sarcasm" : "";
  const delivery =
    message.delivery === "sending" ? "Sending..." :
      message.delivery === "persisted" ? "Persisted - waiting for room delivery" :
        message.delivery === "failed" ? "Not saved - retry your message" :
          message.delivery === "delivered" ? "Delivered to this room" : null;

  return (
    <article
      className={`message-bubble ${system ? "message-system" : own ? "message-own" : ""} ${
        vibe ? (targeted ? "vibe-targeted" : "vibe-banner") : ""
      } ${styleClass}`}
    >
      {vibe && (
        <div className="vibe-kicker">
          <span className="vibe-orb" />
          VIBE BOT <span className="vibe-line" />
        </div>
      )}
      {message.style !== 0 && <div className="meme-stamp">{styleNames[message.style] ?? "STYLED"}</div>}
      {targeted && <span className="mention-pill">@{targeted[1]}</span>}
      <div className="message-heading">
        {message.sentiment && (
          <span
            className={`sentiment-indicator sentiment-${message.sentiment}`}
            title={`${message.sentiment} ${Math.round((message.intensity ?? 0) * 100)}%`}
          />
        )}
        <span
          className={`message-author ${
            system
              ? "text-[#8c94a8]"
              : own
                ? "text-[#5d6ad8]"
                : vibe
                  ? "text-[#775fc0]"
                  : "text-[#1d2740]"
          }`}
        >
          {system ? "SYSTEM" : message.username}
        </span>
        <span className="message-time">
          {message.id > 0
            ? new Date(message.timestamp * 1000).toLocaleTimeString([], {
                hour: "2-digit",
                minute: "2-digit",
              })
            : "local"}
        </span>
      </div>
      <p
        className={`message-content ${message.style === MESSAGE_STYLE_SHOUT ? "uppercase" : ""} ${
          system ? "system-content" : ""
        }`}
      >
        {content}
      </p>
      {message.sentiment && (
        <div className="sentiment-meter">
          {message.sentiment} - {Math.round((message.intensity ?? 0) * 100)}%
        </div>
      )}
      {delivery && (
        <div
          className={`delivery-meta ${
            message.delivery === "delivered" ? "delivered" : message.delivery === "persisted" ? "delayed" : ""
          }`}
        >
          {delivery}
        </div>
      )}
    </article>
  );
}
