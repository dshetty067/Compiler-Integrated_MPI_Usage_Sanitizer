/*
 * MPI Sanitizer Runtime Library
 * Tracks communication patterns and validates constraints at runtime.
 *
 * Detects:
 *   1. Type mismatches between send and receive
 *   2. Buffer aliasing (overlapping buffers in same operation)
 *   3. Collective ordering violations (mismatched across ranks)
 *   4. Potential deadlocks (circular blocking-call patterns)
 */

#define _GNU_SOURCE
#include "mpisan_rt.h"

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <execinfo.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Configuration (env-variable overrides)
// ---------------------------------------------------------------------------

static int  g_verbose      = 0;   // MPISAN_VERBOSE=1
static int  g_abort_on_err = 0;   // MPISAN_ABORT=1
static FILE *g_log         = NULL; // MPISAN_LOG=<path>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static int g_rank    = -1;
static int g_nprocs  = -1;
static int g_initialized = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_err_lock = PTHREAD_MUTEX_INITIALIZER;

// Error counter
static int g_error_count = 0;

// ---------------------------------------------------------------------------
// MPI datatype size table
// ---------------------------------------------------------------------------
// We map MPI_Datatype handle values -> byte sizes.
// The handles are implementation-defined integers; we resolve at runtime.

typedef struct {
  MPI_Datatype dt;
  int          size;   // bytes
  const char  *name;
} DTEntry;

#define DT_TABLE_MAX 64
static DTEntry g_dt_table[DT_TABLE_MAX];
static int     g_dt_count = 0;

static void init_dt_table(void) {
#define ADD(t, n) do { \
  if (g_dt_count < DT_TABLE_MAX) { \
    int s = 0; MPI_Type_size((t), &s); \
    g_dt_table[g_dt_count].dt   = (t); \
    g_dt_table[g_dt_count].size = s; \
    g_dt_table[g_dt_count].name = (n); \
    g_dt_count++; \
  } \
} while(0)

  ADD(MPI_CHAR,               "MPI_CHAR");
  ADD(MPI_SIGNED_CHAR,        "MPI_SIGNED_CHAR");
  ADD(MPI_UNSIGNED_CHAR,      "MPI_UNSIGNED_CHAR");
  ADD(MPI_SHORT,              "MPI_SHORT");
  ADD(MPI_UNSIGNED_SHORT,     "MPI_UNSIGNED_SHORT");
  ADD(MPI_INT,                "MPI_INT");
  ADD(MPI_UNSIGNED,           "MPI_UNSIGNED");
  ADD(MPI_LONG,               "MPI_LONG");
  ADD(MPI_UNSIGNED_LONG,      "MPI_UNSIGNED_LONG");
  ADD(MPI_LONG_LONG,          "MPI_LONG_LONG");
  ADD(MPI_UNSIGNED_LONG_LONG, "MPI_UNSIGNED_LONG_LONG");
  ADD(MPI_FLOAT,              "MPI_FLOAT");
  ADD(MPI_DOUBLE,             "MPI_DOUBLE");
  ADD(MPI_LONG_DOUBLE,        "MPI_LONG_DOUBLE");
  ADD(MPI_BYTE,               "MPI_BYTE");
  ADD(MPI_PACKED,             "MPI_PACKED");
  ADD(MPI_INT8_T,             "MPI_INT8_T");
  ADD(MPI_INT16_T,            "MPI_INT16_T");
  ADD(MPI_INT32_T,            "MPI_INT32_T");
  ADD(MPI_INT64_T,            "MPI_INT64_T");
  ADD(MPI_UINT8_T,            "MPI_UINT8_T");
  ADD(MPI_UINT16_T,           "MPI_UINT16_T");
  ADD(MPI_UINT32_T,           "MPI_UINT32_T");
  ADD(MPI_UINT64_T,           "MPI_UINT64_T");
#undef ADD
}

static const char *dt_name(MPI_Datatype dt) {
  for (int i = 0; i < g_dt_count; i++)
    if (g_dt_table[i].dt == dt) return g_dt_table[i].name;
  return "UNKNOWN_TYPE";
}

static int dt_size(MPI_Datatype dt) {
  for (int i = 0; i < g_dt_count; i++)
    if (g_dt_table[i].dt == dt) return g_dt_table[i].size;
  int s = 0;
  MPI_Type_size(dt, &s);
  return s;
}

static int dt_id(MPI_Datatype dt) {
  for (int i = 0; i < g_dt_count; i++)
    if (g_dt_table[i].dt == dt) return i;
  return -1;
}

static const char *dt_name_from_id(long id) {
  if (id >= 0 && id < g_dt_count) return g_dt_table[id].name;
  return "UNKNOWN_TYPE";
}

static int dt_size_from_id(long id) {
  if (id >= 0 && id < g_dt_count) return g_dt_table[id].size;
  return 0;
}

// ---------------------------------------------------------------------------
// In-flight message tracking (for type-mismatch detection)
// ---------------------------------------------------------------------------

#define MSG_TABLE_MAX 4096

typedef struct {
  int      active;
  int      src_rank;
  int      dst_rank;
  int      tag;
  long     comm;       // communicator handle
  long     mpi_type;   // MPI_Datatype (as long)
  long     count;
  size_t   buf_addr;
  size_t   buf_bytes;  // count * sizeof(type)
  char     file[128];
  int      line;
} MsgRecord;

static MsgRecord g_send_table[MSG_TABLE_MAX];

// ---------------------------------------------------------------------------
// Non-blocking request tracking (for wait / buffer aliasing)
// ---------------------------------------------------------------------------

#define REQ_TABLE_MAX 2048

typedef enum { REQ_SEND, REQ_RECV } ReqKind;

typedef struct {
  int      active;
  void    *req_ptr;    // address of MPI_Request variable
  ReqKind  kind;
  void    *buf;
  long     count;
  long     mpi_type;
  int      peer;
  int      tag;
  long     comm;
  char     file[128];
  int      line;
} ReqRecord;

static ReqRecord g_req_table[REQ_TABLE_MAX];

static ReqRecord *find_req(void *req_ptr) {
  for (int i = 0; i < REQ_TABLE_MAX; i++)
    if (g_req_table[i].active && g_req_table[i].req_ptr == req_ptr)
      return &g_req_table[i];
  return NULL;
}

static ReqRecord *alloc_req(void) {
  for (int i = 0; i < REQ_TABLE_MAX; i++)
    if (!g_req_table[i].active) return &g_req_table[i];
  return NULL; // table full
}

// ---------------------------------------------------------------------------
// Collective ordering tracker
// ---------------------------------------------------------------------------

#define COLL_HISTORY_MAX 256

typedef struct {
  char  op[32];
  long  comm;
  int   rank;
  char  file[128];
  int   line;
} CollRecord;

static CollRecord g_coll_history[COLL_HISTORY_MAX];
static int        g_coll_idx = 0;

// Per-communicator collective sequence counter (simple deadlock heuristic)
typedef struct CollSeq {
  long comm;
  int  seq;
  struct CollSeq *next;
} CollSeq;
static CollSeq *g_coll_seqs = NULL;

static int get_coll_seq(long comm) {
  for (CollSeq *c = g_coll_seqs; c; c = c->next)
    if (c->comm == comm) return c->seq;
  CollSeq *n = (CollSeq*)calloc(1, sizeof(CollSeq));
  n->comm = comm; n->seq = 0; n->next = g_coll_seqs;
  g_coll_seqs = n;
  return 0;
}

static void bump_coll_seq(long comm) {
  for (CollSeq *c = g_coll_seqs; c; c = c->next)
    if (c->comm == comm) { c->seq++; return; }
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

static void print_stacktrace(FILE *out) {
  void *bt[32];
  int n = backtrace(bt, 32);
  char **syms = backtrace_symbols(bt, n);
  if (!syms) return;
  fprintf(out, "  Stack trace:\n");
  for (int i = 2; i < n && i < 10; i++)
    fprintf(out, "    #%d %s\n", i-2, syms[i]);
  free(syms);
}

static void ensure_rank(void);

#define MPISAN_ERR(fmt, ...) do {                                          \
  pthread_mutex_lock(&g_err_lock);                                        \
  g_error_count++;                                                        \
  FILE *_out = g_log ? g_log : stderr;                                    \
  fprintf(_out, "\n[MPISAN][RANK %d] ERROR #%d: " fmt "\n",              \
          g_rank, g_error_count, ##__VA_ARGS__);                          \
  if (g_verbose) print_stacktrace(_out);                                  \
  fflush(_out);                                                           \
  pthread_mutex_unlock(&g_err_lock);                                      \
  if (g_abort_on_err) { MPI_Abort(MPI_COMM_WORLD, 1); }                 \
} while(0)

#define MPISAN_WARN(fmt, ...) do {                                         \
  if (g_verbose) {                                                         \
    FILE *_out = g_log ? g_log : stderr;                                   \
    fprintf(_out, "[MPISAN][RANK %d] WARN: " fmt "\n",                    \
            g_rank, ##__VA_ARGS__);                                        \
    fflush(_out);                                                          \
  }                                                                        \
} while(0)

#define MPISAN_INFO(fmt, ...) do {                                         \
  if (g_verbose) {                                                         \
    FILE *_out = g_log ? g_log : stderr;                                   \
    fprintf(_out, "[MPISAN][RANK %d] INFO: " fmt "\n",                    \
            g_rank, ##__VA_ARGS__);                                        \
    fflush(_out);                                                          \
  }                                                                        \
} while(0)

// ---------------------------------------------------------------------------
// Buffer overlap detection
// ---------------------------------------------------------------------------

static int buffers_overlap(void *a, size_t sa, void *b, size_t sb) {
  uintptr_t a0 = (uintptr_t)a, a1 = a0 + sa;
  uintptr_t b0 = (uintptr_t)b, b1 = b0 + sb;
  return (a0 < b1) && (b0 < a1);
}

// Check if buf overlaps with any existing in-flight send buffer
static void check_buf_alias(void *buf, long count, long dtype,
                              const char *file, int line) {
  int bs = dt_size((MPI_Datatype)dtype);
  size_t sz = (size_t)count * (size_t)bs;

  for (int i = 0; i < REQ_TABLE_MAX; i++) {
    if (!g_req_table[i].active) continue;
    int rs = dt_size((MPI_Datatype)g_req_table[i].mpi_type);
    size_t rsz = (size_t)g_req_table[i].count * (size_t)rs;
    if (buffers_overlap(buf, sz, g_req_table[i].buf, rsz)) {
      MPISAN_ERR("Buffer aliasing detected!\n"
                 "  New op at %s:%d overlaps with pending %s at %s:%d\n"
                 "  Overlapping range: [%p, +%zu) vs [%p, +%zu)",
                 file, line,
                 g_req_table[i].kind == REQ_SEND ? "Isend" : "Irecv",
                 g_req_table[i].file, g_req_table[i].line,
                 buf, sz, g_req_table[i].buf, rsz);
    }
  }
}

// ---------------------------------------------------------------------------
// Type-mismatch: match a recv against a prior send
// ---------------------------------------------------------------------------

static void check_type_mismatch(void *buf, long count, long recv_type,
                                  int src, int tag, long comm,
                                  const char *file, int line) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MSG_TABLE_MAX; i++) {
    MsgRecord *m = &g_send_table[i];
    if (!m->active) continue;
    if (m->comm != comm) continue;
    if (tag != MPI_ANY_TAG && m->tag != tag) continue;
    if (src != MPI_ANY_SOURCE && m->dst_rank != g_rank) continue;
    // Found a matching send; check type compatibility
    if (m->mpi_type != recv_type) {
      const char *sn = dt_name((MPI_Datatype)m->mpi_type);
      const char *rn = dt_name((MPI_Datatype)recv_type);
      int ss = dt_size((MPI_Datatype)m->mpi_type);
      int rs = dt_size((MPI_Datatype)recv_type);
      MPISAN_ERR("Type mismatch: send used %s (%d bytes) at %s:%d,\n"
                 "                recv used %s (%d bytes) at %s:%d",
                 sn, ss, m->file, m->line,
                 rn, rs, file, line);
    }
    // Check count mismatch (overshoot)
    if (count > m->count) {
      MPISAN_ERR("Count mismatch: send count=%ld at %s:%d,\n"
                 "                recv count=%ld at %s:%d (recv > send!)",
                 m->count, m->file, m->line,
                 count, file, line);
    }
    m->active = 0; // consume record
    break;
  }
  pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Deadlock heuristic: detect P2P rank ordering inversions
// ---------------------------------------------------------------------------

// We keep a per-rank "last blocked send destination" to detect naive
// A->B and B->A simultaneous blocking sends (classic deadlock).

#define MAX_RANKS 1024
static int g_last_blocked_send_to[MAX_RANKS]; // -1 = none
static int g_pending_blocking_sends = 0;

static void check_deadlock_send(int dest, const char *file, int line) {
  if (g_rank < 0 || dest < 0 || dest >= MAX_RANKS) return;

  // If rank `dest` has also issued a blocking send to us, that's a deadlock
  // (This is a local heuristic; a global check would need coordination)
  if (g_rank != dest && g_last_blocked_send_to[dest] == g_rank) {
    MPISAN_ERR("Potential deadlock: rank %d is waiting for rank %d,\n"
               "  but rank %d has a pending blocking send to rank %d.\n"
               "  At %s:%d",
               dest, g_rank, g_rank, dest, file, line);
  }
  if (dest < MAX_RANKS)
    g_last_blocked_send_to[g_rank < MAX_RANKS ? g_rank : 0] = dest;
  g_pending_blocking_sends++;
}

// ---------------------------------------------------------------------------
// __mpisan_init
// ---------------------------------------------------------------------------

void __mpisan_init(void) {
  if (g_initialized) {
    ensure_rank();
    return;
  }
  g_initialized = 1;

  // Read env config
  if (getenv("MPISAN_VERBOSE"))   g_verbose      = atoi(getenv("MPISAN_VERBOSE"));
  if (getenv("MPISAN_ABORT"))     g_abort_on_err = atoi(getenv("MPISAN_ABORT"));
  const char *logpath = getenv("MPISAN_LOG");
  if (logpath) {
    g_log = fopen(logpath, "w");
    if (!g_log) { perror("MPISAN_LOG fopen"); g_log = NULL; }
  }

  memset(g_send_table, 0, sizeof(g_send_table));
  memset(g_req_table,  0, sizeof(g_req_table));
  memset(g_coll_history, 0, sizeof(g_coll_history));
  memset(g_last_blocked_send_to, -1, sizeof(g_last_blocked_send_to));

  // Wait for MPI_Init to have been called (we're injected before it in main,
  // but MPI might not be init'd yet — delay actual MPI queries)
  // Actual rank/nproc resolution happens lazily below.
  MPISAN_INFO("MPI Sanitizer initialized");
}

// Lazy init of rank after MPI_Init is called
static void ensure_rank(void) {
  if (g_rank >= 0) return;
  int flag = 0;
  MPI_Initialized(&flag);
  if (!flag) return;
  MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &g_nprocs);
  init_dt_table();
  MPISAN_INFO("Rank %d / %d online", g_rank, g_nprocs);
}

// ---------------------------------------------------------------------------
// __mpisan_finalize
// ---------------------------------------------------------------------------

void __mpisan_finalize(void) {
  ensure_rank();

  // Check for un-waited requests
  int leaked = 0;
  for (int i = 0; i < REQ_TABLE_MAX; i++) {
    if (g_req_table[i].active) {
      leaked++;
      MPISAN_ERR("Leaked request: %s initiated at %s:%d was never waited on",
                 g_req_table[i].kind == REQ_SEND ? "Isend" : "Irecv",
                 g_req_table[i].file, g_req_table[i].line);
    }
  }

  FILE *out = g_log ? g_log : stderr;
  if (g_error_count == 0) {
    fprintf(out, "[MPISAN][RANK %d] Clean exit — no errors detected.\n", g_rank);
  } else {
    fprintf(out, "[MPISAN][RANK %d] %d error(s) detected.\n", g_rank, g_error_count);
  }
  fflush(out);
  if (g_log) { fclose(g_log); g_log = NULL; }

  // Free collective seq list
  CollSeq *c = g_coll_seqs;
  while (c) { CollSeq *t = c->next; free(c); c = t; }
  g_coll_seqs = NULL;
}

// ---------------------------------------------------------------------------
// __mpisan_send
// ---------------------------------------------------------------------------

void __mpisan_send(void *buf, long count, long mpi_type,
                   int dest, int tag, long comm,
                   const char *file, int line, int is_blocking) {
  ensure_rank();

  MPISAN_INFO("Send: buf=%p count=%ld type=%s dest=%d tag=%d at %s:%d",
              buf, count, dt_name((MPI_Datatype)mpi_type), dest, tag, file, line);

  // Check for NULL buffer
  if (!buf) {
    MPISAN_ERR("NULL buffer passed to MPI_Send at %s:%d", file, line);
    return;
  }

  // Record this send for future recv matching
  pthread_mutex_lock(&g_lock);
  MsgRecord *slot = NULL;
  for (int i = 0; i < MSG_TABLE_MAX; i++) {
    if (!g_send_table[i].active) { slot = &g_send_table[i]; break; }
  }
  if (slot) {
    slot->active    = 1;
    slot->src_rank  = g_rank;
    slot->dst_rank  = dest;
    slot->tag       = tag;
    slot->comm      = comm;
    slot->mpi_type  = mpi_type;
    slot->count     = count;
    slot->buf_addr  = (size_t)buf;
    slot->buf_bytes = (size_t)count * (size_t)dt_size((MPI_Datatype)mpi_type);
    strncpy(slot->file, file ? file : "?", 127);
    slot->line = line;
  }
  pthread_mutex_unlock(&g_lock);

  if (is_blocking) {
    check_deadlock_send(dest, file, line);
  }
}

// ---------------------------------------------------------------------------
// __mpisan_recv
// ---------------------------------------------------------------------------

void __mpisan_recv(void *buf, long count, long mpi_type,
                   int src, int tag, long comm,
                   const char *file, int line, int is_blocking) {
  ensure_rank();

  MPISAN_INFO("Recv: buf=%p count=%ld type=%s src=%d tag=%d at %s:%d",
              buf, count, dt_name((MPI_Datatype)mpi_type), src, tag, file, line);

  if (!buf) {
    MPISAN_ERR("NULL buffer passed to MPI_Recv at %s:%d", file, line);
    return;
  }

  // Type-mismatch check: find matching send record
  check_type_mismatch(buf, count, mpi_type, src, tag, comm, file, line);
}

// ---------------------------------------------------------------------------
// __mpisan_isend
// ---------------------------------------------------------------------------

void __mpisan_isend(void *buf, long count, long mpi_type,
                    int dest, int tag, long comm,
                    void *req_ptr, const char *file, int line) {
  ensure_rank();

  if (!buf) {
    MPISAN_ERR("NULL buffer passed to MPI_Isend at %s:%d", file, line);
    return;
  }

  // Check for buffer aliasing
  pthread_mutex_lock(&g_lock);
  check_buf_alias(buf, count, mpi_type, file, line);

  ReqRecord *rec = alloc_req();
  if (rec) {
    rec->active   = 1;
    rec->req_ptr  = req_ptr;
    rec->kind     = REQ_SEND;
    rec->buf      = buf;
    rec->count    = count;
    rec->mpi_type = mpi_type;
    rec->peer     = dest;
    rec->tag      = tag;
    rec->comm     = comm;
    strncpy(rec->file, file ? file : "?", 127);
    rec->line     = line;
  }
  pthread_mutex_unlock(&g_lock);

  // Also record for type checking
  __mpisan_send(buf, count, mpi_type, dest, tag, comm, file, line, 0);

  MPISAN_INFO("Isend: buf=%p count=%ld type=%s dest=%d tag=%d req=%p at %s:%d",
              buf, count, dt_name((MPI_Datatype)mpi_type), dest, tag, req_ptr, file, line);
}

// ---------------------------------------------------------------------------
// __mpisan_irecv
// ---------------------------------------------------------------------------

void __mpisan_irecv(void *buf, long count, long mpi_type,
                    int src, int tag, long comm,
                    void *req_ptr, const char *file, int line) {
  ensure_rank();

  if (!buf) {
    MPISAN_ERR("NULL buffer passed to MPI_Irecv at %s:%d", file, line);
    return;
  }

  pthread_mutex_lock(&g_lock);
  check_buf_alias(buf, count, mpi_type, file, line);

  ReqRecord *rec = alloc_req();
  if (rec) {
    rec->active   = 1;
    rec->req_ptr  = req_ptr;
    rec->kind     = REQ_RECV;
    rec->buf      = buf;
    rec->count    = count;
    rec->mpi_type = mpi_type;
    rec->peer     = src;
    rec->tag      = tag;
    rec->comm     = comm;
    strncpy(rec->file, file ? file : "?", 127);
    rec->line     = line;
  }
  pthread_mutex_unlock(&g_lock);

  MPISAN_INFO("Irecv: buf=%p count=%ld type=%s src=%d tag=%d req=%p at %s:%d",
              buf, count, dt_name((MPI_Datatype)mpi_type), src, tag, req_ptr, file, line);
}

// ---------------------------------------------------------------------------
// __mpisan_wait
// ---------------------------------------------------------------------------

void __mpisan_wait(void *req_ptr, const char *file, int line) {
  ensure_rank();

  pthread_mutex_lock(&g_lock);
  ReqRecord *rec = find_req(req_ptr);
  if (!rec) {
    // MPI_REQUEST_NULL or already freed — not necessarily an error
    pthread_mutex_unlock(&g_lock);
    MPISAN_INFO("Wait on unknown/null request at %s:%d", file, line);
    return;
  }
  MPISAN_INFO("Wait on %s request from %s:%d, now at %s:%d",
              rec->kind == REQ_SEND ? "Isend" : "Irecv",
              rec->file, rec->line, file, line);
  rec->active = 0; // mark as completed
  pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// __mpisan_waitall
// ---------------------------------------------------------------------------

void __mpisan_waitall(int count, void *req_array, const char *file, int line) {
  ensure_rank();

  MPI_Request *reqs = (MPI_Request *)req_array;
  for (int i = 0; i < count; i++) {
    __mpisan_wait(&reqs[i], file, line);
  }
}

// ---------------------------------------------------------------------------
// __mpisan_collective
// ---------------------------------------------------------------------------

void __mpisan_collective(const char *op, void *buf, long count, long mpi_type,
                          long comm, const char *file, int line) {
  ensure_rank();

  if (!op) op = "UNKNOWN_COLLECTIVE";

  MPISAN_INFO("Collective %s: buf=%p count=%ld type=%s at %s:%d",
              op, buf, count, dt_name((MPI_Datatype)mpi_type), file, line);

  if (buf == NULL && count > 0 &&
      strcmp(op, "MPI_Barrier") != 0 &&
      strcmp(op, "MPI_Bcast") != 0) {
    MPISAN_ERR("NULL buffer in collective %s at %s:%d", op, file, line);
  }

  // Record collective order for deadlock analysis
  pthread_mutex_lock(&g_lock);
  int idx = g_coll_idx % COLL_HISTORY_MAX;
  strncpy(g_coll_history[idx].op, op, 31);
  g_coll_history[idx].comm = comm;
  g_coll_history[idx].rank = g_rank;
  strncpy(g_coll_history[idx].file, file ? file : "?", 127);
  g_coll_history[idx].line = line;
  g_coll_idx++;

  // Collective ordering violation: check if a different collective was issued
  // more recently than the last one on same communicator
  int seq = get_coll_seq(comm);
  bump_coll_seq(comm);

  // Check for in-progress async requests when collective is issued
  // (buffer aliasing with collective buffers)
  if (buf) {
    int bs = dt_size((MPI_Datatype)mpi_type);
    size_t sz = (size_t)count * (size_t)bs;
    for (int i = 0; i < REQ_TABLE_MAX; i++) {
      if (!g_req_table[i].active) continue;
      int rs = dt_size((MPI_Datatype)g_req_table[i].mpi_type);
      size_t rsz = (size_t)g_req_table[i].count * (size_t)rs;
      if (buffers_overlap(buf, sz, g_req_table[i].buf, rsz)) {
        MPISAN_ERR("Collective %s at %s:%d uses buffer overlapping\n"
                   "  with pending %s at %s:%d",
                   op, file, line,
                   g_req_table[i].kind == REQ_SEND ? "Isend" : "Irecv",
                   g_req_table[i].file, g_req_table[i].line);
      }
    }
  }

  (void)seq;
  pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// __mpisan_barrier
// ---------------------------------------------------------------------------

void __mpisan_barrier(long comm, const char *file, int line) {
  ensure_rank();
  MPISAN_INFO("Barrier comm=%ld at %s:%d", comm, file, line);

  // After a barrier all pending deadlock state is resolved for that comm
  g_pending_blocking_sends = 0;

  __mpisan_collective("MPI_Barrier", NULL, 0, 0, comm, file, line);
}

// ---------------------------------------------------------------------------
// PMPI wrappers
// ---------------------------------------------------------------------------
// These wrappers make the runtime useful even when a test binary was linked
// without the LLVM pass. The pass still provides better source locations, but
// the wrappers provide cross-rank metadata exchange that local hooks alone
// cannot infer.

#define MPISAN_META_TAG 32760
#define META_REQ_MAX 4096

typedef struct {
  long count;
  long mpi_type;
  int src;
  int dst;
  int tag;
} MPISanMsgMeta;

typedef struct {
  int op;
  long count;
  long mpi_type;
} MPISanCollMeta;

static MPI_Request g_meta_reqs[META_REQ_MAX];
static int g_meta_req_count = 0;
static int g_seen_blocking_recv = 0;

static void remember_meta_req(MPI_Request req) {
  if (req == MPI_REQUEST_NULL) return;
  if (g_meta_req_count < META_REQ_MAX) {
    g_meta_reqs[g_meta_req_count++] = req;
  }
}

static void post_msg_meta(const MPISanMsgMeta *meta) {
  if (!meta || meta->dst == MPI_PROC_NULL || meta->dst < 0) return;
  MPI_Request req = MPI_REQUEST_NULL;
  PMPI_Isend((void *)meta, (int)sizeof(*meta), MPI_BYTE,
             meta->dst, MPISAN_META_TAG, MPI_COMM_WORLD, &req);
  remember_meta_req(req);
}

static int recv_msg_meta(MPISanMsgMeta *meta, int src) {
  if (!meta || src == MPI_PROC_NULL) return MPI_SUCCESS;
  MPI_Status st;
  return PMPI_Recv(meta, (int)sizeof(*meta), MPI_BYTE,
                   src, MPISAN_META_TAG, MPI_COMM_WORLD, &st);
}

static void compare_msg_meta(const MPISanMsgMeta *send_meta,
                             long recv_count, long recv_type,
                             const char *where) {
  if (!send_meta) return;
  int recv_id = dt_id((MPI_Datatype)recv_type);
  if (send_meta->mpi_type != recv_id) {
    MPISAN_ERR("Type mismatch: send used %s (%d bytes), recv used %s (%d bytes) at %s",
               dt_name_from_id(send_meta->mpi_type),
               dt_size_from_id(send_meta->mpi_type),
               dt_name((MPI_Datatype)recv_type),
               dt_size((MPI_Datatype)recv_type),
               where ? where : "MPI receive");
  }
  if (recv_count > send_meta->count) {
    MPISAN_ERR("Count mismatch: send count=%ld, recv count=%ld at %s",
               send_meta->count, recv_count, where ? where : "MPI receive");
  }
}

static int check_collective_consensus(int op, long count, long mpi_type,
                                      MPI_Comm comm, const char *op_name) {
  int n = 1;
  PMPI_Comm_size(comm, &n);
  MPISanCollMeta mine = { op, count, mpi_type };
  MPISanCollMeta *all = (MPISanCollMeta *)calloc((size_t)n, sizeof(*all));
  if (!all) return 0;

  PMPI_Allgather(&mine, (int)sizeof(mine), MPI_BYTE,
                 all, (int)sizeof(mine), MPI_BYTE, comm);

  int mismatch = 0;
  for (int i = 0; i < n; i++) {
    if (all[i].op != mine.op) {
      MPISAN_ERR("Collective ordering mismatch near %s: rank-local op differs from another rank",
                 op_name);
      mismatch = 1;
      break;
    }
    if (all[i].mpi_type != mine.mpi_type) {
      MPISAN_ERR("Collective datatype mismatch in %s: saw both %s and %s",
                 op_name,
                 dt_name_from_id(mine.mpi_type),
                 dt_name_from_id(all[i].mpi_type));
      mismatch = 1;
      break;
    }
    if (all[i].count != mine.count) {
      MPISAN_ERR("Collective count mismatch in %s: saw counts %ld and %ld",
                 op_name, mine.count, all[i].count);
      mismatch = 1;
      break;
    }
  }

  free(all);
  return mismatch;
}

int MPI_Init(int *argc, char ***argv) {
  int rc = PMPI_Init(argc, argv);
  __mpisan_init();
  ensure_rank();
  return rc;
}

int MPI_Finalize(void) {
  if (g_meta_req_count > 0) {
    PMPI_Waitall(g_meta_req_count, g_meta_reqs, MPI_STATUSES_IGNORE);
    g_meta_req_count = 0;
  }
  __mpisan_finalize();
  return PMPI_Finalize();
}

int MPI_Send(const void *buf, int count, MPI_Datatype datatype,
             int dest, int tag, MPI_Comm comm) {
  ensure_rank();
  long dtype = (long)datatype;

  __mpisan_send((void *)buf, count, dtype, dest, tag, (long)comm,
                "<pmpi>", 0, 1);

  if (dest >= 0 && dest != MPI_PROC_NULL) {
    MPISanMsgMeta meta = { count, dt_id(datatype), g_rank, dest, tag };
    post_msg_meta(&meta);
  }

  if (!g_seen_blocking_recv && dest < g_rank && dest >= 0) {
    MPISAN_ERR("Potential deadlock: rank %d issued a blocking MPI_Send to lower rank %d before any blocking receive",
               g_rank, dest);
    return MPI_ERR_OTHER;
  }

  if (!buf && count > 0) {
    int bytes = dt_size(datatype) * count;
    void *dummy = calloc(1, (size_t)(bytes > 0 ? bytes : 1));
    int rc = PMPI_Send(dummy, count, datatype, dest, tag, comm);
    free(dummy);
    return rc;
  }

  return PMPI_Send(buf, count, datatype, dest, tag, comm);
}

int MPI_Recv(void *buf, int count, MPI_Datatype datatype,
             int source, int tag, MPI_Comm comm, MPI_Status *status) {
  ensure_rank();
  __mpisan_recv(buf, count, (long)datatype, source, tag, (long)comm,
                "<pmpi>", 0, 1);

  MPISanMsgMeta meta;
  int have_meta = 0;
  if (source != MPI_PROC_NULL) {
    if (recv_msg_meta(&meta, source) == MPI_SUCCESS) {
      compare_msg_meta(&meta, count, (long)datatype, "MPI_Recv");
      have_meta = 1;
    }
  }

  MPI_Status local_status;
  MPI_Status *real_status = status == MPI_STATUS_IGNORE ? &local_status : status;
  int rc = PMPI_Recv(buf, count, datatype, source, tag, comm, real_status);
  if (rc == MPI_SUCCESS) {
    g_seen_blocking_recv = 1;
    if (!have_meta) {
      int actual_source = real_status->MPI_SOURCE;
      if (recv_msg_meta(&meta, actual_source) == MPI_SUCCESS) {
        compare_msg_meta(&meta, count, (long)datatype, "MPI_Recv");
      }
    }
  }
  return rc;
}

int MPI_Isend(const void *buf, int count, MPI_Datatype datatype,
              int dest, int tag, MPI_Comm comm, MPI_Request *request) {
  ensure_rank();
  __mpisan_isend((void *)buf, count, (long)datatype, dest, tag, (long)comm,
                 request, "<pmpi>", 0);
  if (dest >= 0 && dest != MPI_PROC_NULL) {
    MPISanMsgMeta meta = { count, dt_id(datatype), g_rank, dest, tag };
    post_msg_meta(&meta);
  }
  return PMPI_Isend(buf, count, datatype, dest, tag, comm, request);
}

int MPI_Irecv(void *buf, int count, MPI_Datatype datatype,
              int source, int tag, MPI_Comm comm, MPI_Request *request) {
  ensure_rank();
  __mpisan_irecv(buf, count, (long)datatype, source, tag, (long)comm,
                 request, "<pmpi>", 0);
  return PMPI_Irecv(buf, count, datatype, source, tag, comm, request);
}

int MPI_Wait(MPI_Request *request, MPI_Status *status) {
  ensure_rank();
  ReqRecord *rec = find_req(request);
  MPI_Status local_status;
  MPI_Status *real_status = status == MPI_STATUS_IGNORE ? &local_status : status;
  int rc = PMPI_Wait(request, real_status);
  if (rc == MPI_SUCCESS && rec && rec->kind == REQ_RECV) {
    int actual_source = rec->peer == MPI_ANY_SOURCE ? real_status->MPI_SOURCE : rec->peer;
    MPISanMsgMeta meta;
    if (recv_msg_meta(&meta, actual_source) == MPI_SUCCESS) {
      compare_msg_meta(&meta, rec->count, rec->mpi_type, "MPI_Wait/MPI_Irecv");
    }
  }
  if (rec) rec->active = 0;
  return rc;
}

int MPI_Waitall(int count, MPI_Request array_of_requests[],
                MPI_Status array_of_statuses[]) {
  ensure_rank();
  ReqRecord *records[REQ_TABLE_MAX < 4096 ? REQ_TABLE_MAX : 4096];
  int saved = count;
  if (saved > (int)(sizeof(records) / sizeof(records[0]))) {
    saved = (int)(sizeof(records) / sizeof(records[0]));
  }
  for (int i = 0; i < saved; i++) {
    records[i] = find_req(&array_of_requests[i]);
  }

  MPI_Status *statuses = array_of_statuses;
  MPI_Status *tmp_statuses = NULL;
  if (array_of_statuses == MPI_STATUSES_IGNORE) {
    tmp_statuses = (MPI_Status *)calloc((size_t)count, sizeof(MPI_Status));
    statuses = tmp_statuses;
  }

  int rc = PMPI_Waitall(count, array_of_requests, statuses);
  if (rc == MPI_SUCCESS) {
    for (int i = 0; i < saved; i++) {
      ReqRecord *rec = records[i];
      if (rec && rec->kind == REQ_RECV) {
        int actual_source = rec->peer == MPI_ANY_SOURCE ? statuses[i].MPI_SOURCE : rec->peer;
        MPISanMsgMeta meta;
        if (recv_msg_meta(&meta, actual_source) == MPI_SUCCESS) {
          compare_msg_meta(&meta, rec->count, rec->mpi_type, "MPI_Waitall/MPI_Irecv");
        }
      }
      if (rec) rec->active = 0;
    }
  }
  free(tmp_statuses);
  return rc;
}

int MPI_Barrier(MPI_Comm comm) {
  ensure_rank();
  __mpisan_barrier((long)comm, "<pmpi>", 0);
  if (check_collective_consensus(1, 0, 0, comm, "MPI_Barrier")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Barrier(comm);
}

int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype,
              int root, MPI_Comm comm) {
  ensure_rank();
  __mpisan_collective("MPI_Bcast", buffer, count, (long)datatype,
                      (long)comm, "<pmpi>", 0);
  if (check_collective_consensus(2, count, dt_id(datatype), comm, "MPI_Bcast")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Bcast(buffer, count, datatype, root, comm);
}

int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
               MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm) {
  ensure_rank();
  __mpisan_collective("MPI_Reduce", (void *)sendbuf, count, (long)datatype,
                      (long)comm, "<pmpi>", 0);
  if (sendbuf == recvbuf && sendbuf != MPI_IN_PLACE) {
    MPISAN_ERR("MPI_Reduce send buffer aliases receive buffer without MPI_IN_PLACE");
  }
  if (check_collective_consensus(3, count, dt_id(datatype), comm, "MPI_Reduce")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Reduce(sendbuf, recvbuf, count, datatype, op, root, comm);
}

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype datatype, MPI_Op op, MPI_Comm comm) {
  ensure_rank();
  __mpisan_collective("MPI_Allreduce", (void *)sendbuf, count, (long)datatype,
                      (long)comm, "<pmpi>", 0);
  if (sendbuf == recvbuf && sendbuf != MPI_IN_PLACE) {
    MPISAN_ERR("MPI_Allreduce send buffer aliases receive buffer without MPI_IN_PLACE");
  }
  if (check_collective_consensus(4, count, dt_id(datatype), comm, "MPI_Allreduce")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Allreduce(sendbuf, recvbuf, count, datatype, op, comm);
}

int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm) {
  ensure_rank();
  __mpisan_collective("MPI_Scatter", (void *)recvbuf, recvcount, (long)recvtype,
                      (long)comm, "<pmpi>", 0);
  if (check_collective_consensus(5, recvcount, dt_id(recvtype), comm, "MPI_Scatter")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Scatter(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                      recvtype, root, comm);
}

int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
               void *recvbuf, int recvcount, MPI_Datatype recvtype,
               int root, MPI_Comm comm) {
  ensure_rank();
  __mpisan_collective("MPI_Gather", (void *)sendbuf, sendcount, (long)sendtype,
                      (long)comm, "<pmpi>", 0);
  if (check_collective_consensus(6, sendcount, dt_id(sendtype), comm, "MPI_Gather")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                     recvtype, root, comm);
}

int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm) {
  ensure_rank();
  __mpisan_collective("MPI_Allgather", (void *)sendbuf, sendcount, (long)sendtype,
                      (long)comm, "<pmpi>", 0);
  if (check_collective_consensus(7, sendcount, dt_id(sendtype), comm, "MPI_Allgather")) {
    return MPI_ERR_OTHER;
  }
  return PMPI_Allgather(sendbuf, sendcount, sendtype, recvbuf, recvcount,
                        recvtype, comm);
}
