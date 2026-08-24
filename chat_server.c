#define _POSIX_C_SOURCE 200809L // exposes strdup() under strict -std=c11

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <sqlite3.h>
#include <sodium.h>
#include <cjson/cJSON.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <hiredis/hiredis.h>
#include <hiredis/async.h>

#define MAX_USERNAME_LEN 256
#define MAX_JWT_LEN 8192             // generous headroom for a Supabase access token
#define MAX_MSG_LEN (64u * 1024)     // 64 KiB cap per chat message; upload goes through a separate endpoint
#define MAX_HANDSHAKE_LEN 8192       // cap on a buffered HTTP upgrade request
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define DB_PATH_DEFAULT "chat.db"
#define HISTORY_LIMIT 20             // messages replayed to a client on login
#define MAX_JWKS_KEYS 8              // Supabase rotates keys; room for old + new during rotation
#define MAX_CONNECTIONS 256          // hard cap on simultaneous TCP connections
#define MAX_OUTGOING_BUF (4u * 1024 * 1024) // 4 MiB outgoing cap; slow/dead clients dropped beyond this
#define RATE_LIMIT_MSGS 10           // max MSG_CHAT frames per RATE_LIMIT_WINDOW_SECS
#define RATE_LIMIT_WINDOW_SECS 1     // sliding window for per-user rate limiting
#define USER_ID_LEN 64               // Supabase UUID sub claim is 36 chars; extra headroom
#define ROOM_SLUG_LEN 48             // public room identifiers: lowercase letters, digits, _ and -
#define GAME_ID_LEN 48
#define JWT_CLOCK_SKEW_SECS 60       // tolerated clock delta for exp/nbf checks
#define IDLE_TIMEOUT_UNAUTH_SECS 30  // unauthenticated connection closed after 30 s of silence
#define IDLE_TIMEOUT_AUTH_SECS 600   // authenticated connection closed after 10 min of silence
#define CHAOS_PENDING_LIMIT 1024     // bounded holding queue; overflow falls back to immediate delivery

// Wire protocol, all integers big-endian (network byte order):
//   Client -> server: [type][user_len][username][msg_len][message]
//   Server -> client: [type][message_id][timestamp][style][user_len][username]
//                     [msg_len][message]
//
// Authentication is now OAuth via Supabase, not a local username/password.
// The browser handles the actual OAuth redirect dance with Supabase's JS
// SDK; by the time it talks to this server, it already holds a signed JWT
// proving who the user is. That JWT rides in the "message" field of an
// OAUTH_LOGIN frame. The "username" field is ignored for that type --
// identity comes entirely from the verified token, never from something
// the client just typed.
enum {
    MSG_JOIN             = 1,  // server -> clients: "X connected"
    MSG_LEAVE            = 2,  // client -> server (explicit) or server -> clients (notification)
    MSG_CHAT             = 3,  // client -> server (requires auth) or server -> clients
    MSG_OAUTH_LOGIN      = 4,  // client -> server: [ignored][Supabase JWT access token]
    MSG_AUTH_OK          = 6,  // server -> client: [username][""]
    MSG_AUTH_FAIL        = 7,  // server -> client: [username][reason]
    MSG_USERS_LIST       = 8,  // server -> clients: [""]['\n'-delimited list of "user_id\tdisplay_name"]
    MSG_HISTORY_REQUEST  = 9,  // client -> server: [ignored][before_id as ASCII string]
    MSG_HISTORY_RESPONSE = 10, // server -> client: [username][message] with ID/timestamp prefix
    // WebRTC signaling -- the server acts as a relay only; media is P2P.
    // For MSG_SIGNAL: [username] = target user_id, [msg] = JSON payload.
    // The server injects a "from" field (caller's verified user_id) before forwarding.
    MSG_SIGNAL           = 11, // client -> server -> client: WebRTC offer / answer / ICE candidate
    MSG_CALL_REJECT      = 12, // client -> server -> client: call declined or hung up
    MSG_STYLE_UPDATE     = 13, // server -> client: asynchronous AI style refinement for a message ID
    // The text field is a small JSON object: {"sentiment":"...","intensity":0..1}.
    MSG_SENTIMENT_UPDATE = 14, // server -> client: asynchronous sentiment refinement for a message ID
    MSG_ROOM_LIST        = 15, // server -> client: JSON list of public rooms
    MSG_ROOM_JOIN        = 16, // client -> server switch room; server -> client confirms it
    MSG_CHAOS_UPDATE     = 17, // owner-only per-room chaos profile update/confirmation
    MSG_CHAT_ACK         = 18, // server -> client durable acknowledgement for a client message ID
    MSG_CHALLENGE        = 19, // player challenge request / response events
    MSG_GAME_EVENT       = 20, // real-time game input and authoritative state relay
    MSG_GAME_RESULT      = 21, // persisted room-wide game result event
    MSG_GAME_HISTORY     = 22  // client request / server response: room's durable match ledger
};

typedef enum {
    MESSAGE_STYLE_PLAIN = 0,
    MESSAGE_STYLE_REACTION = 1,
    MESSAGE_STYLE_SHOUT = 2,
    MESSAGE_STYLE_CONFESSION = 3,
    MESSAGE_STYLE_QUESTION = 4,
    MESSAGE_STYLE_CELEBRATION = 5,
    MESSAGE_STYLE_SARCASM = 6,
} MessageStyle;

// Which transport a connection is speaking. Decided from the first byte(s)
// the client sends: 'G' (as in "GET ") means an HTTP upgrade request is
// coming, anything else is assumed to be our legacy raw framing.
typedef enum {
    MODE_DETECT = 0,   // haven't seen enough bytes yet to know
    MODE_RAW,          // legacy: incoming bytes are directly our app frames
    MODE_WS_HANDSHAKE, // buffering an HTTP upgrade request
    MODE_WS_ACTIVE,    // handshake done; incoming bytes are WebSocket frames
} ConnMode;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} DynamicBuffer;

typedef struct Conn {
    int fd;
    _Bool want_read;
    _Bool want_write;
    _Bool want_close;
    ConnMode mode;
    DynamicBuffer incoming;           // raw bytes off the wire (transport layer)
    DynamicBuffer app_incoming;       // decoded app-protocol bytes, WS mode only
    DynamicBuffer outgoing;
    char username[MAX_USERNAME_LEN + 1];
    char user_id[USER_ID_LEN + 1];    // stable Supabase UUID (sub claim)
    char room[ROOM_SLUG_LEN + 1];     // current public room; one room per connection
    _Bool has_username;
    time_t last_activity;             // timestamp of last received data; used for idle timeout
    int rate_msg_count;               // MSG_CHAT frames sent in current window
    time_t rate_window_start;         // start of the current rate-limit window
} Conn;

// Global registry of live connections, indexed by fd. This is what makes
// broadcasting possible: anything that needs to reach "every connected
// client" walks this array.
static Conn **g_fd2conn = NULL;
static size_t g_fd2conn_cap = 0;
static int g_conn_count = 0;                // current live connection count
// One exact origin or comma-separated exact origins for WebSocket upgrades.
static char g_allowed_origin[512] = "";
static char g_expected_issuer[512] = "";    // SUPABASE_ISSUER env; empty = no check
// Explicit local-test mode. When set, OAuth is disabled and a connection may
// authenticate only with a validated display name and an empty token.
static _Bool g_dev_auth_enabled = 0;

// Lightweight Prometheus-style counters exported from GET /metrics.
static uint64_t g_metric_chat_persisted = 0;
static uint64_t g_metric_chat_delivered = 0;
static uint64_t g_metric_chaos_duplicates = 0;
static uint64_t g_metric_redis_fallbacks = 0;
static uint64_t g_metric_ai_updates = 0;
static uint64_t g_metric_challenges_created = 0;
static uint64_t g_metric_games_completed = 0;

// epoll fd used by the event loop. -1 until main() initializes it.
static int g_epoll_fd = -1;

// ---------------------------------------------------------------------
// Redis Pub/Sub globals.
//
// Two separate hiredis synchronous contexts: one for PUBLISH, one for
// SUBSCRIBE.  The subscriber fd is added to epoll so the event loop
// wakes up as soon as Redis delivers a message.
//
// Redis Presence (distributed user list):
//   Hash key : REDIS_PRESENCE_KEY  ("chat:presence")
//   Fields   : user_id  ->  display_name
//   TTL      : PRESENCE_TTL_SECS (45 s), refreshed every PRESENCE_HEARTBEAT_SECS (20 s)
//              so ghost users auto-expire after a server crash.
// ---------------------------------------------------------------------
#define REDIS_PRESENCE_KEY "chat:presence"
#define PRESENCE_TTL_SECS 45
#define PRESENCE_HEARTBEAT_SECS 20
static time_t g_presence_last_heartbeat = 0;
#define REDIS_CHANNEL "chat:messages"       // Pub/Sub channel name

// Redis contexts are declared here because both the presence helpers and
// shutdown path need them before the Redis helper implementations below.
static redisContext *g_redis_pub_ctx = NULL;  // PUBLISH, HSET, HDEL, HGETALL, EXPIRE
static redisContext *g_redis_sub_ctx = NULL;  // SUBSCRIBE / reads
static int  g_redis_sub_fd = -1;              // subscriber fd registered with epoll
static char g_redis_host[256] = "127.0.0.1";  // from REDIS_URL env var
static int  g_redis_port = 6379;

// ---------------------------------------------------------------------
// Chaos delay/reorder layer.
//
// Messages are persisted before entering this queue, then published only
// when their randomized release time is reached.  The queue owns copies of
// the strings, so closing a client's connection cannot lose a held message.
// ---------------------------------------------------------------------
typedef struct PendingChat {
    int64_t msg_id;
    int64_t timestamp;
    int64_t min_release_ms;
    int64_t release_ms;
    _Bool duplicate;
    MessageStyle style;
    char room[ROOM_SLUG_LEN + 1];
    char *username;
    char *text;
    struct PendingChat *next;
} PendingChat;

static PendingChat *g_pending_chat_head = NULL;
static size_t g_pending_chat_count = 0;
static _Bool g_chaos_enabled = 0;
static int g_chaos_min_delay_ms = 500;
static int g_chaos_max_delay_ms = 5000;
static int g_chaos_reorder_window_ms = 750;
static int g_chaos_duplicate_percent = 10;
static uint32_t g_chaos_rng_state = 0x9E3779B9u;

typedef struct {
    _Bool enabled;
    _Bool auto_cycle;
    int min_delay_ms;
    int max_delay_ms;
    int reorder_window_ms;
    int duplicate_percent;
    int cycle_min_ms;
    int cycle_max_ms;
} ChaosConfig;

// Auto-pulse deadlines are process-local; room profiles remain durable.
typedef struct ChaosCycle {
    char room[ROOM_SLUG_LEN + 1];
    _Bool active;
    int64_t next_toggle_ms;
    struct ChaosCycle *next;
} ChaosCycle;

static ChaosCycle *g_chaos_cycles = NULL;

static ChaosConfig chaos_effective_config(const char *room);
static void chaos_cycle_reset(const char *room, const ChaosConfig *config);
static void chaos_cycle_tick(void);
static void broadcast_room_metadata(const char *room, uint8_t type);

typedef struct Challenge {
    char id[GAME_ID_LEN + 1];
    char room[ROOM_SLUG_LEN + 1];
    char requester_id[USER_ID_LEN + 1];
    char requester_name[MAX_USERNAME_LEN + 1];
    char target_id[USER_ID_LEN + 1];
    char target_name[MAX_USERNAME_LEN + 1];
    time_t expires_at;
    struct Challenge *next;
} Challenge;

typedef struct GameSession {
    char id[GAME_ID_LEN + 1];
    char room[ROOM_SLUG_LEN + 1];
    char host_id[USER_ID_LEN + 1];
    char host_name[MAX_USERNAME_LEN + 1];
    char guest_id[USER_ID_LEN + 1];
    char guest_name[MAX_USERNAME_LEN + 1];
    time_t expires_at;
    struct GameSession *next;
} GameSession;

static Challenge *g_challenges = NULL;
static GameSession *g_game_sessions = NULL;

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) { die("fcntl(F_GETFL)"); }
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) { die("fcntl(F_SETFL)"); }
}

static void dbuf_append(DynamicBuffer *dbuf, const void *data, size_t len) {
    if (len == 0) return;
    if (dbuf->len + len > dbuf->cap) {
        if (dbuf->cap == 0) dbuf->cap = 1024;
        while (dbuf->len + len > dbuf->cap) dbuf->cap *= 2;
        dbuf->data = (uint8_t *)realloc(dbuf->data, dbuf->cap);
        assert(dbuf->data != NULL);
    }
    memcpy(dbuf->data + dbuf->len, data, len);
    dbuf->len += len;
}

static void dbuf_consume(DynamicBuffer *dbuf, size_t n) {
    assert(n <= dbuf->len);
    memmove(dbuf->data, dbuf->data + n, dbuf->len - n);
    dbuf->len -= n;
}

static void conn_free(Conn *conn) {
    if (!conn) return;
    free(conn->incoming.data);
    free(conn->app_incoming.data);
    free(conn->outgoing.data);
    free(conn);
}

// Registers a connection in the global fd -> Conn table, growing it as needed,
// and adds the fd to the epoll interest set (EPOLLIN initially; EPOLLOUT added
// later by conn_update_epoll when the outgoing buffer becomes non-empty).
static void conn_register(Conn *conn) {
    if ((size_t)conn->fd >= g_fd2conn_cap) {
        size_t new_cap = g_fd2conn_cap ? g_fd2conn_cap : 1;
        while (new_cap <= (size_t)conn->fd) new_cap *= 2;
        g_fd2conn = (Conn **)realloc(g_fd2conn, new_cap * sizeof(Conn *));
        for (size_t j = g_fd2conn_cap; j < new_cap; ++j) g_fd2conn[j] = NULL;
        g_fd2conn_cap = new_cap;
    }
    g_fd2conn[conn->fd] = conn;
    if (g_epoll_fd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = conn->fd };
        epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);
    }
}

// Synchronise this connection's epoll registration with its current want_write
// state. Called whenever want_write is toggled so we only wake the loop when
// there really is data to flush (EPOLLOUT fires continuously while the kernel
// send buffer has room -- we must NOT leave it registered when the buffer is
// empty or epoll_wait will spin at 100 % CPU).
static void conn_update_epoll(Conn *conn) {
    if (g_epoll_fd < 0) return;
    uint32_t wanted = EPOLLIN;
    if (conn->want_write) wanted |= EPOLLOUT;
    struct epoll_event ev = { .events = wanted, .data.fd = conn->fd };
    epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

static void handle_write(Conn *conn); // Forward declaration

// ---------------------------------------------------------------------
// Transport-level send helpers.
//
// raw_send() and ws_send_frame() both do the same job -- append bytes to
// conn->outgoing and try to flush -- but ws_send_frame() first wraps the
// bytes in a WebSocket frame header, since a WS client has no way to make
// sense of unframed bytes arriving on the socket.
// ---------------------------------------------------------------------

static void raw_send(Conn *target, const uint8_t *data, size_t len) {
    dbuf_append(&target->outgoing, data, len);
    target->want_write = 1;
    target->want_read = 0;
    conn_update_epoll(target); // register EPOLLOUT so the loop flushes us
    handle_write(target);
}

// Builds a WebSocket frame around `payload` and sends it. Server->client
// frames are never masked (RFC 6455 requires masking only client->server).
static void ws_send_frame(Conn *target, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t header[10];
    size_t hlen = 0;

    header[hlen++] = (uint8_t)(0x80 | (opcode & 0x0F)); // FIN=1, given opcode

    if (len <= 125) {
        header[hlen++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[hlen++] = 126;
        header[hlen++] = (uint8_t)(len >> 8);
        header[hlen++] = (uint8_t)(len);
    } else {
        header[hlen++] = 127;
        for (int i = 7; i >= 0; --i) {
            header[hlen++] = (uint8_t)((uint64_t)len >> (i * 8));
        }
    }

    dbuf_append(&target->outgoing, header, hlen);
    if (len > 0) dbuf_append(&target->outgoing, payload, len);

    target->want_write = 1;
    target->want_read = 0;
    conn_update_epoll(target); // register EPOLLOUT so the loop flushes us
    handle_write(target);
}

// The one function the application layer (queue_frame/broadcast) actually
// calls. It doesn't know or care which transport `target` is using --
// that's the whole point of this layer.
static void conn_send(Conn *target, const uint8_t *data, size_t len) {
    if (target->mode == MODE_WS_ACTIVE) {
        ws_send_frame(target, 0x2 /* binary */, data, len);
    } else {
        raw_send(target, data, len);
    }
}

// Builds one app-level frame (type + username + text) and sends it to a
// single connection via conn_send, which picks the right transport.
static uint64_t htonll(uint64_t val) {
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (((uint64_t)htonl(val & 0xFFFFFFFF)) << 32) | htonl(val >> 32);
    #else
    return val;
    #endif
}

// Builds one app-level frame with message ID, timestamp, and display style
// (Server-to-Client format) and sends it via conn_send.
static void queue_frame_ext(Conn *target, uint8_t type, uint64_t msg_id, uint64_t timestamp,
                            MessageStyle style, const char *username, const char *text) {
    if (target->outgoing.len > MAX_OUTGOING_BUF) {
        target->want_close = 1;
        return;
    }

    uint32_t user_len = (uint32_t)strlen(username);
    uint32_t msg_len = (uint32_t)strlen(text);
    uint32_t user_len_be = htonl(user_len);
    uint32_t msg_len_be = htonl(msg_len);

    uint64_t msg_id_be = htonll(msg_id);
    uint64_t timestamp_be = htonll(timestamp);
    uint8_t style_byte = (uint8_t)style;

    DynamicBuffer frame = {0};
    dbuf_append(&frame, &type, 1);
    dbuf_append(&frame, &msg_id_be, 8);
    dbuf_append(&frame, &timestamp_be, 8);
    dbuf_append(&frame, &style_byte, 1);
    dbuf_append(&frame, &user_len_be, 4);
    dbuf_append(&frame, username, user_len);
    dbuf_append(&frame, &msg_len_be, 4);
    dbuf_append(&frame, text, msg_len);

    conn_send(target, frame.data, frame.len);
    free(frame.data);
}

// Builds one app-level frame (type + username + text) and sends it to a
// single connection via conn_send, which picks the right transport.
// Backward-compatible queue_frame for system/auth messages where ID and timestamp are 0.
static void queue_frame(Conn *target, uint8_t type, const char *username, const char *text) {
    queue_frame_ext(target, type, 0, 0, MESSAGE_STYLE_PLAIN, username, text);
}

// Sends a frame to every registered connection except `exclude` (pass NULL
// to include everyone). This is transport-agnostic: a raw client and a
// WebSocket client can both be in this loop and each gets the right framing.
static void broadcast_room(const char *room, Conn *exclude, uint8_t type, const char *username, const char *text) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (!c) continue;
        if (c == exclude) continue;
        if (!c->has_username || strcmp(c->room, room) != 0) continue;
        queue_frame(c, type, username, text);
    }
}

static _Bool origin_is_allowed(const char *origin) {
    if (!origin || !origin[0]) return 0;
    const char *cursor = g_allowed_origin;
    size_t origin_len = strlen(origin);
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) cursor++;
        const char *end = cursor;
        while (*end && *end != ',') end++;
        const char *trimmed_end = end;
        while (trimmed_end > cursor && isspace((unsigned char)trimmed_end[-1])) trimmed_end--;
        size_t allowed_len = (size_t)(trimmed_end - cursor);
        if (allowed_len == origin_len && strncmp(cursor, origin, origin_len) == 0) return 1;
        cursor = *end ? end + 1 : end;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Persistence (SQLite) and auth (Supabase OAuth via JWT verification).
//
// Supabase owns accounts and the OAuth relationship with providers (Google,
// GitHub, etc). The local user_profiles table is only an application profile
// cache keyed by the verified Supabase user ID; it stores the chosen nickname
// used by chat and presence.
//
// This project uses Supabase's newer asymmetric JWT signing (ES256 --
// ECDSA over the P-256 curve), not the legacy HS256 shared-secret scheme.
// That means verification needs the project's PUBLIC key, not a shared
// secret: Supabase publishes it at
//   https://<project-ref>.supabase.co/auth/v1/.well-known/jwks.json
// Fetch that once and paste the JSON it returns into the SUPABASE_JWKS
// environment variable (see jwks_init below). Unlike the JWT secret,
// this is a public key -- nothing sensitive about it -- but it's still
// loaded from the environment rather than hardcoded, since which project
// it belongs to is still config, not a source-code constant.
//
// Messages retain the nickname that was active when each message was sent.
//
// IMPORTANT CAVEAT: these SQLite queries run synchronously, on the same
// thread as the event loop. Fine for a small local database; a much
// bigger one or a slow disk would stall every connection's I/O mid-query.
// ---------------------------------------------------------------------

static sqlite3 *g_db = NULL;

static _Bool default_room_slug(const char *room) {
    return strcmp(room, "lobby") == 0 || strcmp(room, "matchpoint") == 0 ||
           strcmp(room, "signal-lab") == 0 || strcmp(room, "green-room") == 0 ||
           strcmp(room, "after-hours") == 0;
}

static void db_seed_default_rooms(void) {
    const char *sql =
        "INSERT OR IGNORE INTO rooms "
        "(slug, owner_user_id, created_at, chaos_enabled, chaos_min_delay_ms, chaos_max_delay_ms, "
        "chaos_reorder_window_ms, chaos_duplicate_percent, chaos_auto_cycle, chaos_cycle_min_ms, chaos_cycle_max_ms) VALUES "
        "('lobby', 'system', strftime('%s','now'), 0, 500, 5000, 750, 10, 0, 30000, 90000), "
        "('matchpoint', 'system', strftime('%s','now'), 1, 500, 5000, 750, 10, 0, 30000, 90000), "
        "('signal-lab', 'system', strftime('%s','now'), 1, 500, 5000, 750, 10, 0, 30000, 90000), "
        "('green-room', 'system', strftime('%s','now'), 0, 500, 5000, 750, 10, 0, 30000, 90000), "
        "('after-hours', 'system', strftime('%s','now'), 0, 500, 5000, 750, 10, 0, 30000, 90000);"
        "UPDATE rooms SET chaos_enabled = CASE WHEN slug IN ('matchpoint', 'signal-lab') THEN 1 ELSE 0 END "
        "WHERE slug IN ('lobby', 'matchpoint', 'signal-lab', 'green-room', 'after-hours');";
    char *errmsg = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "default room seed failed: %s\n", errmsg ? errmsg : "unknown error");
        sqlite3_free(errmsg);
        die("db_seed_default_rooms");
    }
}

static _Bool db_profile_upsert(const char *user_id, const char *nickname) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO user_profiles (user_id, nickname, updated_at) VALUES (?, ?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET nickname=excluded.nickname, updated_at=excluded.updated_at";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, nickname, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static _Bool db_profile_get(const char *user_id, char *nickname, size_t nickname_len) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT nickname FROM user_profiles WHERE user_id = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_STATIC);
    _Bool found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        if (value && value[0]) {
            strncpy(nickname, (const char *)value, nickname_len - 1);
            nickname[nickname_len - 1] = '\0';
            found = 1;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

// One EC public key loaded from Supabase's JWKS. Supabase may publish
// more than one during a key rotation window, distinguished by `kid`
// (key ID) -- the token's header says which one signed it.
typedef struct {
    char kid[128];
    EC_KEY *key;
} JwkKey;

static JwkKey g_jwks[MAX_JWKS_KEYS];
static int g_jwks_count = 0;

static void db_init(const char *path) {
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open failed: %s\n", sqlite3_errmsg(g_db));
        die("db_init");
    }

    // WAL journal mode is faster for write-heavy workloads and more resilient
    // to crashes than the default DELETE mode. NORMAL synchronous is safe with WAL.
    char *errmsg = NULL;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",
                 NULL, NULL, &errmsg);
    if (errmsg) {
        fprintf(stderr, "PRAGMA WAL warning: %s\n", errmsg);
        sqlite3_free(errmsg);
        errmsg = NULL;
    }

    const char *schema =
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT NOT NULL,"
        "  user_id TEXT NOT NULL DEFAULT '',"
        "  room_slug TEXT NOT NULL DEFAULT 'lobby',"
        "  text TEXT NOT NULL,"
        "  message_style INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS user_profiles ("
        "  user_id TEXT PRIMARY KEY,"
        "  nickname TEXT NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS rooms ("
        "  slug TEXT PRIMARY KEY,"
        "  owner_user_id TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  chaos_enabled INTEGER NOT NULL DEFAULT 0,"
        "  chaos_min_delay_ms INTEGER NOT NULL DEFAULT 500,"
        "  chaos_max_delay_ms INTEGER NOT NULL DEFAULT 5000,"
        "  chaos_reorder_window_ms INTEGER NOT NULL DEFAULT 750,"
        "  chaos_duplicate_percent INTEGER NOT NULL DEFAULT 10,"
        "  chaos_auto_cycle INTEGER NOT NULL DEFAULT 0,"
        "  chaos_cycle_min_ms INTEGER NOT NULL DEFAULT 30000,"
        "  chaos_cycle_max_ms INTEGER NOT NULL DEFAULT 90000"
        ");"
        "CREATE TABLE IF NOT EXISTS game_results ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  game_id TEXT NOT NULL UNIQUE,"
        "  room_slug TEXT NOT NULL,"
        "  game_type TEXT NOT NULL,"
        "  player_one_id TEXT NOT NULL,"
        "  player_one_name TEXT NOT NULL,"
        "  player_one_rounds INTEGER NOT NULL,"
        "  player_two_id TEXT NOT NULL,"
        "  player_two_name TEXT NOT NULL,"
        "  player_two_rounds INTEGER NOT NULL,"
        "  winner_id TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS game_results_room_created_idx "
        "ON game_results(room_slug, created_at DESC);";

    if (sqlite3_exec(g_db, schema, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "schema init failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        die("db_init schema");
    }

    // Migration for pre-existing databases: add user_id column if absent.
    // ALTER TABLE returns SQLITE_ERROR if the column already exists -- that's
    // expected and safe to ignore.
    sqlite3_exec(g_db,
        "ALTER TABLE messages ADD COLUMN user_id TEXT NOT NULL DEFAULT ''",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "ALTER TABLE messages ADD COLUMN room_slug TEXT NOT NULL DEFAULT 'lobby'",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "CREATE INDEX IF NOT EXISTS messages_room_id_idx ON messages(room_slug, id)",
        NULL, NULL, NULL);
    errmsg = NULL;
    if (sqlite3_exec(g_db,
            "ALTER TABLE messages ADD COLUMN message_style INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, &errmsg) != SQLITE_OK) {
        if (!errmsg || !strstr(errmsg, "duplicate column name")) {
            fprintf(stderr, "message style migration failed: %s\n", errmsg ? errmsg : "unknown error");
            if (errmsg) sqlite3_free(errmsg);
            die("db_init message_style migration");
        }
    }
    if (errmsg) sqlite3_free(errmsg);

    // Safe no-op for databases that already have these columns.
    sqlite3_exec(g_db,
        "ALTER TABLE rooms ADD COLUMN chaos_auto_cycle INTEGER NOT NULL DEFAULT 0",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "ALTER TABLE rooms ADD COLUMN chaos_cycle_min_ms INTEGER NOT NULL DEFAULT 30000",
        NULL, NULL, NULL);
    sqlite3_exec(g_db,
        "ALTER TABLE rooms ADD COLUMN chaos_cycle_max_ms INTEGER NOT NULL DEFAULT 90000",
        NULL, NULL, NULL);
    db_seed_default_rooms();
}

static _Bool room_slug_valid(const char *room) {
    size_t len = strlen(room);
    if (len == 0 || len > ROOM_SLUG_LEN) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)room[i];
        if (!(islower(ch) || isdigit(ch) || ch == '-' || ch == '_')) return 0;
    }
    return 1;
}

static _Bool db_room_ensure(const char *room, const char *owner_user_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT OR IGNORE INTO rooms "
        "(slug, owner_user_id, created_at, chaos_enabled, chaos_min_delay_ms, chaos_max_delay_ms, "
        "chaos_reorder_window_ms, chaos_duplicate_percent, chaos_auto_cycle, chaos_cycle_min_ms, chaos_cycle_max_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 30000, 90000);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, owner_user_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
    sqlite3_bind_int(stmt, 4, g_chaos_enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 5, g_chaos_min_delay_ms);
    sqlite3_bind_int(stmt, 6, g_chaos_max_delay_ms);
    sqlite3_bind_int(stmt, 7, g_chaos_reorder_window_ms);
    sqlite3_bind_int(stmt, 8, g_chaos_duplicate_percent);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static _Bool db_room_is_owner(const char *room, const char *user_id) {
    sqlite3_stmt *stmt = NULL;
    _Bool owner = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT owner_user_id FROM rooms WHERE slug = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        owner = value && strcmp((const char *)value, user_id) == 0;
    }
    sqlite3_finalize(stmt);
    return owner;
}

static ChaosConfig db_room_chaos(const char *room) {
    ChaosConfig config = {
        .enabled = g_chaos_enabled,
        .auto_cycle = 0,
        .min_delay_ms = g_chaos_min_delay_ms,
        .max_delay_ms = g_chaos_max_delay_ms,
        .reorder_window_ms = g_chaos_reorder_window_ms,
        .duplicate_percent = g_chaos_duplicate_percent,
        .cycle_min_ms = 30000,
        .cycle_max_ms = 90000,
    };
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT chaos_enabled, chaos_min_delay_ms, chaos_max_delay_ms, chaos_reorder_window_ms, chaos_duplicate_percent, chaos_auto_cycle, chaos_cycle_min_ms, chaos_cycle_max_ms FROM rooms WHERE slug = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return config;
    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        config.enabled = sqlite3_column_int(stmt, 0) != 0;
        config.min_delay_ms = sqlite3_column_int(stmt, 1);
        config.max_delay_ms = sqlite3_column_int(stmt, 2);
        config.reorder_window_ms = sqlite3_column_int(stmt, 3);
        config.duplicate_percent = sqlite3_column_int(stmt, 4);
        config.auto_cycle = sqlite3_column_int(stmt, 5) != 0;
        config.cycle_min_ms = sqlite3_column_int(stmt, 6);
        config.cycle_max_ms = sqlite3_column_int(stmt, 7);
    }
    sqlite3_finalize(stmt);
    return config;
}

static _Bool db_room_set_chaos(const char *room, const char *user_id, const ChaosConfig *config) {
    if (!db_room_is_owner(room, user_id)) return 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE rooms SET chaos_enabled=?, chaos_min_delay_ms=?, chaos_max_delay_ms=?, chaos_reorder_window_ms=?, chaos_duplicate_percent=?, chaos_auto_cycle=?, chaos_cycle_min_ms=?, chaos_cycle_max_ms=? WHERE slug=?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, config->enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 2, config->min_delay_ms);
    sqlite3_bind_int(stmt, 3, config->max_delay_ms);
    sqlite3_bind_int(stmt, 4, config->reorder_window_ms);
    sqlite3_bind_int(stmt, 5, config->duplicate_percent);
    sqlite3_bind_int(stmt, 6, config->auto_cycle ? 1 : 0);
    sqlite3_bind_int(stmt, 7, config->cycle_min_ms);
    sqlite3_bind_int(stmt, 8, config->cycle_max_ms);
    sqlite3_bind_text(stmt, 9, room, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static char *db_room_metadata_json(const char *room, const char *user_id) {
    ChaosConfig profile = db_room_chaos(room);
    ChaosConfig config = chaos_effective_config(room);
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "room", room);
    cJSON_AddBoolToObject(root, "owner", db_room_is_owner(room, user_id));
    cJSON *chaos = cJSON_AddObjectToObject(root, "chaos");
    if (chaos) {
        cJSON_AddBoolToObject(chaos, "enabled", config.enabled);
        cJSON_AddNumberToObject(chaos, "minDelayMs", config.min_delay_ms);
        cJSON_AddNumberToObject(chaos, "maxDelayMs", config.max_delay_ms);
        cJSON_AddNumberToObject(chaos, "reorderWindowMs", config.reorder_window_ms);
        cJSON_AddNumberToObject(chaos, "duplicatePercent", config.duplicate_percent);
        cJSON_AddBoolToObject(chaos, "autoCycle", profile.auto_cycle);
        cJSON_AddNumberToObject(chaos, "cycleMinMs", profile.cycle_min_ms);
        cJSON_AddNumberToObject(chaos, "cycleMaxMs", profile.cycle_max_ms);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *db_room_list_json(void) {
    sqlite3_stmt *stmt = NULL;
    cJSON *rooms = cJSON_CreateArray();
    if (!rooms) return NULL;
    const char *sql =
        "SELECT slug, owner_user_id, chaos_enabled FROM rooms "
        "WHERE slug IN ('lobby', 'matchpoint', 'signal-lab', 'green-room', 'after-hours') "
        "ORDER BY CASE slug WHEN 'lobby' THEN 1 WHEN 'matchpoint' THEN 2 WHEN 'signal-lab' THEN 3 "
        "WHEN 'green-room' THEN 4 WHEN 'after-hours' THEN 5 END";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            cJSON *room = cJSON_CreateObject();
            if (!room) continue;
            const unsigned char *slug = sqlite3_column_text(stmt, 0);
            const unsigned char *owner = sqlite3_column_text(stmt, 1);
            cJSON_AddStringToObject(room, "slug", slug ? (const char *)slug : "");
            cJSON_AddStringToObject(room, "ownerId", owner ? (const char *)owner : "");
            cJSON_AddBoolToObject(room, "chaosEnabled", sqlite3_column_int(stmt, 2) != 0);
            cJSON_AddItemToArray(rooms, room);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    char *json = cJSON_PrintUnformatted(rooms);
    cJSON_Delete(rooms);
    return json;
}

// Standard base64 decode (RFC 4648 alphabet, '+' '/', optional '=' padding).
// `in_len` must be a multiple of 4 (base64url_decode below guarantees this
// by padding before calling in). On success, *out is malloc'd -- caller
// frees it.
static int base64_decode(const char *in, size_t in_len, uint8_t **out, size_t *out_len) {
    static int8_t rev[256];
    static _Bool rev_ready = 0;
    if (!rev_ready) {
        memset(rev, -1, sizeof(rev));
        const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) rev[(unsigned char)tbl[i]] = (int8_t)i;
        rev_ready = 1;
    }
    if (in_len == 0 || in_len % 4 != 0) return -1;

    uint8_t *buf = (uint8_t *)malloc((in_len / 4) * 3);
    if (!buf) return -1;

    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        _Bool pad2 = (in[i + 2] == '=');
        _Bool pad3 = (in[i + 3] == '=');
        int c0 = rev[(unsigned char)in[i]];
        int c1 = rev[(unsigned char)in[i + 1]];
        int c2 = pad2 ? 0 : rev[(unsigned char)in[i + 2]];
        int c3 = pad3 ? 0 : rev[(unsigned char)in[i + 3]];
        if (c0 < 0 || c1 < 0 || (!pad2 && c2 < 0) || (!pad3 && c3 < 0)) {
            free(buf);
            return -1;
        }
        uint32_t triple = ((uint32_t)c0 << 18) | ((uint32_t)c1 << 12) | ((uint32_t)c2 << 6) | (uint32_t)c3;
        buf[o++] = (uint8_t)(triple >> 16);
        if (!pad2) buf[o++] = (uint8_t)(triple >> 8);
        if (!pad3) buf[o++] = (uint8_t)triple;
    }

    *out = buf;
    *out_len = o;
    return 0;
}

// Base64URL decode (RFC 4648 section 5: '-'/'_' instead of '+'/'/', and JWTs
// omit the '=' padding entirely). Converts to standard base64 + restores
// padding, then delegates to base64_decode.
static int base64url_decode(const char *in, size_t in_len, uint8_t **out, size_t *out_len) {
    size_t pad_len = (4 - (in_len % 4)) % 4;
    size_t padded_len = in_len + pad_len;

    char *buf = (char *)malloc(padded_len);
    if (!buf) return -1;
    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        buf[i] = c;
    }
    for (size_t i = 0; i < pad_len; ++i) buf[in_len + i] = '=';

    int rc = base64_decode(buf, padded_len, out, out_len);
    free(buf);
    return rc;
}

// Decodes a base64url-encoded big-endian coordinate (32 bytes for P-256)
// into a BIGNUM. Returns NULL on failure.
static BIGNUM *decode_coord_to_bn(const char *b64, size_t b64_len) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (base64url_decode(b64, b64_len, &raw, &raw_len) != 0) return NULL;
    if (raw_len != 32) { // P-256 coordinates are always exactly 32 bytes
        free(raw);
        return NULL;
    }
    BIGNUM *bn = BN_bin2bn(raw, (int)raw_len, NULL);
    free(raw);
    return bn;
}

// Loads Supabase's public signing keys from the SUPABASE_JWKS environment
// variable -- the raw JSON you get from GET-ting your project's
//   https://<project-ref>.supabase.co/auth/v1/.well-known/jwks.json
// This is a public key, not a secret, but it's still config specific to
// your project rather than something to hardcode.
static void jwks_init(void) {
    const char *jwks_json = getenv("SUPABASE_JWKS");
    if (!jwks_json || jwks_json[0] == '\0') {
        fprintf(stderr,
            "SUPABASE_JWKS is not set.\n"
            "Fetch it with: curl https://<your-project-ref>.supabase.co/auth/v1/.well-known/jwks.json\n"
            "Then run: SUPABASE_JWKS='<that JSON>' ./chat_server\n");
        die("jwks_init");
    }

    cJSON *root = cJSON_Parse(jwks_json);
    if (!root) die("jwks_init: SUPABASE_JWKS is not valid JSON");

    cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys");
    if (!cJSON_IsArray(keys)) {
        cJSON_Delete(root);
        die("jwks_init: SUPABASE_JWKS has no \"keys\" array");
    }

    cJSON *jwk;
    cJSON_ArrayForEach(jwk, keys) {
        if (g_jwks_count >= MAX_JWKS_KEYS) break;

        cJSON *kty = cJSON_GetObjectItemCaseSensitive(jwk, "kty");
        cJSON *crv = cJSON_GetObjectItemCaseSensitive(jwk, "crv");
        cJSON *kid = cJSON_GetObjectItemCaseSensitive(jwk, "kid");
        cJSON *x = cJSON_GetObjectItemCaseSensitive(jwk, "x");
        cJSON *y = cJSON_GetObjectItemCaseSensitive(jwk, "y");

        // Only EC/P-256 keys are relevant here -- this server only
        // implements ES256 verification. Skip anything else rather than
        // failing the whole startup over one unrelated key entry.
        if (!cJSON_IsString(kty) || strcmp(kty->valuestring, "EC") != 0) continue;
        if (!cJSON_IsString(crv) || strcmp(crv->valuestring, "P-256") != 0) continue;
        if (!cJSON_IsString(x) || !cJSON_IsString(y)) continue;

        BIGNUM *bn_x = decode_coord_to_bn(x->valuestring, strlen(x->valuestring));
        BIGNUM *bn_y = decode_coord_to_bn(y->valuestring, strlen(y->valuestring));
        if (!bn_x || !bn_y) {
            BN_free(bn_x);
            BN_free(bn_y);
            continue;
        }

        EC_KEY *ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        BN_CTX *ctx = BN_CTX_new();
        _Bool built = ec_key && ctx && EC_KEY_set_public_key_affine_coordinates(ec_key, bn_x, bn_y);
        BN_CTX_free(ctx);
        BN_free(bn_x);
        BN_free(bn_y);

        if (!built) {
            if (ec_key) EC_KEY_free(ec_key);
            continue;
        }

        JwkKey *slot = &g_jwks[g_jwks_count++];
        if (cJSON_IsString(kid)) {
            strncpy(slot->kid, kid->valuestring, sizeof(slot->kid) - 1);
            slot->kid[sizeof(slot->kid) - 1] = '\0';
        } else {
            slot->kid[0] = '\0';
        }
        slot->key = ec_key;
    }

    cJSON_Delete(root);

    if (g_jwks_count == 0) {
        die("jwks_init: no usable EC/P-256 keys found in SUPABASE_JWKS");
    }
    printf("Loaded %d JWT verification key(s) from SUPABASE_JWKS\n", g_jwks_count);
}

// Verifies a Supabase JWT's signature, expiry, nbf, issuer, and audience;
// extracts the display name and stable user ID (sub UUID) for the session.
// Returns 1 on success filling out_username + out_user_id; 0 on failure
// filling err with a human-readable reason.
static _Bool jwt_verify_and_extract(const char *token,
                                     char *out_username, size_t out_username_len,
                                     char *out_user_id, size_t out_user_id_len,
                                     char *err, size_t err_len) {
    out_user_id[0] = '\0'; // cleared on entry; only written on success
    const char *dot1 = strchr(token, '.');
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2) {
        snprintf(err, err_len, "malformed token");
        return 0;
    }

    // The signed content is the literal "header.payload" substring --
    // exactly as it appears in the token, before any decoding.
    size_t signing_input_len = (size_t)(dot2 - token);
    const char *sig_b64 = dot2 + 1;
    size_t sig_b64_len = strlen(sig_b64);

    // Decode and check the header BEFORE trusting anything else. Two
    // things matter here: `alg` must be exactly ES256 -- accepting
    // whatever algorithm the token claims would open an algorithm-
    // confusion attack (e.g. an attacker crafting an HS256 token "signed"
    // using the public key bytes as an HMAC secret, which some naive
    // verifiers have historically fallen for) -- and `kid` tells us which
    // of Supabase's (possibly several, during key rotation) public keys
    // to check against.
    uint8_t *header_bytes = NULL;
    size_t header_len = 0;
    if (base64url_decode(token, (size_t)(dot1 - token), &header_bytes, &header_len) != 0) {
        snprintf(err, err_len, "malformed token header");
        return 0;
    }
    char *header_str = (char *)malloc(header_len + 1);
    if (!header_str) { free(header_bytes); snprintf(err, err_len, "server error"); return 0; }
    memcpy(header_str, header_bytes, header_len);
    header_str[header_len] = '\0';
    free(header_bytes);

    cJSON *header = cJSON_Parse(header_str);
    free(header_str);
    if (!header) { snprintf(err, err_len, "malformed token header"); return 0; }

    cJSON *alg = cJSON_GetObjectItemCaseSensitive(header, "alg");
    cJSON *kid = cJSON_GetObjectItemCaseSensitive(header, "kid");
    _Bool alg_ok = cJSON_IsString(alg) && strcmp(alg->valuestring, "ES256") == 0;

    EC_KEY *verify_key = NULL;
    if (alg_ok) {
        if (cJSON_IsString(kid)) {
            for (int i = 0; i < g_jwks_count; ++i) {
                if (strcmp(g_jwks[i].kid, kid->valuestring) == 0) { verify_key = g_jwks[i].key; break; }
            }
        } else if (g_jwks_count == 1) {
            verify_key = g_jwks[0].key; // no kid to match, but only one key loaded anyway
        }
    }
    cJSON_Delete(header);

    if (!alg_ok) { snprintf(err, err_len, "unsupported or missing token algorithm"); return 0; }
    if (!verify_key) { snprintf(err, err_len, "no matching verification key for this token"); return 0; }

    // ES256's signature is the raw concatenation r||s (32 bytes each for
    // P-256) -- NOT the DER-encoded form OpenSSL's high-level signing
    // API produces by default. Decode it and split it by hand.
    uint8_t *sig_bytes = NULL;
    size_t sig_len = 0;
    if (base64url_decode(sig_b64, sig_b64_len, &sig_bytes, &sig_len) != 0 || sig_len != 64) {
        free(sig_bytes);
        snprintf(err, err_len, "malformed token signature");
        return 0;
    }

    BIGNUM *r = BN_bin2bn(sig_bytes, 32, NULL);
    BIGNUM *s = BN_bin2bn(sig_bytes + 32, 32, NULL);
    free(sig_bytes);

    ECDSA_SIG *ecdsa_sig = ECDSA_SIG_new();
    if (!r || !s || !ecdsa_sig || !ECDSA_SIG_set0(ecdsa_sig, r, s)) {
        BN_free(r);
        BN_free(s);
        if (ecdsa_sig) ECDSA_SIG_free(ecdsa_sig);
        snprintf(err, err_len, "malformed token signature");
        return 0;
        // Note: if ECDSA_SIG_set0 succeeded, r/s ownership passed to
        // ecdsa_sig, so they must NOT be freed separately below.
    }

    // Asymmetric signing signs a hash of the content, not the content
    // itself -- compute SHA-256 over the same "header.payload" bytes the
    // signature was calculated over.
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    _Bool hash_ok = mdctx &&
                    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) &&
                    EVP_DigestUpdate(mdctx, token, signing_input_len) &&
                    EVP_DigestFinal_ex(mdctx, digest, &digest_len);
    if (mdctx) EVP_MD_CTX_free(mdctx);

    if (!hash_ok) {
        ECDSA_SIG_free(ecdsa_sig);
        snprintf(err, err_len, "server error hashing token");
        return 0;
    }

    int verify_rc = ECDSA_do_verify(digest, (int)digest_len, ecdsa_sig, verify_key);
    ECDSA_SIG_free(ecdsa_sig); // frees r and s too

    if (verify_rc != 1) {
        snprintf(err, err_len, "invalid token signature");
        return 0;
    }

    // Signature is genuine -- now decode and read the payload claims.
    const char *payload_b64 = dot1 + 1;
    size_t payload_b64_len = (size_t)(dot2 - payload_b64);
    uint8_t *payload_bytes = NULL;
    size_t payload_len = 0;
    if (base64url_decode(payload_b64, payload_b64_len, &payload_bytes, &payload_len) != 0) {
        snprintf(err, err_len, "malformed token payload");
        return 0;
    }

    char *payload_json_str = (char *)malloc(payload_len + 1);
    if (!payload_json_str) {
        free(payload_bytes);
        snprintf(err, err_len, "server error");
        return 0;
    }
    memcpy(payload_json_str, payload_bytes, payload_len);
    payload_json_str[payload_len] = '\0';
    free(payload_bytes);

    cJSON *root = cJSON_Parse(payload_json_str);
    free(payload_json_str);
    if (!root) {
        snprintf(err, err_len, "malformed token claims");
        return 0;
    }

    _Bool ok = 0;
    do {
        // exp: reject tokens past their expiry, with clock-skew tolerance.
        cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
        if (!cJSON_IsNumber(exp)) { snprintf(err, err_len, "token missing exp claim"); break; }
        if ((time_t)exp->valuedouble < time(NULL) - JWT_CLOCK_SKEW_SECS) {
            snprintf(err, err_len, "token expired"); break;
        }

        // nbf (not before): reject tokens that are not yet valid.
        cJSON *nbf_claim = cJSON_GetObjectItemCaseSensitive(root, "nbf");
        if (cJSON_IsNumber(nbf_claim)) {
            if (time(NULL) + JWT_CLOCK_SKEW_SECS < (time_t)nbf_claim->valuedouble) {
                snprintf(err, err_len, "token not yet valid (nbf)"); break;
            }
        }

        // iss (issuer): if SUPABASE_ISSUER is configured, the token must match.
        // This prevents tokens from other Supabase projects being accepted here.
        if (g_expected_issuer[0] != '\0') {
            cJSON *iss = cJSON_GetObjectItemCaseSensitive(root, "iss");
            if (!cJSON_IsString(iss) || strcmp(iss->valuestring, g_expected_issuer) != 0) {
                snprintf(err, err_len, "unexpected token issuer"); break;
            }
        }

        // aud: Supabase sets this to "authenticated" for logged-in users.
        // Requiring it guards against tokens issued for other purposes.
        cJSON *aud = cJSON_GetObjectItemCaseSensitive(root, "aud");
        if (!cJSON_IsString(aud) || strcmp(aud->valuestring, "authenticated") != 0) {
            snprintf(err, err_len, "unexpected token audience"); break;
        }

        // sub (subject): the stable Supabase UUID that uniquely identifies
        // this user across all sessions and name changes.
        cJSON *sub_claim = cJSON_GetObjectItemCaseSensitive(root, "sub");
        if (!cJSON_IsString(sub_claim) || !sub_claim->valuestring[0]) {
            snprintf(err, err_len, "token missing sub claim"); break;
        }
        strncpy(out_user_id, sub_claim->valuestring, out_user_id_len - 1);
        out_user_id[out_user_id_len - 1] = '\0';

        // Display name: prefer a human-readable name from user_metadata,
        // falling back to email, then the UUID sub as a last resort.
        const char *display = NULL;
        char saved_nickname[MAX_USERNAME_LEN + 1];
        cJSON *user_metadata = cJSON_GetObjectItemCaseSensitive(root, "user_metadata");
        if (cJSON_IsObject(user_metadata)) {
            cJSON *nickname = cJSON_GetObjectItemCaseSensitive(user_metadata, "nickname");
            if (cJSON_IsString(nickname) && nickname->valuestring[0]) display = nickname->valuestring;
            cJSON *full_name = cJSON_GetObjectItemCaseSensitive(user_metadata, "full_name");
            if (!display && cJSON_IsString(full_name) && full_name->valuestring[0]) display = full_name->valuestring;
            if (!display) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(user_metadata, "name");
                if (cJSON_IsString(name) && name->valuestring[0]) display = name->valuestring;
            }
            if (!display) {
                cJSON *user_name = cJSON_GetObjectItemCaseSensitive(user_metadata, "user_name");
                if (cJSON_IsString(user_name) && user_name->valuestring[0]) display = user_name->valuestring;
            }
        }
        if (!display) {
            if (db_profile_get(out_user_id, saved_nickname, sizeof(saved_nickname))) display = saved_nickname;
        }
        if (!display) {
            cJSON *email = cJSON_GetObjectItemCaseSensitive(root, "email");
            if (cJSON_IsString(email) && email->valuestring[0]) display = email->valuestring;
        }
        if (!display) display = out_user_id; // guaranteed non-empty at this point

        strncpy(out_username, display, out_username_len - 1);
        out_username[out_username_len - 1] = '\0';
        ok = 1;
    } while (0);

    cJSON_Delete(root);
    return ok;
}

static _Bool text_contains_ci(const char *text, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return 1;
    for (; *text; ++text) {
        size_t i = 0;
        while (i < needle_len && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return 1;
    }
    return 0;
}

// Fast classification for unmistakable card shapes; AI handles subtle tone.
static MessageStyle classify_message(const char *text) {
    while (isspace((unsigned char)*text)) text++;
    size_t len = strlen(text);
    if (len == 0) return MESSAGE_STYLE_PLAIN;

    const char *end = text + len;
    while (end > text && isspace((unsigned char)end[-1])) end--;
    if (end > text && end[-1] == '?') return MESSAGE_STYLE_QUESTION;

    if (text_contains_ci(text, "/s") || text_contains_ci(text, "yeah right") ||
        text_contains_ci(text, "as if") || text_contains_ci(text, "sure jan") ||
        text_contains_ci(text, "love that for me")) {
        return MESSAGE_STYLE_SARCASM;
    }

    static const char *const celebrations[] = {
        "yay", "woohoo", "let's go", "lets go", "congrats", "congratulations",
        "we won", "nailed it", "finally did it", "great news", "so proud"
    };
    for (size_t i = 0; i < sizeof(celebrations) / sizeof(celebrations[0]); ++i) {
        if (text_contains_ci(text, celebrations[i])) return MESSAGE_STYLE_CELEBRATION;
    }

    static const char *const confessions[] = {
        "ngl", "tbh", "not gonna lie", "i confess", "confession:", "i have to admit", "i must admit"
    };
    for (size_t i = 0; i < sizeof(confessions) / sizeof(confessions[0]); ++i) {
        if (text_contains_ci(text, confessions[i])) {
            return MESSAGE_STYLE_CONFESSION;
        }
    }

    size_t letters = 0, uppercase = 0, exclamations = 0, words = 0;
    _Bool in_word = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (isalpha(*p)) {
            letters++;
            if (isupper(*p)) uppercase++;
        }
        if (isalnum(*p)) {
            if (!in_word) words++;
            in_word = 1;
        } else {
            in_word = 0;
        }
        if (*p == '!') exclamations++;
    }
    if ((letters >= 4 && uppercase * 100 >= letters * 75) || exclamations >= 3) {
        return MESSAGE_STYLE_SHOUT;
    }

    // REACTS is reserved for short, standalone responses.
    static const char *const reactions[] = { "lol", "lmao", "lmfao", "bruh", "wtf", "omg", "rip" };
    if (len <= 32 && words <= 3) {
        for (size_t i = 0; i < sizeof(reactions) / sizeof(reactions[0]); ++i) {
            size_t reaction_len = strlen(reactions[i]);
            if (strncasecmp(text, reactions[i], reaction_len) == 0 &&
                (text[reaction_len] == '\0' || ispunct((unsigned char)text[reaction_len]) ||
                 isspace((unsigned char)text[reaction_len]))) {
                return MESSAGE_STYLE_REACTION;
            }
        }
    }
    return MESSAGE_STYLE_PLAIN;
}

static const char *message_style_name(MessageStyle style) {
    switch (style) {
    case MESSAGE_STYLE_REACTION: return "reaction";
    case MESSAGE_STYLE_SHOUT: return "shout";
    case MESSAGE_STYLE_CONFESSION: return "confession";
    case MESSAGE_STYLE_QUESTION: return "question";
    case MESSAGE_STYLE_CELEBRATION: return "celebration";
    case MESSAGE_STYLE_SARCASM: return "sarcasm";
    case MESSAGE_STYLE_PLAIN:
    default: return "plain";
    }
}

// Persists one chat message. Called for every MSG_CHAT that makes it
// through auth -- this is what makes history survive a server restart.
// user_id is the stable Supabase UUID (sub claim) and stays consistent
// even if the user later changes their OAuth display name.
// Returns the database row ID of the inserted message, or 0 on failure.
static int64_t db_store_message(const char *room, const char *username, const char *user_id,
                                const char *text, MessageStyle style) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO messages (room_slug, username, user_id, text, message_style, created_at) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, room,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, user_id,  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, text,     -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, style);
    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_int64(stmt, 6, now);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    g_metric_chat_persisted++;
    return sqlite3_last_insert_rowid(g_db);
}

// The existing SQLite column names predate the single-game ruleset. They now
// store each player's final point score (first to seven), preserving existing
// local databases without a destructive table rewrite.
static int64_t db_store_game_result(const GameSession *session, int host_score, int guest_score,
                                    const char *winner_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO game_results (game_id, room_slug, game_type, player_one_id, player_one_name, player_one_rounds, "
        "player_two_id, player_two_name, player_two_rounds, winner_id, created_at) VALUES (?, ?, 'pong-single', ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, session->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, session->room, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, session->host_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, session->host_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, host_score);
    sqlite3_bind_text(stmt, 6, session->guest_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, session->guest_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 8, guest_score);
    sqlite3_bind_text(stmt, 9, winner_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 10, (int64_t)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(g_db) : 0;
}

// The game ledger is intentionally separate from the message history.  It is
// durable, room-scoped, and can be rendered as a compact scoreboard even when
// the chat transcript has scrolled away or the browser reconnects later.
static char *db_game_results_json(const char *room) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT id, game_id, game_type, player_one_id, player_one_name, player_one_rounds, "
        "player_two_id, player_two_name, player_two_rounds, winner_id, created_at "
        "FROM game_results WHERE room_slug = ? ORDER BY created_at DESC, id DESC LIMIT 12";
    cJSON *results = cJSON_CreateArray();
    if (!results) return NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        cJSON_Delete(results);
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *one_id = (const char *)sqlite3_column_text(stmt, 3);
        const char *one_name = (const char *)sqlite3_column_text(stmt, 4);
        const char *two_id = (const char *)sqlite3_column_text(stmt, 6);
        const char *two_name = (const char *)sqlite3_column_text(stmt, 7);
        const char *winner_id = (const char *)sqlite3_column_text(stmt, 9);
        const char *winner = "Draw";
        if (winner_id && one_id && strcmp(winner_id, one_id) == 0) winner = one_name ? one_name : "Player one";
        else if (winner_id && two_id && strcmp(winner_id, two_id) == 0) winner = two_name ? two_name : "Player two";

        cJSON *entry = cJSON_CreateObject();
        if (!entry) continue;
        cJSON_AddNumberToObject(entry, "id", (double)sqlite3_column_int64(stmt, 0));
        cJSON_AddStringToObject(entry, "gameId", (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddStringToObject(entry, "game", (const char *)sqlite3_column_text(stmt, 2));
        cJSON_AddStringToObject(entry, "playerOne", one_name ? one_name : "Player one");
        cJSON_AddNumberToObject(entry, "playerOneScore", sqlite3_column_int(stmt, 5));
        cJSON_AddStringToObject(entry, "playerTwo", two_name ? two_name : "Player two");
        cJSON_AddNumberToObject(entry, "playerTwoScore", sqlite3_column_int(stmt, 8));
        cJSON_AddStringToObject(entry, "winner", winner);
        cJSON_AddNumberToObject(entry, "createdAt", (double)sqlite3_column_int64(stmt, 10));
        cJSON_AddItemToArray(results, entry);
    }
    sqlite3_finalize(stmt);
    char *json = cJSON_PrintUnformatted(results);
    cJSON_Delete(results);
    return json;
}

static void send_game_history(Conn *conn) {
    char *results = db_game_results_json(conn->room);
    if (!results) return;
    queue_frame(conn, MSG_GAME_HISTORY, conn->room, results);
    free(results);
}

// Sends up to HISTORY_LIMIT messages older than before_id (or latest if before_id is 0)
// to a single connection. Replayed oldest-first so they read top-to-bottom in order.
static void db_send_history_ext(Conn *conn, int64_t before_id, uint8_t response_type) {
    sqlite3_stmt *stmt;
    const char *sql;
    if (before_id > 0) {
        sql = "SELECT id, username, text, message_style, created_at FROM messages WHERE room_slug = ? AND id < ? ORDER BY id DESC LIMIT ?;";
    } else {
        sql = "SELECT id, username, text, message_style, created_at FROM messages WHERE room_slug = ? ORDER BY id DESC LIMIT ?;";
    }

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    int param_idx = 1;
    sqlite3_bind_text(stmt, param_idx++, conn->room, -1, SQLITE_STATIC);
    if (before_id > 0) {
        sqlite3_bind_int64(stmt, param_idx++, before_id);
    }
    sqlite3_bind_int(stmt, param_idx++, HISTORY_LIMIT);

    int64_t ids[HISTORY_LIMIT];
    char *usernames[HISTORY_LIMIT];
    char *texts[HISTORY_LIMIT];
    int64_t timestamps[HISTORY_LIMIT];
    MessageStyle styles[HISTORY_LIMIT];
    int count = 0;

    while (count < HISTORY_LIMIT && sqlite3_step(stmt) == SQLITE_ROW) {
        ids[count] = sqlite3_column_int64(stmt, 0);
        const unsigned char *u = sqlite3_column_text(stmt, 1);
        const unsigned char *t = sqlite3_column_text(stmt, 2);
        styles[count] = (MessageStyle)sqlite3_column_int(stmt, 3);
        timestamps[count] = sqlite3_column_int64(stmt, 4);
        usernames[count] = strdup(u ? (const char *)u : "");
        texts[count] = strdup(t ? (const char *)t : "");
        count++;
    }
    sqlite3_finalize(stmt);

    for (int i = count - 1; i >= 0; --i) {
        queue_frame_ext(conn, response_type, (uint64_t)ids[i], (uint64_t)timestamps[i],
                        styles[i], usernames[i], texts[i]);
        free(usernames[i]);
        free(texts[i]);
    }
}

// Resume after a durable ID, rather than replaying an arbitrary fixed window.
static void db_send_history_since(Conn *conn, int64_t after_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, username, text, message_style, created_at FROM messages WHERE room_slug = ? AND id > ? ORDER BY id ASC LIMIT 200";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, conn->room, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, after_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *u = sqlite3_column_text(stmt, 1);
        const unsigned char *t = sqlite3_column_text(stmt, 2);
        queue_frame_ext(conn, MSG_HISTORY_RESPONSE, (uint64_t)sqlite3_column_int64(stmt, 0),
                        (uint64_t)sqlite3_column_int64(stmt, 4), (MessageStyle)sqlite3_column_int(stmt, 3),
                        u ? (const char *)u : "", t ? (const char *)t : "");
    }
    sqlite3_finalize(stmt);
}

// Sends the last HISTORY_LIMIT messages to a single freshly-authenticated connection.
static void db_send_history(Conn *conn) {
    db_send_history_ext(conn, 0, MSG_HISTORY_RESPONSE);
}

// Broadcasts a new real-time chat message to all registered connections, including the sender,
// complete with unique database row ID and timestamp.
static void broadcast_chat(const char *room, int64_t msg_id, int64_t timestamp, MessageStyle style,
                           const char *username, const char *text) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (!c || !c->has_username || strcmp(c->room, room) != 0) continue;
        queue_frame_ext(c, MSG_CHAT, (uint64_t)msg_id, (uint64_t)timestamp, style, username, text);
        g_metric_chat_delivered++;
    }
}

// A future AI sidecar may publish a style-update event after it has examined
// context. This changes presentation only; it never changes stored message
// text, ordering, or authentication state.
static void broadcast_style_update(const char *room, int64_t msg_id, MessageStyle style) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (!c || !c->has_username || strcmp(c->room, room) != 0) continue;
        queue_frame_ext(c, MSG_STYLE_UPDATE, (uint64_t)msg_id, 0, style, "", "");
    }
    g_metric_ai_updates++;
}

// Sentiment is deliberately ephemeral presentation metadata.  It is not
// persisted with a chat message, so a slow classifier can never alter message
// order, content, or the durable chat log.
static void broadcast_sentiment_update(const char *room, int64_t msg_id, const char *sentiment_json) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (!c || !c->has_username || strcmp(c->room, room) != 0) continue;
        queue_frame_ext(c, MSG_SENTIMENT_UPDATE, (uint64_t)msg_id, 0,
                        MESSAGE_STYLE_PLAIN, "", sentiment_json);
    }
    g_metric_ai_updates++;
}

static void broadcast_game_result(const char *room, const char *result_json) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (!c || !c->has_username || strcmp(c->room, room) != 0) continue;
        queue_frame(c, MSG_GAME_RESULT, "Game Referee", result_json);
    }
}

// Sends the current set of authenticated online users to every local connection
// as a MSG_USERS_LIST frame.  Each line in the message field is:
//   "<user_id>\t<display_name>"
//
// If Redis is available, the list is fetched via HGETALL from the shared
// chat:presence hash -- this gives a globally accurate list across all
// server instances (distributed presence).
// Falls back to the local g_fd2conn scan if Redis is unavailable so the
// server continues to work in single-node mode.
// Called after any auth/join/leave/disconnect so all clients stay in sync.
static void broadcast_users_list(const char *room_name) {
    DynamicBuffer buf = {0};
    _Bool built = 0;

    // --- Distributed path: HGETALL chat:presence ---
    if (g_redis_pub_ctx) {
        char presence_key[128];
        snprintf(presence_key, sizeof(presence_key), "%s:%s", REDIS_PRESENCE_KEY, room_name);
        redisReply *r = (redisReply *)redisCommand(g_redis_pub_ctx,
            "HGETALL %s", presence_key);
        if (r && r->type == REDIS_REPLY_ARRAY && r->elements >= 2) {
            _Bool first = 1;
            // HGETALL returns alternating [field, value, field, value, ...]
            // where field = user_id, value = display_name.
            for (size_t i = 0; i + 1 < r->elements; i += 2) {
                redisReply *uid  = r->element[i];
                redisReply *name = r->element[i + 1];
                if (!uid || !name) continue;
                if (!first) dbuf_append(&buf, "\n", 1);
                dbuf_append(&buf, uid->str,  (size_t)uid->len);
                dbuf_append(&buf, "\t",      1);
                dbuf_append(&buf, name->str, (size_t)name->len);
                first = 0;
            }
            built = 1;
        }
        if (r) freeReplyObject(r);
    }

    // --- Local fallback: walk g_fd2conn ---
    if (!built) {
        _Bool first = 1;
        for (size_t i = 0; i < g_fd2conn_cap; ++i) {
            Conn *c = g_fd2conn[i];
            if (!c || !c->has_username || strcmp(c->room, room_name) != 0) continue;
            if (!first) dbuf_append(&buf, "\n", 1);
            dbuf_append(&buf, c->user_id,  strlen(c->user_id));
            dbuf_append(&buf, "\t",        1);
            dbuf_append(&buf, c->username, strlen(c->username));
            first = 0;
        }
    }

    // Null-terminate so the buffer can be passed to queue_frame as a C string.
    dbuf_append(&buf, "\0", 1);
    const char *list = buf.len > 1 ? (const char *)buf.data : "";

    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (!c || !c->has_username || strcmp(c->room, room_name) != 0) continue;
        queue_frame(c, MSG_USERS_LIST, "", list);
    }

    free(buf.data);
}

// ---------------------------------------------------------------------
// Redis Pub/Sub helpers  (synchronous hiredis)
//
// Why synchronous instead of async:
//   redisAsyncConnect() does a non-blocking TCP connect -- the socket is
//   not yet established when the call returns, so any command queued
//   immediately after (e.g. SUBSCRIBE) sits in hiredis's internal buffer
//   until the first POLLOUT wake-up.  That means the channel is never
//   actually subscribed until the event loop runs, causing the "empty
//   pubsub channels" symptom and introducing a startup race.
//
//   Synchronous connections solve this: redisConnect() blocks until the
//   TCP handshake completes (localhost: <1 ms), the SUBSCRIBE command is
//   sent and its confirmation reply is consumed immediately, and we then
//   set the fd non-blocking for use in the poll() loop.  PUBLISH is even
//   simpler -- a one-shot synchronous call with no buffering at all.
//
// g_redis_pub  : synchronous connection used only for PUBLISH.
// g_redis_sub  : synchronous connection in SUBSCRIBE mode.
//                Set non-blocking after initial SUBSCRIBE so poll() can
//                drain incoming messages without blocking the event loop.
// g_redis_sub_fd: fd registered with epoll so the loop wakes on messages.
// ---------------------------------------------------------------------

// Forward declaration needed by redis_publish_chat.
static void broadcast_chat(const char *room, int64_t msg_id, int64_t timestamp, MessageStyle style,
                           const char *username, const char *text);

// Periodic reconnect tracking.
static time_t g_redis_last_connect_attempt = 0;
#define REDIS_RECONNECT_INTERVAL_SECS 10

// ---------------------------------------------------------------------
// Redis Presence helpers
//
// chat:presence is a Redis Hash: { user_id -> display_name }
// All live server instances share this hash, giving a globally-accurate
// online-users list. The EXPIRE TTL means ghost entries self-clean after
// a server crash (no graceful HDEL on shutdown).
// ---------------------------------------------------------------------

// Register (or refresh) a user in the shared presence hash.
static void redis_presence_set(const char *room, const char *user_id, const char *display_name) {
    if (!g_redis_pub_ctx) return;
    char presence_key[128];
    snprintf(presence_key, sizeof(presence_key), "%s:%s", REDIS_PRESENCE_KEY, room);
    redisReply *r;
    // HSET chat:presence:<room> <user_id> <display_name>
    r = (redisReply *)redisCommand(g_redis_pub_ctx,
        "HSET %s %s %s", presence_key, user_id, display_name);
    if (r) freeReplyObject(r);
    // Reset the TTL so the whole hash lives for another PRESENCE_TTL_SECS.
    r = (redisReply *)redisCommand(g_redis_pub_ctx,
        "EXPIRE %s %d", presence_key, PRESENCE_TTL_SECS);
    if (r) freeReplyObject(r);
}

// Remove a user from the shared presence hash on graceful disconnect.
static void redis_presence_del(const char *room, const char *user_id) {
    if (!g_redis_pub_ctx) return;
    char presence_key[128];
    snprintf(presence_key, sizeof(presence_key), "%s:%s", REDIS_PRESENCE_KEY, room);
    redisReply *r = (redisReply *)redisCommand(g_redis_pub_ctx,
        "HDEL %s %s", presence_key, user_id);
    if (r) freeReplyObject(r);
}

// Refresh the TTL on the presence hash (called every PRESENCE_HEARTBEAT_SECS).
// Prevents the hash from expiring while users are still connected.
static void redis_presence_heartbeat(void) {
    if (!g_redis_pub_ctx) return;
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *conn = g_fd2conn[i];
        if (!conn || !conn->has_username) continue;
        _Bool already_seen = 0;
        for (size_t j = 0; j < i; ++j) {
            Conn *earlier = g_fd2conn[j];
            if (earlier && earlier->has_username && strcmp(earlier->room, conn->room) == 0) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) continue;
        char presence_key[128];
        snprintf(presence_key, sizeof(presence_key), "%s:%s", REDIS_PRESENCE_KEY, conn->room);
        redisReply *r = (redisReply *)redisCommand(g_redis_pub_ctx,
            "EXPIRE %s %d", presence_key, PRESENCE_TTL_SECS);
        if (r) freeReplyObject(r);
    }
}

// Opens (or re-opens) both Redis connections synchronously.
// The subscriber fd is set non-blocking after the SUBSCRIBE handshake so
// the poll() loop can read messages without blocking.
static void redis_connect(void) {
    // --- Publisher ---
    if (g_redis_pub_ctx) { redisFree(g_redis_pub_ctx); g_redis_pub_ctx = NULL; }
    redisContext *pub = redisConnect(g_redis_host, g_redis_port);
    if (!pub || pub->err) {
        fprintf(stderr, "[redis] publisher connect failed: %s -- running in single-node mode\n",
                pub ? pub->errstr : "OOM");
        if (pub) { redisFree(pub); }
        g_redis_pub_ctx = NULL;
    } else {
        g_redis_pub_ctx = pub;
        printf("[redis] publisher connected to %s:%d\n", g_redis_host, g_redis_port);
    }

    // --- Subscriber ---
    if (g_redis_sub_ctx) { redisFree(g_redis_sub_ctx); g_redis_sub_ctx = NULL; g_redis_sub_fd = -1; }
    redisContext *sub = redisConnect(g_redis_host, g_redis_port);
    if (!sub || sub->err) {
        fprintf(stderr, "[redis] subscriber connect failed: %s -- running in single-node mode\n",
                sub ? sub->errstr : "OOM");
        if (sub) { redisFree(sub); }
        g_redis_sub_ctx = NULL;
        g_redis_sub_fd = -1;
        return;
    }

    // Send SUBSCRIBE synchronously.  redisCommand() blocks until the
    // confirmation reply arrives -- guaranteed to be sent before we return.
    redisReply *reply = (redisReply *)redisCommand(sub, "SUBSCRIBE %s", REDIS_CHANNEL);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "[redis] SUBSCRIBE failed: %s\n",
                reply ? reply->str : "no reply");
        if (reply) freeReplyObject(reply);
        redisFree(sub);
        g_redis_sub_ctx = NULL;
        g_redis_sub_fd = -1;
        return;
    }
    freeReplyObject(reply);

    // Now set the subscriber socket non-blocking so epoll + redisGetReply
    // can drain incoming messages without stalling the event loop.
    fd_set_nb(sub->fd);
    g_redis_sub_ctx = sub;
    g_redis_sub_fd  = sub->fd;

    // Add the Redis subscriber fd to epoll so the event loop wakes on messages.
    if (g_epoll_fd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = g_redis_sub_fd };
        epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_redis_sub_fd, &ev);
    }

    printf("[redis] subscriber connected to %s:%d, channel: %s\n",
           g_redis_host, g_redis_port, REDIS_CHANNEL);
}

// Decodes one incoming Pub/Sub message from the subscriber context and
// fans it out to all local clients via broadcast_chat().
// Returns 1 if a message was processed, 0 if no complete message was
// available yet (EAGAIN / buffer empty).
static int redis_try_read_message(void) {
    if (!g_redis_sub_ctx) return 0;

    redisReply *r = NULL;
    // redisGetReplyFromReader() only inspects buffered bytes -- no I/O.
    // If the buffer is empty (non-blocking read returned EAGAIN), it
    // returns REDIS_OK with r == NULL.  We read from the socket first.
    if (redisBufferRead(g_redis_sub_ctx) != REDIS_OK) {
        // EAGAIN / EWOULDBLOCK: the non-blocking socket has no more bytes
        // buffered right now.  This is normal -- the while-loop in the event
        // loop drains until the socket is empty, so the last call always
        // hits this.  Reset the error state and return "no message yet".
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            g_redis_sub_ctx->err = 0;
            g_redis_sub_ctx->errstr[0] = '\0';
            return 0;
        }
        // Any other error means the connection really dropped.
        fprintf(stderr, "[redis] subscriber read error: %s -- will reconnect\n",
                g_redis_sub_ctx->errstr);
        redisFree(g_redis_sub_ctx);
        g_redis_sub_ctx = NULL;
        g_redis_sub_fd = -1;
        return 0;
    }

    // Try to parse one complete reply from the input buffer.
    if (redisGetReplyFromReader(g_redis_sub_ctx, (void **)&r) != REDIS_OK || !r) {
        return 0; // incomplete, wait for more data
    }

    // Pub/Sub message: ["message", channel, payload]
    if (r->type == REDIS_REPLY_ARRAY && r->elements >= 3) {
        redisReply *kind    = r->element[0];
        redisReply *payload = r->element[2];

        if (kind && kind->type == REDIS_REPLY_STRING &&
            strcmp(kind->str, "message") == 0 &&
            payload && payload->type == REDIS_REPLY_STRING) {

            cJSON *root = cJSON_ParseWithLength(payload->str, (size_t)payload->len);
            if (root) {
                cJSON *j_id   = cJSON_GetObjectItemCaseSensitive(root, "id");
                cJSON *j_ts   = cJSON_GetObjectItemCaseSensitive(root, "ts");
                cJSON *j_user = cJSON_GetObjectItemCaseSensitive(root, "user");
                cJSON *j_text = cJSON_GetObjectItemCaseSensitive(root, "text");
                cJSON *j_style = cJSON_GetObjectItemCaseSensitive(root, "style");
                cJSON *j_kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
                cJSON *j_room = cJSON_GetObjectItemCaseSensitive(root, "room");
                const char *room = (cJSON_IsString(j_room) && room_slug_valid(j_room->valuestring))
                    ? j_room->valuestring : "lobby";

                if (cJSON_IsString(j_kind) && strcmp(j_kind->valuestring, "sentiment_update") == 0) {
                    cJSON *j_sentiment = cJSON_GetObjectItemCaseSensitive(root, "sentiment");
                    cJSON *j_intensity = cJSON_GetObjectItemCaseSensitive(root, "intensity");
                    if (cJSON_IsNumber(j_id) && cJSON_IsString(j_sentiment) && cJSON_IsNumber(j_intensity) &&
                        (strcmp(j_sentiment->valuestring, "positive") == 0 ||
                         strcmp(j_sentiment->valuestring, "negative") == 0 ||
                         strcmp(j_sentiment->valuestring, "neutral") == 0) &&
                        j_intensity->valuedouble >= 0.0 && j_intensity->valuedouble <= 1.0) {
                        cJSON *event = cJSON_CreateObject();
                        if (event) {
                            cJSON_AddStringToObject(event, "sentiment", j_sentiment->valuestring);
                            cJSON_AddNumberToObject(event, "intensity", j_intensity->valuedouble);
                            char *event_json = cJSON_PrintUnformatted(event);
                            cJSON_Delete(event);
                            if (event_json) {
                                broadcast_sentiment_update(room, (int64_t)j_id->valuedouble, event_json);
                                free(event_json);
                            }
                        }
                    } else {
                        fprintf(stderr, "[redis] invalid sentiment update payload\n");
                    }
                    cJSON_Delete(root);
                    freeReplyObject(r);
                    return 1;
                }

                if (cJSON_IsString(j_kind) && strcmp(j_kind->valuestring, "style_update") == 0) {
                    if (cJSON_IsNumber(j_id) && cJSON_IsNumber(j_style) &&
                        j_style->valueint >= MESSAGE_STYLE_PLAIN &&
                        j_style->valueint <= MESSAGE_STYLE_SARCASM) {
                        broadcast_style_update(room, (int64_t)j_id->valuedouble,
                                               (MessageStyle)j_style->valueint);
                    } else {
                        fprintf(stderr, "[redis] invalid style update payload\n");
                    }
                    cJSON_Delete(root);
                    freeReplyObject(r);
                    return 1;
                }

                if (cJSON_IsString(j_kind) && strcmp(j_kind->valuestring, "bot_message") == 0) {
                    if (cJSON_IsString(j_user) && cJSON_IsString(j_text) &&
                        strlen(j_user->valuestring) <= MAX_USERNAME_LEN &&
                        strlen(j_text->valuestring) <= MAX_MSG_LEN) {
                        MessageStyle style = cJSON_IsNumber(j_style)
                            ? (MessageStyle)j_style->valueint : classify_message(j_text->valuestring);
                        if (style < MESSAGE_STYLE_PLAIN || style > MESSAGE_STYLE_SARCASM) {
                            style = MESSAGE_STYLE_PLAIN;
                        }
                        int64_t now = (int64_t)time(NULL);
                        db_room_ensure(room, "intervener");
                        int64_t msg_id = db_store_message(room, j_user->valuestring, "intervener",
                                                          j_text->valuestring, style);
                        broadcast_chat(room, msg_id, now, style, j_user->valuestring, j_text->valuestring);
                    } else {
                        fprintf(stderr, "[redis] invalid bot message payload\n");
                    }
                    cJSON_Delete(root);
                    freeReplyObject(r);
                    return 1;
                }

                // Game results are already broadcast by the authoritative
                // game-session handler.  The Pub/Sub copy exists so the AI
                // sidecar can retain it as room context and add commentary.
                if (cJSON_IsString(j_kind) && strcmp(j_kind->valuestring, "game_result") == 0) {
                    cJSON_Delete(root);
                    freeReplyObject(r);
                    return 1;
                }

                if (cJSON_IsNumber(j_id) && cJSON_IsNumber(j_ts) &&
                    cJSON_IsString(j_user) && cJSON_IsString(j_text)) {
                    MessageStyle style = cJSON_IsNumber(j_style)
                        ? (MessageStyle)j_style->valueint : classify_message(j_text->valuestring);
                    if (style < MESSAGE_STYLE_PLAIN || style > MESSAGE_STYLE_SARCASM) {
                        style = MESSAGE_STYLE_PLAIN;
                    }
                    broadcast_chat(room, (int64_t)j_id->valuedouble,
                                   (int64_t)j_ts->valuedouble, style,
                                   j_user->valuestring,
                                   j_text->valuestring);
                } else {
                    fprintf(stderr, "[redis] pub/sub payload missing required fields\n");
                }
                cJSON_Delete(root);
            } else {
                fprintf(stderr, "[redis] failed to parse pub/sub payload\n");
            }
        }
    }

    freeReplyObject(r);
    return 1;
}

// Publishes one chat message to Redis immediately. Uses a synchronous PUBLISH call
// so there is zero buffering delay -- the command is sent immediately.
// Falls back to direct broadcast_chat() if Redis is unavailable.
static void redis_publish_chat_now(const char *room, int64_t msg_id, int64_t timestamp, MessageStyle style,
                                   const char *username, const char *text) {
    if (!g_redis_pub_ctx) {
        g_metric_redis_fallbacks++;
        broadcast_chat(room, msg_id, timestamp, style, username, text);
        return;
    }

    // Build JSON.  cJSON escapes special characters in username/text,
    // preventing injection into the channel payload.
    cJSON *root = cJSON_CreateObject();
    if (!root) { broadcast_chat(room, msg_id, timestamp, style, username, text); return; }
    cJSON_AddNumberToObject(root, "id",   (double)msg_id);
    cJSON_AddNumberToObject(root, "ts",   (double)timestamp);
    cJSON_AddNumberToObject(root, "style", (double)style);
    cJSON_AddStringToObject(root, "kind", "chat");
    cJSON_AddStringToObject(root, "room", room);
    cJSON_AddStringToObject(root, "user", username);
    cJSON_AddStringToObject(root, "text", text);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) { broadcast_chat(room, msg_id, timestamp, style, username, text); return; }

    redisReply *reply = (redisReply *)redisCommand(g_redis_pub_ctx, "PUBLISH %s %s",
                                                   REDIS_CHANNEL, json);
    free(json);

    if (!reply || g_redis_pub_ctx->err) {
        fprintf(stderr, "[redis] PUBLISH failed: %s -- falling back to direct broadcast\n",
                g_redis_pub_ctx->errstr);
        if (reply) freeReplyObject(reply);
        // Publisher connection is broken; free and mark for reconnect.
        redisFree(g_redis_pub_ctx);
        g_redis_pub_ctx = NULL;
        g_metric_redis_fallbacks++;
        broadcast_chat(room, msg_id, timestamp, style, username, text);
        return;
    }
    freeReplyObject(reply);
    // Broadcast will arrive via the subscriber side for all local clients.
}

static void redis_publish_game_result(const char *room, const char *result_json) {
    if (!g_redis_pub_ctx) return;
    cJSON *root = cJSON_Parse(result_json);
    if (!root) return;
    cJSON_AddStringToObject(root, "kind", "game_result");
    cJSON_AddStringToObject(root, "room", room);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    redisReply *reply = (redisReply *)redisCommand(g_redis_pub_ctx, "PUBLISH %s %s", REDIS_CHANNEL, json);
    free(json);
    if (reply) freeReplyObject(reply);
}

static int64_t monotonic_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) die("clock_gettime");
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t chaos_random_u32(void) {
    // xorshift32: small, deterministic, and sufficient for visual timing.
    uint32_t x = g_chaos_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_chaos_rng_state = x;
    return x;
}

static int chaos_random_range(int low, int high) {
    assert(low <= high);
    uint32_t span = (uint32_t)(high - low) + 1u;
    return low + (int)(chaos_random_u32() % span);
}

static ChaosCycle *chaos_cycle_for_room(const char *room, _Bool create) {
    for (ChaosCycle *cycle = g_chaos_cycles; cycle; cycle = cycle->next) {
        if (strcmp(cycle->room, room) == 0) return cycle;
    }
    if (!create) return NULL;
    ChaosCycle *cycle = calloc(1, sizeof(*cycle));
    if (!cycle) die("calloc chaos cycle");
    strncpy(cycle->room, room, ROOM_SLUG_LEN);
    cycle->room[ROOM_SLUG_LEN] = '\0';
    cycle->next = g_chaos_cycles;
    g_chaos_cycles = cycle;
    return cycle;
}

static int64_t chaos_cycle_deadline(const ChaosConfig *config, int64_t now) {
    return now + chaos_random_range(config->cycle_min_ms, config->cycle_max_ms);
}

// Apply the current auto-pulse phase to a room profile.
static ChaosConfig chaos_effective_config(const char *room) {
    ChaosConfig config = db_room_chaos(room);
    if (!config.auto_cycle) return config;

    ChaosCycle *cycle = chaos_cycle_for_room(room, 1);
    if (cycle->next_toggle_ms == 0) {
        cycle->active = config.enabled;
        cycle->next_toggle_ms = chaos_cycle_deadline(&config, monotonic_millis());
    }
    config.enabled = cycle->active;
    return config;
}

// A manual update begins a fresh pulse interval.
static void chaos_cycle_reset(const char *room, const ChaosConfig *config) {
    ChaosCycle *cycle = chaos_cycle_for_room(room, config->auto_cycle);
    if (!cycle) return;
    if (!config->auto_cycle) {
        cycle->next_toggle_ms = 0;
        cycle->active = config->enabled;
        return;
    }
    cycle->active = config->enabled;
    cycle->next_toggle_ms = chaos_cycle_deadline(config, monotonic_millis());
}

// Publish auto-pulse state changes to room clients.
static void chaos_cycle_tick(void) {
    int64_t now = monotonic_millis();
    for (ChaosCycle *cycle = g_chaos_cycles; cycle; cycle = cycle->next) {
        if (cycle->next_toggle_ms == 0 || cycle->next_toggle_ms > now) continue;
        ChaosConfig profile = db_room_chaos(cycle->room);
        if (!profile.auto_cycle) {
            cycle->next_toggle_ms = 0;
            continue;
        }
        cycle->active = !cycle->active;
        cycle->next_toggle_ms = chaos_cycle_deadline(&profile, now);
        fprintf(stderr, "[chaos] auto pulse %s for room %s\n",
                cycle->active ? "on" : "off", cycle->room);
        broadcast_room_metadata(cycle->room, MSG_CHAOS_UPDATE);
    }
}

static void chaos_free_pending(PendingChat *pending) {
    free(pending->username);
    free(pending->text);
    free(pending);
}

static void chaos_enqueue_chat(const char *room, const ChaosConfig *config, int64_t msg_id,
                               int64_t timestamp, MessageStyle style, const char *username, const char *text) {
    if (!config->enabled || g_pending_chat_count >= CHAOS_PENDING_LIMIT) {
        if (g_pending_chat_count >= CHAOS_PENDING_LIMIT) {
            fprintf(stderr, "[chaos] queue full; delivering message %lld immediately\n",
                    (long long)msg_id);
        }
        redis_publish_chat_now(room, msg_id, timestamp, style, username, text);
        return;
    }

    PendingChat *pending = calloc(1, sizeof(*pending));
    if (!pending) die("calloc chaos message");
    pending->username = strdup(username);
    pending->text = strdup(text);
    if (!pending->username || !pending->text) die("strdup chaos message");

    int64_t now = monotonic_millis();
    int delay = chaos_random_range(config->min_delay_ms, config->max_delay_ms);
    pending->msg_id = msg_id;
    pending->timestamp = timestamp;
    pending->style = style;
    pending->min_release_ms = now + config->min_delay_ms;
    pending->release_ms = now + delay;
    pending->duplicate = chaos_random_range(1, 100) <= config->duplicate_percent;
    strncpy(pending->room, room, ROOM_SLUG_LEN);
    pending->room[ROOM_SLUG_LEN] = '\0';
    pending->next = g_pending_chat_head;
    g_pending_chat_head = pending;
    g_pending_chat_count++;
}

// Release every message eligible now. Among eligible messages we pick a
// random entry, which makes messages with close release times visibly reorder
// without releasing any before its configured minimum delay.
static void chaos_publish_due_messages(void) {
    for (;;) {
        int64_t now = monotonic_millis();
        PendingChat **chosen_link = NULL;
        size_t candidates = 0;

        for (PendingChat **link = &g_pending_chat_head; *link; link = &(*link)->next) {
            PendingChat *pending = *link;
            if (pending->min_release_ms <= now &&
                pending->release_ms <= now + g_chaos_reorder_window_ms) {
                candidates++;
                if (chaos_random_range(1, (int)candidates) == 1) {
                    chosen_link = link;
                }
            }
        }
        if (!chosen_link) return;

        PendingChat *pending = *chosen_link;
        *chosen_link = pending->next;
        g_pending_chat_count--;
        redis_publish_chat_now(pending->room, pending->msg_id, pending->timestamp, pending->style,
                               pending->username, pending->text);
        if (pending->duplicate) {
            g_metric_chaos_duplicates++;
            redis_publish_chat_now(pending->room, pending->msg_id, pending->timestamp, pending->style,
                                   pending->username, pending->text);
        }
        chaos_free_pending(pending);
    }
}

// Returns a timeout suitable for epoll_wait: it wakes as soon as at least one
// queued message can join the reorder window, capped by the normal idle sweep.
static int chaos_next_wait_ms(int fallback_ms) {
    int64_t now = monotonic_millis();
    int64_t earliest_ready = INT64_MAX;
    for (PendingChat *pending = g_pending_chat_head; pending; pending = pending->next) {
        int64_t ready = pending->release_ms - g_chaos_reorder_window_ms;
        if (ready < pending->min_release_ms) ready = pending->min_release_ms;
        if (ready < earliest_ready) earliest_ready = ready;
    }
    for (ChaosCycle *cycle = g_chaos_cycles; cycle; cycle = cycle->next) {
        if (cycle->next_toggle_ms > 0 && cycle->next_toggle_ms < earliest_ready) {
            earliest_ready = cycle->next_toggle_ms;
        }
    }
    if (earliest_ready == INT64_MAX) return fallback_ms;
    if (earliest_ready <= now) return 0;
    int64_t wait = earliest_ready - now;
    return wait < fallback_ms ? (int)wait : fallback_ms;
}

static _Bool env_is_enabled(const char *value) {
    return value && (!strcasecmp(value, "1") || !strcasecmp(value, "true") ||
                     !strcasecmp(value, "yes") || !strcasecmp(value, "on"));
}

static int chaos_env_int(const char *name, int default_value, int min_value, int max_value) {
    const char *value = getenv(name);
    if (!value || !value[0]) return default_value;
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || !end || *end != '\0' || parsed < min_value || parsed > max_value) {
        fprintf(stderr, "[chaos] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }
    return (int)parsed;
}

static void chaos_init(void) {
    g_chaos_enabled = env_is_enabled(getenv("CHAT_CHAOS_ENABLED"));
    g_chaos_min_delay_ms = chaos_env_int("CHAT_CHAOS_MIN_DELAY_MS", 500, 0, 60000);
    g_chaos_max_delay_ms = chaos_env_int("CHAT_CHAOS_MAX_DELAY_MS", 5000, 0, 60000);
    if (g_chaos_max_delay_ms < g_chaos_min_delay_ms) {
        fprintf(stderr, "[chaos] max delay is below min delay; using min for both\n");
        g_chaos_max_delay_ms = g_chaos_min_delay_ms;
    }
    g_chaos_reorder_window_ms = chaos_env_int("CHAT_CHAOS_REORDER_WINDOW_MS", 750, 0, 60000);
    g_chaos_duplicate_percent = chaos_env_int("CHAT_CHAOS_DUPLICATE_PERCENT", 10, 0, 100);
    g_chaos_rng_state ^= (uint32_t)time(NULL) ^ (uint32_t)getpid();

    printf("Chaos mode %s (delay %d-%d ms, reorder window %d ms, duplicates %d%%)\n",
           g_chaos_enabled ? "enabled" : "disabled", g_chaos_min_delay_ms,
           g_chaos_max_delay_ms, g_chaos_reorder_window_ms, g_chaos_duplicate_percent);
}

// Public chat publication path: either hold the message for chaos delivery or
// immediately hand it to Redis/the local fallback.
static void redis_publish_chat(const char *room, int64_t msg_id, int64_t timestamp, MessageStyle style,
                               const char *username, const char *text) {
    ChaosConfig config = db_room_chaos(room);
    chaos_enqueue_chat(room, &config, msg_id, timestamp, style, username, text);
}

// No-op: flush helper kept for API compatibility; sync publish needs none.
static void redis_flush_pub(void) { /* sync publish has no internal buffer */ }


// ---------------------------------------------------------------------
// Minimal self-contained SHA-1 and Base64, needed only for the WebSocket
// handshake (RFC 6455: Sec-WebSocket-Accept = base64(sha1(key + GUID))).
// No external crypto library required.
// ---------------------------------------------------------------------

typedef struct {
    uint32_t h[5];
    uint64_t total_len;
    uint8_t buffer[64];
    size_t buffer_len;
} Sha1Ctx;

static uint32_t sha1_rol(uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void sha1_init(Sha1Ctx *ctx) {
    ctx->h[0] = 0x67452301;
    ctx->h[1] = 0xEFCDAB89;
    ctx->h[2] = 0x98BADCFE;
    ctx->h[3] = 0x10325476;
    ctx->h[4] = 0xC3D2E1F0;
    ctx->total_len = 0;
    ctx->buffer_len = 0;
}

static void sha1_process_block(Sha1Ctx *ctx, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t temp = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rol(b, 30); b = a; a = temp;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d; ctx->h[4] += e;
}

static void sha1_update(Sha1Ctx *ctx, const uint8_t *data, size_t len) {
    ctx->total_len += len;
    while (len > 0) {
        size_t n = 64 - ctx->buffer_len;
        if (n > len) n = len;
        memcpy(ctx->buffer + ctx->buffer_len, data, n);
        ctx->buffer_len += n;
        data += n;
        len -= n;
        if (ctx->buffer_len == 64) {
            sha1_process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t digest[20]) {
    uint64_t bitlen = ctx->total_len * 8;
    size_t idx = ctx->buffer_len;

    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buffer[idx++] = 0;
        sha1_process_block(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) ctx->buffer[idx++] = 0;
    for (int i = 7; i >= 0; --i) ctx->buffer[idx++] = (uint8_t)(bitlen >> (i * 8));
    sha1_process_block(ctx, ctx->buffer);

    for (int i = 0; i < 5; ++i) {
        digest[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

// Caller must provide `out` sized for at least 4*ceil(len/3)+1 bytes.
static void base64_encode(const uint8_t *data, size_t len, char *out) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = tbl[(n >> 6) & 0x3F];
        out[o++] = tbl[n & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = tbl[(n >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------
// WebSocket handshake (HTTP Upgrade -> 101 Switching Protocols)
// ---------------------------------------------------------------------

// Buffers an HTTP upgrade request until the blank line that ends the
// headers, extracts Sec-WebSocket-Key, and responds. On success, flips
// conn->mode to MODE_WS_ACTIVE and consumes the request bytes -- any bytes
// after the blank line (which shouldn't normally exist for a GET) are left
// in conn->incoming for the next stage to interpret as WS frames.
static void try_ws_handshake(Conn *conn) {
    // Look for the blank line ending the HTTP headers.
    uint8_t *data = conn->incoming.data;
    size_t len = conn->incoming.len;
    size_t header_end = 0;
    _Bool found = 0;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            header_end = i + 4;
            found = 1;
            break;
        }
    }

    if (!found) {
        if (len > MAX_HANDSHAKE_LEN) {
            msg("handshake request too large, closing");
            conn->want_close = 1;
        }
        return; // wait for more data
    }

    // Null-terminate a copy of just the header block so we can use string
    // functions on it safely.
    char *headers = (char *)malloc(header_end + 1);
    if (!headers) die("malloc(headers)");
    memcpy(headers, data, header_end);
    headers[header_end] = '\0';

    // The same listener exposes minimal operational probes before a WebSocket
    // upgrade.  They are intentionally unauthenticated and contain counters
    // only—safe for a local reverse proxy or container health check.
    if (strncmp(headers, "GET /healthz ", 13) == 0 || strncmp(headers, "GET /readyz ", 12) == 0 ||
        strncmp(headers, "GET /metrics ", 13) == 0) {
        _Bool metrics = strncmp(headers, "GET /metrics ", 13) == 0;
        char body[2048];
        if (metrics) {
            snprintf(body, sizeof(body),
                "# TYPE chaos_chat_connections gauge\nchaos_chat_connections %d\n"
                "# TYPE chaos_chat_pending_messages gauge\nchaos_chat_pending_messages %zu\n"
                "# TYPE chaos_chat_persisted_total counter\nchaos_chat_persisted_total %llu\n"
                "# TYPE chaos_chat_delivered_total counter\nchaos_chat_delivered_total %llu\n"
                "# TYPE chaos_chat_duplicates_total counter\nchaos_chat_duplicates_total %llu\n"
                "# TYPE chaos_chat_redis_fallbacks_total counter\nchaos_chat_redis_fallbacks_total %llu\n"
                "# TYPE chaos_chat_ai_updates_total counter\nchaos_chat_ai_updates_total %llu\n"
                "# TYPE chaos_chat_challenges_total counter\nchaos_chat_challenges_total %llu\n"
                "# TYPE chaos_chat_games_completed_total counter\nchaos_chat_games_completed_total %llu\n"
                "chaos_chat_redis_connected %d\nchaos_chat_database_ready %d\n",
                g_conn_count, g_pending_chat_count, (unsigned long long)g_metric_chat_persisted,
                (unsigned long long)g_metric_chat_delivered, (unsigned long long)g_metric_chaos_duplicates,
                (unsigned long long)g_metric_redis_fallbacks, (unsigned long long)g_metric_ai_updates,
                (unsigned long long)g_metric_challenges_created, (unsigned long long)g_metric_games_completed,
                g_redis_pub_ctx && g_redis_sub_ctx ? 1 : 0, g_db ? 1 : 0);
        } else {
            snprintf(body, sizeof(body), "{\"status\":\"%s\",\"database\":%s,\"redis\":%s}\n",
                g_db ? "ok" : "unavailable", g_db ? "true" : "false",
                g_redis_pub_ctx && g_redis_sub_ctx ? "true" : "false");
        }
        char response[4096];
        int response_len = snprintf(response, sizeof(response),
            "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n%s",
            g_db ? "200 OK" : "503 Service Unavailable", metrics ? "text/plain; version=0.0.4" : "application/json",
            strlen(body), body);
        raw_send(conn, (const uint8_t *)response, (size_t)response_len);
        conn->want_close = 1;
        free(headers);
        return;
    }

    // Scan all headers once to extract both Sec-WebSocket-Key and Origin.
    // We used to break as soon as we found the key, but we now need Origin
    // too for CSRF-over-WebSocket protection, so we run the full loop.
    char *key = NULL;
    char *origin = NULL;
    char *line = headers;
    while (line && *line) {
        char *line_end = strstr(line, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);

        if (line_len > 18 && strncasecmp(line, "Sec-WebSocket-Key:", 18) == 0) {
            if (!key) {
                char *val = line + 18;
                while (*val == ' ') val++;
                size_t val_len = (size_t)((line + line_len) - val);
                key = (char *)malloc(val_len + 1);
                if (!key) die("malloc(key)");
                memcpy(key, val, val_len);
                key[val_len] = '\0';
            }
        } else if (line_len > 7 && strncasecmp(line, "Origin:", 7) == 0) {
            if (!origin) {
                char *val = line + 7;
                while (*val == ' ') val++;
                size_t val_len = (size_t)((line + line_len) - val);
                origin = (char *)malloc(val_len + 1);
                if (!origin) die("malloc(origin)");
                memcpy(origin, val, val_len);
                origin[val_len] = '\0';
            }
        }

        if (!line_end) break;
        line = line_end + 2;
        if (line[0] == '\r' && line[1] == '\n') break; // reached blank line
    }

    // Origin validation: if CHAT_ALLOWED_ORIGIN is set, any WebSocket
    // upgrade from a different origin is a potential CSRF attack.
    if (g_allowed_origin[0] != '\0') {
        if (!origin_is_allowed(origin)) {
            char rejection[768];
            snprintf(rejection, sizeof(rejection), "WebSocket upgrade rejected: Origin '%s' not in allowlist",
                     origin ? origin : "(missing)");
            msg(rejection);
            const char *resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
            raw_send(conn, (const uint8_t *)resp, strlen(resp));
            conn->want_close = 1;
            free(origin);
            free(key);
            free(headers);
            return;
        }
    }
    free(origin);

    if (!key) {
        msg("WebSocket upgrade missing Sec-WebSocket-Key, closing");
        const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        raw_send(conn, (const uint8_t *)resp, strlen(resp));
        conn->want_close = 1;
        free(headers);
        return;
    }

    // accept = base64(sha1(key + GUID))
    char concat[256 + sizeof(WS_GUID)];
    int n = snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    Sha1Ctx sha;
    sha1_init(&sha);
    sha1_update(&sha, (const uint8_t *)concat, (size_t)n);
    uint8_t digest[20];
    sha1_final(&sha, digest);
    char accept_b64[32];
    base64_encode(digest, sizeof(digest), accept_b64);

    char response[512];
    int rn = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_b64);

    raw_send(conn, (const uint8_t *)response, (size_t)rn);

    conn->mode = MODE_WS_ACTIVE;
    dbuf_consume(&conn->incoming, header_end);

    free(key);
    free(headers);
}

// ---------------------------------------------------------------------
// WebSocket frame decode (client -> server, always masked per RFC 6455)
// ---------------------------------------------------------------------

// Decodes at most one WebSocket frame from conn->incoming. Returns 1 if a
// frame was processed (there may be more buffered), 0 if we need more
// bytes before we can make progress. Payload from text/binary frames is
// appended to conn->app_incoming for try_one_request to parse; control
// frames (ping/pong/close) are handled here directly.
static _Bool ws_try_decode_frame(Conn *conn) {
    DynamicBuffer *buf = &conn->incoming;
    if (buf->len < 2) return 0;

    uint8_t byte0 = buf->data[0];
    uint8_t byte1 = buf->data[1];
    _Bool fin = (byte0 & 0x80) != 0;
    uint8_t opcode = byte0 & 0x0F;
    _Bool masked = (byte1 & 0x80) != 0;
    uint64_t paylen7 = byte1 & 0x7F;

    size_t pos = 2;
    uint64_t payload_len;

    if (paylen7 <= 125) {
        payload_len = paylen7;
    } else if (paylen7 == 126) {
        if (buf->len < pos + 2) return 0;
        payload_len = ((uint64_t)buf->data[pos] << 8) | buf->data[pos + 1];
        pos += 2;
    } else {
        if (buf->len < pos + 8) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | buf->data[pos + i];
        pos += 8;
    }

    // Reject before waiting for a payload this large to buffer -- same
    // "validate before you wait" principle as try_one_request.
    if (payload_len > (uint64_t)(MAX_MSG_LEN + 4096)) {
        msg("WebSocket frame too large, closing");
        conn->want_close = 1;
        return 0;
    }

    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) {
        if (buf->len < pos + 4) return 0;
        memcpy(mask_key, buf->data + pos, 4);
        pos += 4;
    }

    size_t header_len = pos;
    if (buf->len < header_len + payload_len) return 0; // wait for full payload

    uint8_t *payload = buf->data + header_len;
    if (masked) {
        for (uint64_t i = 0; i < payload_len; ++i) {
            payload[i] ^= mask_key[i % 4];
        }
    }

    switch (opcode) {
    case 0x1: // text
    case 0x2: // binary
        if (!fin) {
            // Fragmented messages aren't supported yet -- our messages are
            // small enough that browsers send them in one frame. Treat a
            // continuation as a protocol error rather than silently
            // mis-parsing it.
            msg("fragmented WebSocket messages not supported, closing");
            conn->want_close = 1;
            break;
        }
        dbuf_append(&conn->app_incoming, payload, (size_t)payload_len);
        break;
    case 0x8: // close
        ws_send_frame(conn, 0x8, NULL, 0); // echo close per RFC 6455
        conn->want_close = 1;
        break;
    case 0x9: // ping
        ws_send_frame(conn, 0xA, payload, (size_t)payload_len); // pong
        break;
    case 0xA: // pong
        break; // nothing to do
    default:
        break; // ignore reserved/unsupported opcodes
    }

    dbuf_consume(&conn->incoming, header_len + (size_t)payload_len);
    return (conn->incoming.len > 0);
}

// ---------------------------------------------------------------------
// Application-level protocol parsing (unchanged logic, now parameterized
// on which buffer to read from -- conn->incoming for raw clients,
// conn->app_incoming for decoded WebSocket payloads).
// ---------------------------------------------------------------------

// Looks up a live, authenticated connection by its Supabase user_id.
// Used by the WebRTC signaling path to route to a specific peer.
static Conn *find_conn_by_user_id(const char *user_id) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (c && c->has_username && strcmp(c->user_id, user_id) == 0)
            return c;
    }
    return NULL;
}

// Resolve duplicate local-test identities within the active room.
static Conn *find_conn_by_user_id_in_room(const char *user_id, const char *room) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *c = g_fd2conn[i];
        if (c && c->has_username && strcmp(c->user_id, user_id) == 0 && strcmp(c->room, room) == 0)
            return c;
    }
    return NULL;
}

static void make_game_id(char out[GAME_ID_LEN + 1]) {
    unsigned char bytes[12];
    randombytes_buf(bytes, sizeof(bytes));
    snprintf(out, GAME_ID_LEN + 1, "pong-");
    size_t offset = 5;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        snprintf(out + offset, GAME_ID_LEN + 1 - offset, "%02x", bytes[i]);
        offset += 2;
    }
}

static Challenge *find_challenge(const char *id) {
    for (Challenge *challenge = g_challenges; challenge; challenge = challenge->next) {
        if (strcmp(challenge->id, id) == 0) return challenge;
    }
    return NULL;
}

static void remove_challenge(Challenge *target) {
    for (Challenge **link = &g_challenges; *link; link = &(*link)->next) {
        if (*link == target) {
            *link = target->next;
            free(target);
            return;
        }
    }
}

static GameSession *find_game_session(const char *id) {
    for (GameSession *session = g_game_sessions; session; session = session->next) {
        if (strcmp(session->id, id) == 0) return session;
    }
    return NULL;
}

static void remove_game_session(GameSession *target) {
    for (GameSession **link = &g_game_sessions; *link; link = &(*link)->next) {
        if (*link == target) {
            *link = target->next;
            free(target);
            return;
        }
    }
}

static _Bool game_has_user(const GameSession *session, const char *user_id) {
    return strcmp(session->host_id, user_id) == 0 || strcmp(session->guest_id, user_id) == 0;
}

static Conn *game_opponent(const GameSession *session, const Conn *sender) {
    const char *opponent_id = strcmp(session->host_id, sender->user_id) == 0
        ? session->guest_id : session->host_id;
    return find_conn_by_user_id_in_room(opponent_id, session->room);
}

static void send_challenge_event(Conn *target, const char *action, const Challenge *challenge) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "id", challenge->id);
    cJSON_AddStringToObject(root, "game", "pong");
    cJSON_AddStringToObject(root, "room", challenge->room);
    cJSON_AddStringToObject(root, "requesterId", challenge->requester_id);
    cJSON_AddStringToObject(root, "requesterName", challenge->requester_name);
    cJSON_AddStringToObject(root, "targetId", challenge->target_id);
    cJSON_AddStringToObject(root, "targetName", challenge->target_name);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        queue_frame(target, MSG_CHALLENGE, challenge->requester_id, json);
        free(json);
    }
}

static void send_game_started(Conn *target, const GameSession *session, _Bool host) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "action", "accepted");
    cJSON_AddStringToObject(root, "game", "pong");
    cJSON_AddStringToObject(root, "gameId", session->id);
    cJSON_AddStringToObject(root, "room", session->room);
    cJSON_AddStringToObject(root, "role", host ? "host" : "guest");
    cJSON_AddStringToObject(root, "opponentId", host ? session->guest_id : session->host_id);
    cJSON_AddStringToObject(root, "opponentName", host ? session->guest_name : session->host_name);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        queue_frame(target, MSG_CHALLENGE, host ? session->guest_id : session->host_id, json);
        free(json);
    }
}

static Challenge *create_challenge(const Conn *requester, const Conn *target) {
    Challenge *challenge = calloc(1, sizeof(*challenge));
    if (!challenge) return NULL;
    make_game_id(challenge->id);
    strncpy(challenge->room, requester->room, ROOM_SLUG_LEN);
    strncpy(challenge->requester_id, requester->user_id, USER_ID_LEN);
    strncpy(challenge->requester_name, requester->username, MAX_USERNAME_LEN);
    strncpy(challenge->target_id, target->user_id, USER_ID_LEN);
    strncpy(challenge->target_name, target->username, MAX_USERNAME_LEN);
    challenge->expires_at = time(NULL) + 60;
    challenge->next = g_challenges;
    g_challenges = challenge;
    g_metric_challenges_created++;
    return challenge;
}

static GameSession *start_game_from_challenge(const Challenge *challenge) {
    GameSession *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    strncpy(session->id, challenge->id, GAME_ID_LEN);
    strncpy(session->room, challenge->room, ROOM_SLUG_LEN);
    strncpy(session->host_id, challenge->requester_id, USER_ID_LEN);
    strncpy(session->host_name, challenge->requester_name, MAX_USERNAME_LEN);
    strncpy(session->guest_id, challenge->target_id, USER_ID_LEN);
    strncpy(session->guest_name, challenge->target_name, MAX_USERNAME_LEN);
    session->expires_at = time(NULL) + 20 * 60;
    session->next = g_game_sessions;
    g_game_sessions = session;
    return session;
}

static void game_send_abandoned(GameSession *session, const char *leaver_id) {
    const char *opponent_id = strcmp(session->host_id, leaver_id) == 0 ? session->guest_id : session->host_id;
    Conn *opponent = find_conn_by_user_id_in_room(opponent_id, session->room);
    if (!opponent) return;
    char event[128];
    snprintf(event, sizeof(event), "{\"kind\":\"abandoned\",\"gameId\":\"%s\"}", session->id);
    queue_frame(opponent, MSG_GAME_EVENT, leaver_id, event);
}

static void game_cleanup_for_user(const char *user_id) {
    for (Challenge *challenge = g_challenges, *next = NULL; challenge; challenge = next) {
        next = challenge->next;
        if (strcmp(challenge->requester_id, user_id) == 0 || strcmp(challenge->target_id, user_id) == 0) {
            remove_challenge(challenge);
        }
    }
    for (GameSession *session = g_game_sessions, *next = NULL; session; session = next) {
        next = session->next;
        if (game_has_user(session, user_id)) {
            game_send_abandoned(session, user_id);
            remove_game_session(session);
        }
    }
}

static void game_sweep_expired(void) {
    time_t now = time(NULL);
    for (Challenge *challenge = g_challenges, *next = NULL; challenge; challenge = next) {
        next = challenge->next;
        if (challenge->expires_at <= now) remove_challenge(challenge);
    }
    for (GameSession *session = g_game_sessions, *next = NULL; session; session = next) {
        next = session->next;
        if (session->expires_at <= now) {
            game_send_abandoned(session, session->host_id);
            remove_game_session(session);
        }
    }
}

static _Bool dev_identity_from_name(const char *requested, char *out_username,
                                    size_t out_username_len, char *out_user_id,
                                    size_t out_user_id_len) {
    size_t len = strlen(requested);
    if (len == 0 || len > 48) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)requested[i];
        if (!(isalnum(ch) || ch == ' ' || ch == '-' || ch == '_')) return 0;
    }
    // Demo suffixes make room-local bot identities while preserving display names.
    const char *marker = strstr(requested, "__demo__");
    size_t display_len = marker ? (size_t)(marker - requested) : len;
    if (display_len == 0 || display_len + 1 > out_username_len) return 0;
    if (marker) {
        const char *room_tag = marker + strlen("__demo__");
        if (!room_slug_valid(room_tag)) return 0;
    }
    if (snprintf(out_user_id, out_user_id_len, "dev:%s", requested) >= (int)out_user_id_len) return 0;
    memcpy(out_username, requested, display_len);
    out_username[display_len] = '\0';
    return 1;
}

static _Bool client_message_id_valid(const char *value) {
    size_t len = strlen(value);
    if (len == 0 || len > 64) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (!(isalnum(ch) || ch == '-' || ch == '_')) return 0;
    }
    return 1;
}

static int room_chaos_int(cJSON *root, const char *field, int fallback, int minimum, int maximum) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsNumber(value)) return fallback;
    int parsed = value->valueint;
    return parsed < minimum || parsed > maximum ? fallback : parsed;
}

static void send_room_metadata(Conn *conn, uint8_t type) {
    char *json = db_room_metadata_json(conn->room, conn->user_id);
    if (!json) return;
    queue_frame(conn, type, conn->room, json);
    free(json);
}

static void broadcast_room_metadata(const char *room, uint8_t type) {
    for (size_t i = 0; i < g_fd2conn_cap; ++i) {
        Conn *conn = g_fd2conn[i];
        if (!conn || !conn->has_username || strcmp(conn->room, room) != 0) continue;
        send_room_metadata(conn, type);
    }
}

static _Bool try_one_request(Conn *conn, DynamicBuffer *buf) {
    if (buf->len < 5) {
        return 0; // Not enough data yet
    }

    uint8_t msg_type = buf->data[0];

    uint32_t user_len = 0;
    memcpy(&user_len, buf->data + 1, 4);
    user_len = ntohl(user_len);

    // Validate lengths BEFORE waiting for the corresponding payload bytes.
    // See the v1 comments: this closes a memory-exhaustion DoS and avoids
    // stack-VLA sizing from attacker-controlled values.
    if (user_len > MAX_USERNAME_LEN) {
        msg("username too long, closing connection");
        conn->want_close = 1;
        return 0;
    }

    if (buf->len < 5 + user_len) return 0;
    if (buf->len < 5 + user_len + 4) return 0;

    uint32_t msg_len = 0;
    memcpy(&msg_len, buf->data + 5 + user_len, 4);
    msg_len = ntohl(msg_len);

    // OAUTH_LOGIN carries a JWT in this field, not a chat message -- give
    // it enough room for a real token but nowhere near the 32MB chat cap.
    uint32_t effective_max = (msg_type == MSG_OAUTH_LOGIN) ? MAX_JWT_LEN : MAX_MSG_LEN;
    if (msg_len > effective_max) {
        msg("message/token too long, closing connection");
        conn->want_close = 1;
        return 0;
    }

    size_t total_len = 5 + (size_t)user_len + 4 + (size_t)msg_len;
    if (buf->len < total_len) return 0;

    char *username_buf = (char *)malloc((size_t)user_len + 1);
    if (!username_buf) die("malloc(username_buf)");
    char *msg_buf = (char *)malloc((size_t)msg_len + 1);
    if (!msg_buf) { free(username_buf); die("malloc(msg_buf)"); }

    memcpy(username_buf, buf->data + 5, user_len);
    username_buf[user_len] = '\0';
    memcpy(msg_buf, buf->data + 5 + user_len + 4, msg_len);
    msg_buf[msg_len] = '\0';

    switch (msg_type) {
    case MSG_OAUTH_LOGIN: {
        char err[128];
        char display_name[MAX_USERNAME_LEN + 1];
        char user_id[USER_ID_LEN + 1];
        // Note: username_buf (the client-sent username field) is never
        // used here -- identity comes only from claims inside a verified
        // token, never from something the client typed alongside it.
        _Bool authenticated = g_dev_auth_enabled
            ? (msg_len == 0 && dev_identity_from_name(username_buf, display_name, sizeof(display_name),
                                                       user_id, sizeof(user_id)))
            : jwt_verify_and_extract(msg_buf, display_name, sizeof(display_name), user_id, sizeof(user_id), err, sizeof(err));
        if (authenticated) {
            Conn *existing = find_conn_by_user_id(user_id);
            if (existing && existing != conn) {
                snprintf(err, sizeof(err), "this account is already connected in another browser");
                printf("Rejected duplicate session for user ID: %s\n", user_id);
                queue_frame(conn, MSG_AUTH_FAIL, "", err);
                break;
            }
            strncpy(conn->username, display_name, MAX_USERNAME_LEN);
            conn->username[MAX_USERNAME_LEN] = '\0';
            strncpy(conn->user_id, user_id, USER_ID_LEN);
            conn->user_id[USER_ID_LEN] = '\0';
            if (!db_profile_upsert(conn->user_id, conn->username)) {
                fprintf(stderr, "could not persist nickname for user %s\n", conn->user_id);
            }
            strcpy(conn->room, "lobby");
            conn->has_username = 1;
            printf("%s authenticated via Supabase OAuth (ID: %s)\n", conn->username, conn->user_id);
            // Register in the global presence hash so all server instances
            // can see this user in their HGETALL broadcast_users_list() calls.
            db_room_ensure(conn->room, conn->user_id);
            redis_presence_set(conn->room, conn->user_id, conn->username);
            queue_frame(conn, MSG_AUTH_OK, conn->username, "");
            broadcast_room(conn->room, conn, MSG_JOIN, conn->username, "");
            broadcast_users_list(conn->room);
            db_send_history(conn);
            send_game_history(conn);
            send_room_metadata(conn, MSG_ROOM_JOIN);
            char *rooms = db_room_list_json();
            if (rooms) {
                queue_frame(conn, MSG_ROOM_LIST, "", rooms);
                free(rooms);
            }
        } else {
            if (g_dev_auth_enabled) snprintf(err, sizeof(err), "development mode requires a 1-48 character name");
            printf("OAuth login failed: %s\n", err);
            queue_frame(conn, MSG_AUTH_FAIL, "", err);
        }
        break;
    }
    case MSG_JOIN: {
        // A client should never send this -- see the enum comment. Most
        // likely an old pre-auth client (e.g. an outdated build). Reject
        // clearly instead of silently misinterpreting it.
        queue_frame(conn, MSG_AUTH_FAIL, username_buf, "authentication required: use OAUTH_LOGIN");
        conn->want_close = 1;
        break;
    }
    case MSG_LEAVE: {
        if (conn->has_username) {
            printf("%s left (explicit LEAVE)\n", conn->username);
            game_cleanup_for_user(conn->user_id);
            redis_presence_del(conn->room, conn->user_id);
            broadcast_room(conn->room, conn, MSG_LEAVE, conn->username, "");
            conn->has_username = 0; // Clear it so broadcast_users_list doesn't see it
            broadcast_users_list(conn->room);
        }
        conn->want_close = 1;
        break;
    }
    case MSG_HISTORY_REQUEST: {
        if (!conn->has_username) {
            queue_frame(conn, MSG_AUTH_FAIL, "", "authentication required: use OAUTH_LOGIN");
            conn->want_close = 1;
            break;
        }
        if (strncasecmp(msg_buf, "after:", 6) == 0) {
            db_send_history_since(conn, atoll(msg_buf + 6));
        } else {
            int64_t before_id = atoll(msg_buf);
            db_send_history_ext(conn, before_id, MSG_HISTORY_RESPONSE);
        }
        break;
    }
    case MSG_GAME_HISTORY: {
        if (conn->has_username) send_game_history(conn);
        break;
    }
    case MSG_ROOM_LIST: {
        if (!conn->has_username) break;
        char *rooms = db_room_list_json();
        if (rooms) {
            queue_frame(conn, MSG_ROOM_LIST, "", rooms);
            free(rooms);
        }
        break;
    }
    case MSG_ROOM_JOIN: {
        if (!conn->has_username) {
            queue_frame(conn, MSG_AUTH_FAIL, "", "authentication required: use OAUTH_LOGIN");
            break;
        }
        if (!room_slug_valid(username_buf)) {
            queue_frame(conn, MSG_ROOM_JOIN, "", "{\"error\":\"room names use lowercase letters, digits, - and _ only\"}");
            break;
        }
        if (!default_room_slug(username_buf)) {
            queue_frame(conn, MSG_ROOM_JOIN, "", "{\"error\":\"that room is not part of the current five-room map\"}");
            break;
        }
        if (strcmp(conn->room, username_buf) == 0) {
            send_room_metadata(conn, MSG_ROOM_JOIN);
            break;
        }
        char old_room[ROOM_SLUG_LEN + 1];
        strncpy(old_room, conn->room, ROOM_SLUG_LEN);
        old_room[ROOM_SLUG_LEN] = '\0';
        redis_presence_del(old_room, conn->user_id);
        broadcast_room(old_room, conn, MSG_LEAVE, conn->username, "");
        broadcast_users_list(old_room);
        if (!db_room_ensure(username_buf, conn->user_id)) {
            queue_frame(conn, MSG_ROOM_JOIN, "", "{\"error\":\"could not create or open room\"}");
            break;
        }
        strncpy(conn->room, username_buf, ROOM_SLUG_LEN);
        conn->room[ROOM_SLUG_LEN] = '\0';
        redis_presence_set(conn->room, conn->user_id, conn->username);
        broadcast_room(conn->room, conn, MSG_JOIN, conn->username, "");
        broadcast_users_list(conn->room);
        send_room_metadata(conn, MSG_ROOM_JOIN);
        db_send_history(conn);
        send_game_history(conn);
        char *rooms = db_room_list_json();
        if (rooms) {
            queue_frame(conn, MSG_ROOM_LIST, "", rooms);
            free(rooms);
        }
        break;
    }
    case MSG_CHAOS_UPDATE: {
        if (!conn->has_username) break;
        cJSON *root = cJSON_ParseWithLength(msg_buf, (size_t)msg_len);
        if (!root) {
            queue_frame(conn, MSG_CHAOS_UPDATE, conn->room, "{\"error\":\"invalid chaos profile\"}");
            break;
        }
        ChaosConfig config = db_room_chaos(conn->room);
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (cJSON_IsBool(enabled)) config.enabled = cJSON_IsTrue(enabled);
        cJSON *auto_cycle = cJSON_GetObjectItemCaseSensitive(root, "autoCycle");
        if (cJSON_IsBool(auto_cycle)) config.auto_cycle = cJSON_IsTrue(auto_cycle);
        config.min_delay_ms = room_chaos_int(root, "minDelayMs", config.min_delay_ms, 0, 60000);
        config.max_delay_ms = room_chaos_int(root, "maxDelayMs", config.max_delay_ms, 0, 60000);
        config.reorder_window_ms = room_chaos_int(root, "reorderWindowMs", config.reorder_window_ms, 0, 60000);
        config.duplicate_percent = room_chaos_int(root, "duplicatePercent", config.duplicate_percent, 0, 100);
        config.cycle_min_ms = room_chaos_int(root, "cycleMinMs", config.cycle_min_ms, 5000, 600000);
        config.cycle_max_ms = room_chaos_int(root, "cycleMaxMs", config.cycle_max_ms, 5000, 600000);
        cJSON_Delete(root);
        if (config.max_delay_ms < config.min_delay_ms) config.max_delay_ms = config.min_delay_ms;
        if (config.cycle_max_ms < config.cycle_min_ms) config.cycle_max_ms = config.cycle_min_ms;
        if (!db_room_set_chaos(conn->room, conn->user_id, &config)) {
            queue_frame(conn, MSG_CHAOS_UPDATE, conn->room, "{\"error\":\"only this room's owner can change chaos controls\"}");
            break;
        }
        chaos_cycle_reset(conn->room, &config);
        broadcast_room_metadata(conn->room, MSG_CHAOS_UPDATE);
        break;
    }
    case MSG_CHALLENGE: {
        if (!conn->has_username) break;
        cJSON *root = cJSON_ParseWithLength(msg_buf, (size_t)msg_len);
        cJSON *action = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
        cJSON *challenge_id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
        if (!cJSON_IsString(action)) {
            queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"invalid challenge request\"}");
            if (root) cJSON_Delete(root);
            break;
        }
        if (strcmp(action->valuestring, "request") == 0) {
            Conn *target = find_conn_by_user_id_in_room(username_buf, conn->room);
            if (!target) {
                queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"player is not available in this room\"}");
            } else if (target == conn) {
                queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"you cannot challenge yourself\"}");
            } else {
                Challenge *challenge = create_challenge(conn, target);
                if (!challenge) {
                    queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"could not create challenge\"}");
                } else {
                    send_challenge_event(conn, "requested", challenge);
                    send_challenge_event(target, "incoming", challenge);
                }
            }
        } else if (cJSON_IsString(challenge_id)) {
            Challenge *challenge = find_challenge(challenge_id->valuestring);
            if (!challenge || strcmp(challenge->room, conn->room) != 0) {
                queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"challenge expired or unavailable\"}");
            } else if (strcmp(action->valuestring, "accept") == 0 && strcmp(challenge->target_id, conn->user_id) == 0) {
                Conn *host = find_conn_by_user_id(challenge->requester_id);
                GameSession *session = host ? start_game_from_challenge(challenge) : NULL;
                if (!host || !session) {
                    queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"challenger is no longer available\"}");
                } else {
                    send_game_started(host, session, 1);
                    send_game_started(conn, session, 0);
                    remove_challenge(challenge);
                }
            } else if (strcmp(action->valuestring, "decline") == 0 && strcmp(challenge->target_id, conn->user_id) == 0) {
                Conn *requester = find_conn_by_user_id(challenge->requester_id);
                if (requester) send_challenge_event(requester, "declined", challenge);
                remove_challenge(challenge);
            } else if (strcmp(action->valuestring, "cancel") == 0 && strcmp(challenge->requester_id, conn->user_id) == 0) {
                Conn *target = find_conn_by_user_id(challenge->target_id);
                if (target) send_challenge_event(target, "cancelled", challenge);
                remove_challenge(challenge);
            } else {
                queue_frame(conn, MSG_CHALLENGE, "", "{\"error\":\"challenge action is not permitted\"}");
            }
        }
        if (root) cJSON_Delete(root);
        break;
    }
    case MSG_GAME_EVENT: {
        if (!conn->has_username) break;
        cJSON *root = cJSON_ParseWithLength(msg_buf, (size_t)msg_len);
        cJSON *game_id = root ? cJSON_GetObjectItemCaseSensitive(root, "gameId") : NULL;
        cJSON *kind = root ? cJSON_GetObjectItemCaseSensitive(root, "kind") : NULL;
        if (!cJSON_IsString(game_id) || !cJSON_IsString(kind)) {
            if (root) cJSON_Delete(root);
            break;
        }
        GameSession *session = find_game_session(game_id->valuestring);
        if (!session || strcmp(session->room, conn->room) != 0 || !game_has_user(session, conn->user_id)) {
            cJSON_Delete(root);
            break;
        }
        session->expires_at = time(NULL) + 20 * 60;
        _Bool sender_is_host = strcmp(session->host_id, conn->user_id) == 0;
        if (strcmp(kind->valuestring, "finish") == 0 && sender_is_host) {
            cJSON *host_score = cJSON_GetObjectItemCaseSensitive(root, "hostScore");
            cJSON *guest_score = cJSON_GetObjectItemCaseSensitive(root, "guestScore");
            // A single Pong game ends only when one player has exactly seven
            // points. This prevents a client from recording an unfinished or
            // tied game as a durable result.
            if (cJSON_IsNumber(host_score) && cJSON_IsNumber(guest_score) &&
                host_score->valueint >= 0 && host_score->valueint <= 7 &&
                guest_score->valueint >= 0 && guest_score->valueint <= 7 &&
                ((host_score->valueint == 7 && guest_score->valueint < 7) ||
                 (guest_score->valueint == 7 && host_score->valueint < 7))) {
                const char *winner_id = host_score->valueint == 7 ? session->host_id : session->guest_id;
                int64_t result_id = db_store_game_result(session, host_score->valueint, guest_score->valueint, winner_id);
                if (result_id > 0) {
                    cJSON *result = cJSON_CreateObject();
                    cJSON_AddNumberToObject(result, "id", (double)result_id);
                    cJSON_AddStringToObject(result, "gameId", session->id);
                    cJSON_AddStringToObject(result, "game", "pong-single");
                    cJSON_AddStringToObject(result, "room", session->room);
                    cJSON_AddStringToObject(result, "playerOne", session->host_name);
                    cJSON_AddNumberToObject(result, "playerOneScore", host_score->valueint);
                    cJSON_AddStringToObject(result, "playerTwo", session->guest_name);
                    cJSON_AddNumberToObject(result, "playerTwoScore", guest_score->valueint);
                    cJSON_AddStringToObject(result, "winner", strcmp(winner_id, session->host_id) == 0 ?
                        session->host_name : session->guest_name);
                    cJSON_AddNumberToObject(result, "createdAt", (double)time(NULL));
                    char *result_json = cJSON_PrintUnformatted(result);
                    cJSON_Delete(result);
                    if (result_json) {
                        broadcast_game_result(session->room, result_json);
                        redis_publish_game_result(session->room, result_json);
                        free(result_json);
                        g_metric_games_completed++;
                    }
                }
                remove_game_session(session);
            }
        } else if (strcmp(kind->valuestring, "quit") == 0) {
            Conn *opponent = game_opponent(session, conn);
            if (opponent) {
                cJSON_DeleteItemFromObjectCaseSensitive(root, "from");
                cJSON_AddStringToObject(root, "from", conn->user_id);
                char *event_json = cJSON_PrintUnformatted(root);
                if (event_json) { queue_frame(opponent, MSG_GAME_EVENT, conn->user_id, event_json); free(event_json); }
            }
            remove_game_session(session);
        } else if ((strcmp(kind->valuestring, "state") == 0 && sender_is_host) ||
                   (strcmp(kind->valuestring, "input") == 0 && !sender_is_host)) {
            Conn *opponent = game_opponent(session, conn);
            if (opponent) {
                cJSON_DeleteItemFromObjectCaseSensitive(root, "from");
                cJSON_AddStringToObject(root, "from", conn->user_id);
                char *event_json = cJSON_PrintUnformatted(root);
                if (event_json) { queue_frame(opponent, MSG_GAME_EVENT, conn->user_id, event_json); free(event_json); }
            }
        }
        cJSON_Delete(root);
        break;
    }
    case MSG_SIGNAL:
    case MSG_CALL_REJECT: {
        // WebRTC signaling relay.  The server never inspects the SDP or ICE
        // payload contents -- it only injects the verified caller identity
        // and forwards to the target connection.
        if (!conn->has_username) {
            queue_frame(conn, MSG_AUTH_FAIL, "", "authentication required: use OAUTH_LOGIN");
            conn->want_close = 1;
            break;
        }
        if (user_len == 0) {
            // Malformed frame: no target specified.
            break;
        }

        // username_buf carries the target's user_id (not their display name).
        // Reject attempts to call yourself.
        if (strcmp(username_buf, conn->user_id) == 0) {
            const char *self_err = "{\"kind\":\"error\",\"msg\":\"cannot signal yourself\"}";
            queue_frame(conn, MSG_SIGNAL, conn->user_id, self_err);
            break;
        }

        // For MSG_SIGNAL frames, parse the JSON payload and inject the
        // caller's verified user_id as the "from" field so the recipient
        // cannot be spoofed by the sender.
        char *out_payload = msg_buf;
        char *injected_json = NULL;
        if (msg_type == MSG_SIGNAL && msg_len > 0) {
            cJSON *root = cJSON_ParseWithLength(msg_buf, (size_t)msg_len);
            if (root) {
                // Remove any client-supplied "from" to prevent impersonation.
                cJSON_DeleteItemFromObjectCaseSensitive(root, "from");
                cJSON_AddStringToObject(root, "from", conn->user_id);
                injected_json = cJSON_PrintUnformatted(root);
                cJSON_Delete(root);
                if (injected_json) out_payload = injected_json;
            }
        }

        // Find the target and forward. O(N) scan is acceptable for <=256 conns.
        Conn *target = find_conn_by_user_id(username_buf);
        if (target) {
            // Forward: [username]=caller's user_id, [msg]=payload
            queue_frame(target, (uint8_t)msg_type, conn->user_id, out_payload);
        } else {
            // Target is not online -- notify the caller.
            const char *offline_err = "{\"kind\":\"error\",\"msg\":\"user not online\"}";
            queue_frame(conn, MSG_SIGNAL, "", offline_err);
        }

        if (injected_json) free(injected_json);
        break;
    }
    case MSG_CHAT:
    default: {
        if (!conn->has_username) {
            queue_frame(conn, MSG_AUTH_FAIL, "", "authentication required: use OAUTH_LOGIN");
            conn->want_close = 1;
            break;
        }

        // Per-user rate limiting: enforce RATE_LIMIT_MSGS messages per
        // RATE_LIMIT_WINDOW_SECS. A new 1-second window opens as soon as
        // the previous one expires, so short bursts are capped cleanly.
        time_t now = time(NULL);
        if (now - conn->rate_window_start >= RATE_LIMIT_WINDOW_SECS) {
            conn->rate_window_start = now;
            conn->rate_msg_count = 0;
        }
        if (conn->rate_msg_count >= RATE_LIMIT_MSGS) {
            // Silently drop rather than disconnect -- a single burst
            // shouldn't kick an otherwise well-behaved user.
            break;
        }
        conn->rate_msg_count++;

        // Use the server-side authenticated identity, NOT username_buf
        // (the username field the client put in this particular frame).
        // Trusting a per-message client-supplied username would let an
        // authenticated connection impersonate anyone in individual chat
        // messages -- auth only means something if every action after it
        // is tied back to the identity established at login.
        MessageStyle style = classify_message(msg_buf);
        printf("[%s] %s says [%s]: %s\n", conn->room, conn->username, message_style_name(style), msg_buf);
        int64_t msg_id = db_store_message(conn->room, conn->username, conn->user_id, msg_buf, style);
        if (msg_id == 0) {
            queue_frame(conn, MSG_CHAT_ACK, client_message_id_valid(username_buf) ? username_buf : "",
                        "{\"status\":\"failed\"}");
            break;
        }
        if (client_message_id_valid(username_buf)) {
            char ack[128];
            snprintf(ack, sizeof(ack), "{\"status\":\"persisted\",\"room\":\"%s\"}", conn->room);
            queue_frame_ext(conn, MSG_CHAT_ACK, (uint64_t)msg_id, (uint64_t)now, MESSAGE_STYLE_PLAIN,
                            username_buf, ack);
        }
        // Route through Redis Pub/Sub so all server instances (including this
        // one) receive the message via the subscriber callback. Falls back to
        // a direct broadcast_chat() call if Redis is not connected.
        redis_publish_chat(conn->room, msg_id, now, style, conn->username, msg_buf);
        // Flush any pending writes on the publisher connection now, before
        // poll() goes back to sleep. Without this, hiredis may buffer the
        // PUBLISH command until the next POLLOUT wake-up.
        redis_flush_pub();
        break;
    }
    }

    free(username_buf);
    free(msg_buf);

    dbuf_consume(buf, total_len);

    return (buf->len > 0);
}

// Application callback when the socket is readable
static void handle_read(Conn *conn) {
    conn->last_activity = time(NULL);
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv <= 0) {
        if (rv < 0 && errno != EAGAIN) msg("read() error");
        conn->want_close = 1;
        return;
    }

    dbuf_append(&conn->incoming, buf, (size_t)rv);

    // Decide transport on first bytes seen. 'G' as in "GET " signals an
    // HTTP upgrade request; anything else is assumed to be our legacy raw
    // framing (whose first byte is always a small integer 1/2/3, nothing
    // close to 'G' == 0x47).
    if (conn->mode == MODE_DETECT) {
        conn->mode = (conn->incoming.data[0] == 'G') ? MODE_WS_HANDSHAKE : MODE_RAW;
    }

    if (conn->mode == MODE_WS_HANDSHAKE) {
        try_ws_handshake(conn); // may advance mode to MODE_WS_ACTIVE
    }

    if (conn->mode == MODE_RAW) {
        while (try_one_request(conn, &conn->incoming)) {}
    } else if (conn->mode == MODE_WS_ACTIVE) {
        while (ws_try_decode_frame(conn)) {}
        while (try_one_request(conn, &conn->app_incoming)) {}
    }

    if (conn->outgoing.len > 0) {
        conn->want_read = 0;
        conn->want_write = 1;
        handle_write(conn);
    }
}

// Application callback when the socket is writable
static void handle_write(Conn *conn) {
    conn->last_activity = time(NULL);
    if (conn->outgoing.len == 0) return;

    ssize_t rv = write(conn->fd, conn->outgoing.data, conn->outgoing.len);
    if (rv < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Kernel send buffer is temporarily full. Leave want_write set
            // so epoll wakes us again when space is available -- this is a
            // normal transient condition, NOT a reason to close.
            conn->want_write = 1;
            return;
        }
        msg("write() error");
        conn->want_close = 1;
        return;
    }

    dbuf_consume(&conn->outgoing, (size_t)rv);

    if (conn->outgoing.len == 0) {
        conn->want_read = 1;
        conn->want_write = 0;
        // Buffer drained: remove EPOLLOUT so epoll_wait doesn't spin.
        conn_update_epoll(conn);
    }
}

// Application callback for new connections
static Conn *handle_accept(int fd) {
    struct sockaddr_in client_addr = {0};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        // EAGAIN/EWOULDBLOCK just means the queue emptied between poll()
        // and accept() -- not an error worth logging.
        if (errno != EAGAIN && errno != EWOULDBLOCK) msg("accept() error");
        return NULL;
    }

    // Hard cap: once we hit MAX_CONNECTIONS, refuse new ones. This closes
    // a trivial resource-exhaustion DoS (file-descriptor flood).
    if (g_conn_count >= MAX_CONNECTIONS) {
        msg("connection limit reached, refusing new connection");
        close(connfd);
        return NULL;
    }

    fd_set_nb(connfd);

    Conn *conn = (Conn *)malloc(sizeof(Conn));
    if (!conn) die("malloc");
    memset(conn, 0, sizeof(Conn)); // Initialize all fields to 0/NULL/false
    conn->fd = connfd;
    conn->want_read = 1;
    conn->mode = MODE_DETECT;
    conn->last_activity = time(NULL);

    conn_register(conn); // Add to the global table so broadcast() can reach it
    g_conn_count++;

    return conn;
}

// SQLite doesn't require an explicit clean shutdown to avoid corruption
// (it's WAL/journal-safe against crashes), but closing cleanly avoids
// leaving a stale -journal/-wal file around and is just good practice
// now that there's persistent state to think about.
static void handle_sigint(int signum) {
    (void)signum;
    // Cleanly disconnect Redis before exiting.
    if (g_redis_pub_ctx) { redisFree(g_redis_pub_ctx); g_redis_pub_ctx = NULL; }
    if (g_redis_sub_ctx) { redisFree(g_redis_sub_ctx); g_redis_sub_ctx = NULL; }
    if (g_db) sqlite3_close(g_db);
    _exit(0);
}

int main() {
    if (sodium_init() < 0) die("sodium_init");

    g_dev_auth_enabled = env_is_enabled(getenv("CHAT_DEV_AUTH"));
    if (g_dev_auth_enabled) {
        fprintf(stderr, "WARNING: CHAT_DEV_AUTH is enabled. OAuth is disabled; use only for local testing.\n");
    }

    // Load optional env vars. SUPABASE_JWKS is required (jwks_init aborts
    // if missing). CHAT_ALLOWED_ORIGIN is optional but strongly recommended
    // to prevent CSRF-over-WebSocket attacks; without it, any webpage the
    // user visits can silently connect to the chat server on their behalf.
    const char *allowed_origin_env = getenv("CHAT_ALLOWED_ORIGIN");
    if (allowed_origin_env && allowed_origin_env[0] != '\0') {
        strncpy(g_allowed_origin, allowed_origin_env, sizeof(g_allowed_origin) - 1);
        g_allowed_origin[sizeof(g_allowed_origin) - 1] = '\0';
        printf("WebSocket Origin restricted to: %s\n", g_allowed_origin);
    } else {
        fprintf(stderr,
            "WARNING: CHAT_ALLOWED_ORIGIN is not set. Any webpage can connect to\n"
            "this server on behalf of a logged-in user (CSRF-over-WebSocket).\n"
            "Set it to the URL serving the chat client, e.g.:\n"
            "  export CHAT_ALLOWED_ORIGIN=http://localhost:3000\n");
    }

    // Load optional issuer verification env. SUPABASE_ISSUER is recommended
    // to prevent verification of tokens issued by other Supabase projects.
    if (!g_dev_auth_enabled) {
        const char *issuer_env = getenv("SUPABASE_ISSUER");
        if (issuer_env && issuer_env[0] != '\0') {
            strncpy(g_expected_issuer, issuer_env, sizeof(g_expected_issuer) - 1);
            g_expected_issuer[sizeof(g_expected_issuer) - 1] = '\0';
            printf("JWT Issuer restricted to: %s\n", g_expected_issuer);
        } else {
            fprintf(stderr,
                "WARNING: SUPABASE_ISSUER is not set. Tokens issued by other Supabase\n"
                "projects using the same verification keys will be accepted.\n");
        }
        jwks_init();
    }
    chaos_init();

    // Load database path from env, falling back to DB_PATH_DEFAULT.
    const char *db_path_env = getenv("CHAT_DB_PATH");
    const char *db_path = db_path_env && db_path_env[0] != '\0' ? db_path_env : DB_PATH_DEFAULT;
    printf("Opening database file: %s\n", db_path);
    db_init(db_path);

    // Connect to Redis (optional). REDIS_URL accepts "host:port" (default:
    // 127.0.0.1:6379). The server starts in single-node mode if Redis is
    // unreachable and retries every REDIS_RECONNECT_INTERVAL_SECS seconds.
    const char *redis_url_env = getenv("REDIS_URL");
    if (redis_url_env && redis_url_env[0] != '\0') {
        // Parse "host:port".
        char url_buf[300];
        strncpy(url_buf, redis_url_env, sizeof(url_buf) - 1);
        url_buf[sizeof(url_buf) - 1] = '\0';
        char *colon = strrchr(url_buf, ':');
        if (colon) {
            *colon = '\0';
            strncpy(g_redis_host, url_buf, sizeof(g_redis_host) - 1);
            g_redis_host[sizeof(g_redis_host) - 1] = '\0';
            g_redis_port = atoi(colon + 1);
            if (g_redis_port <= 0 || g_redis_port > 65535) g_redis_port = 6379;
        } else {
            // No colon -- treat as host-only.
            strncpy(g_redis_host, url_buf, sizeof(g_redis_host) - 1);
            g_redis_host[sizeof(g_redis_host) - 1] = '\0';
        }
    }
    printf("Connecting to Redis at %s:%d\n", g_redis_host, g_redis_port);
    redis_connect();
    g_redis_last_connect_attempt = time(NULL);

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint); // clean SQLite shutdown on kill <pid> too
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    const char *bind_host = getenv("CHAT_BIND_HOST");
    if (!bind_host || !bind_host[0]) bind_host = "127.0.0.1";
    if (inet_pton(AF_INET, bind_host, &addr.sin_addr) != 1) {
        fprintf(stderr, "CHAT_BIND_HOST must be an IPv4 address: %s\n", bind_host);
        return 1;
    }
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr))) die("bind()");
    if (listen(fd, SOMAXCONN)) die("listen()");

    fd_set_nb(fd);

    // Initialise epoll.  The listening fd and the Redis subscriber fd (once
    // connected) are registered here; connection fds are registered inside
    // conn_register() as each new client arrives.
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) die("epoll_create1");

    // Add the listening socket to epoll.  We only ever need EPOLLIN on it.
    {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) die("epoll_ctl listen");
    }

    // Redis connected before epoll existed, so register its already-open
    // subscriber fd now. Later reconnects register themselves in
    // redis_connect(), where g_epoll_fd is already valid.
    if (g_redis_sub_fd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = g_redis_sub_fd };
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_redis_sub_fd, &ev) < 0) {
            die("epoll_ctl redis subscriber");
        }
    }

#define EPOLL_MAX_EVENTS 128

    // The Event Loop
    while (1) {
        // Publish anything that became eligible while the loop was handling
        // another event, then sleep only until the next queue deadline.
        chaos_cycle_tick();
        chaos_publish_due_messages();
        // epoll_wait: blocks up to 5000 ms, wakes only when fds are ready.
        // Unlike poll(), this is O(active events) not O(registered fds).
        struct epoll_event events[EPOLL_MAX_EVENTS];
        int nev = epoll_wait(g_epoll_fd, events, EPOLL_MAX_EVENTS, chaos_next_wait_ms(5000));
        if (nev < 0 && errno != EINTR) die("epoll_wait");

        // --- Pass 1: Service the Redis subscriber first ---
        // Messages arriving here may trigger broadcast_chat() which enqueues
        // data on client connections, so we handle them before client writes.
        for (int ei = 0; ei < nev; ++ei) {
            if (events[ei].data.fd == g_redis_sub_fd) {
                while (redis_try_read_message()) {}
            }
        }

        // --- Pass 2: New connections on the listening fd ---
        for (int ei = 0; ei < nev; ++ei) {
            if (events[ei].data.fd == fd) {
                handle_accept(fd); // conn_register() inside here adds fd to epoll
            }
        }

        // --- Pass 3: Service ready client sockets ---
        for (int ei = 0; ei < nev; ++ei) {
            int event_fd = events[ei].data.fd;
            uint32_t event_mask = events[ei].events;

            // The listener and Redis subscriber were handled in the earlier
            // passes. A descriptor can be reused after a Redis reconnect, so
            // compare against their current values on every iteration.
            if (event_fd == fd || event_fd == g_redis_sub_fd) continue;
            if (event_fd < 0 || (size_t)event_fd >= g_fd2conn_cap) continue;

            Conn *conn = g_fd2conn[event_fd];
            if (!conn) continue;

            if (event_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                conn->want_close = 1;
            }
            if ((event_mask & EPOLLIN) && !conn->want_close) {
                handle_read(conn);
            }
            if ((event_mask & EPOLLOUT) && !conn->want_close) {
                handle_write(conn);
            }
            if (conn->want_close) {
                _Bool was_auth = conn->has_username;
                char closed_room[ROOM_SLUG_LEN + 1];
                strncpy(closed_room, conn->room, ROOM_SLUG_LEN);
                closed_room[ROOM_SLUG_LEN] = '\0';
                if (was_auth) {
                    game_cleanup_for_user(conn->user_id);
                    redis_presence_del(conn->room, conn->user_id);
                    broadcast_room(conn->room, conn, MSG_LEAVE, conn->username, "");
                }
                epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                g_fd2conn[conn->fd] = NULL;
                g_conn_count--;
                conn_free(conn);
                if (was_auth) {
                    broadcast_users_list(closed_room);
                }
            }
        }

        // Periodic sweep: idle connection timeouts + Redis reconnect + presence heartbeat.
        time_t now = time(NULL);
        for (size_t i = 0; i < g_fd2conn_cap; ++i) {
            Conn *conn = g_fd2conn[i];
            if (!conn) continue;
            time_t timeout = conn->has_username ? IDLE_TIMEOUT_AUTH_SECS : IDLE_TIMEOUT_UNAUTH_SECS;
            if (now - conn->last_activity > timeout) {
                printf("fd %d timed out (idle %ld s), closing\n", conn->fd, (long)(now - conn->last_activity));
                _Bool was_auth = conn->has_username;
                char closed_room[ROOM_SLUG_LEN + 1];
                strncpy(closed_room, conn->room, ROOM_SLUG_LEN);
                closed_room[ROOM_SLUG_LEN] = '\0';
                if (was_auth) {
                    game_cleanup_for_user(conn->user_id);
                    redis_presence_del(conn->room, conn->user_id);
                    broadcast_room(conn->room, conn, MSG_LEAVE, conn->username, "");
                }
                epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                g_fd2conn[conn->fd] = NULL;
                g_conn_count--;
                conn_free(conn);
                if (was_auth) {
                    broadcast_users_list(closed_room);
                }
            }
        }

        // Redis reconnect: if either connection is down, retry every
        // REDIS_RECONNECT_INTERVAL_SECS seconds (single-node fallback stays
        // active in the meantime via redis_publish_chat's fallback path).
        if ((!g_redis_sub_ctx || !g_redis_pub_ctx) &&
            now - g_redis_last_connect_attempt >= REDIS_RECONNECT_INTERVAL_SECS) {
            fprintf(stderr, "[redis] attempting reconnect...\n");
            redis_connect();
            g_redis_last_connect_attempt = now;
        }

        // Presence heartbeat: reset the TTL on the presence hash so it
        // doesn't expire while authenticated users are still connected.
        if (now - g_presence_last_heartbeat >= PRESENCE_HEARTBEAT_SECS) {
            redis_presence_heartbeat();
            g_presence_last_heartbeat = now;
        }

        game_sweep_expired();

        chaos_cycle_tick();
        chaos_publish_due_messages();
    }

    return 0;
}
