#ifndef MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include "meiso_drm_single_plane_test_only_private.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_K230D_DRM_SUPERVISOR_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_K230D_DRM_SUPERVISOR_PRIVATE_API
#endif

enum {
    MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES = 32,
    MEISO_K230D_DRM_SUPERVISOR_NONCE_BYTES = 32,
    MEISO_K230D_DRM_SUPERVISOR_MAX_SIGNAL_NUMBER = 127,
    MEISO_K230D_DRM_SUPERVISOR_READY_BYTE = 0x52
};

#define MEISO_K230D_DRM_SUPERVISOR_TERM_AFTER_NS UINT64_C(15000000000)
#define MEISO_K230D_DRM_SUPERVISOR_KILL_AFTER_NS UINT64_C(17000000000)
#define MEISO_K230D_DRM_SUPERVISOR_QUARANTINE_AFTER_NS UINT64_C(20000000000)

/*
 * Callback-owned discriminators and flags use only fixed-width unsigned
 * storage. This lets the portable core seed every output byte with a canary,
 * reject an out-of-domain value, and compare a failed callback's bytes without
 * ever reading an invalid C enum or _Bool representation.
 */
typedef uint8_t MeisoK230dDrmSupervisorFailure;
enum {
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_NONE = 0,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_INITIAL_PREFLIGHT = 1,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_GLOBAL_CLAIM = 2,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_EVIDENCE_CLAIM = 3,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_SPAWN = 4,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_CHILD_GATE = 5,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_PRE_RELEASE_PREFLIGHT = 6,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_READY_RECORD = 7,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_RELEASE_GATE = 8,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_CLOCK = 9,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_WAIT = 10,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_STOP_REQUESTED = 11,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_DEADLINE_EXPIRED = 12,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_TERM = 13,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_KILL = 14,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_QUARANTINE_RECORD = 15,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_POSTFLIGHT = 16,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_TERMINAL_RECORD = 17,
    MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL = 18
};

typedef uint8_t MeisoK230dDrmSupervisorOutcome;
enum {
    MEISO_K230D_DRM_SUPERVISOR_OUTCOME_UNSET = 0,
    MEISO_K230D_DRM_SUPERVISOR_OUTCOME_NO_CHILD_FAILURE = 1,
    MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_WITHOUT_SIGNAL_CALL = 2,
    MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_TERM_CALL = 3,
    MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_KILL_CALL = 4,
    MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_QUARANTINE_ATTEMPT = 5
};

typedef uint8_t MeisoK230dDrmSupervisorInvokeStatus;
enum {
    MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE = 0,
    MEISO_K230D_DRM_SUPERVISOR_INVALID_ARGUMENT = 1,
    MEISO_K230D_DRM_SUPERVISOR_INVALID_CONFIG = 2
};

typedef uint8_t MeisoK230dDrmSupervisorDurableDisposition;
enum {
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_UNSET = 0,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED = 1,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_ALREADY_EXISTS = 2,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_FAILED_CLEAN = 3,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN = 4
};

typedef uint8_t MeisoK230dDrmSupervisorDurableProgress;
enum {
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNSET = 0,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE = 1,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_PRIVATE_BODY_CREATED = 2,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_BODY_SYNCED = 3,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNIQUE_NAME_PUBLISHED = 4,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_PARENT_DIRECTORY_SYNCED = 5,
    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN = 6
};

typedef uint8_t MeisoK230dDrmSupervisorGateKind;
enum {
    MEISO_K230D_DRM_SUPERVISOR_GATE_UNSET = 0,
    MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED = 1,
    MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED = 2,
    MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_SIGNALED = 3,
    MEISO_K230D_DRM_SUPERVISOR_GATE_INTERRUPTED = 4,
    MEISO_K230D_DRM_SUPERVISOR_GATE_STOP_REQUESTED = 5
};

typedef uint8_t MeisoK230dDrmSupervisorReleaseKind;
enum {
    MEISO_K230D_DRM_SUPERVISOR_RELEASE_UNSET = 0,
    MEISO_K230D_DRM_SUPERVISOR_RELEASE_NOT_RELEASED = 1,
    MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITH_POST_CONT_T0 = 2,
    MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITHOUT_T0 = 3,
    MEISO_K230D_DRM_SUPERVISOR_RELEASE_STATE_UNCERTAIN = 4
};

typedef uint8_t MeisoK230dDrmSupervisorWaitKind;
enum {
    MEISO_K230D_DRM_SUPERVISOR_WAIT_UNSET = 0,
    MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED = 1,
    MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED = 2,
    MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE = 3,
    MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED = 4,
    MEISO_K230D_DRM_SUPERVISOR_WAIT_STOP_REQUESTED = 5
};

typedef struct {
    uint8_t run_nonce[MEISO_K230D_DRM_SUPERVISOR_NONCE_BYTES];
    uint8_t expected_boot_id_sha256[
        MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES
    ];
    uint8_t expected_artifact_sha256[
        MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES
    ];
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest derive_request;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding;

typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    uint64_t term_after_ns;
    uint64_t kill_after_ns;
    uint64_t quarantine_after_ns;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig;

typedef struct {
    uint8_t boot_identity_matches;
    uint8_t artifact_identity_matches;
    uint8_t primary_node_identity_matches;
    uint8_t holder_scan_complete;
    uint8_t single_thread_verified;
    uint8_t reserved[3];
    uint32_t primary_holder_count;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight;

typedef struct {
    MeisoK230dDrmSupervisorDurableDisposition disposition;
    MeisoK230dDrmSupervisorDurableProgress progress;
    uint8_t reserved[2];
    uint32_t error_number;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate;

typedef struct {
    MeisoK230dDrmSupervisorGateKind kind;
    uint8_t ready_byte_observed;
    uint8_t exact_wait_child_matched;
    uint8_t exit_code_valid;
    uint16_t child_exit_code;
    uint8_t child_signal;
    uint8_t reserved;
    uint8_t child_start_ticks_valid;
    uint8_t reserved_tail[7];
    uint64_t child_start_ticks;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation;

typedef struct {
    MeisoK230dDrmSupervisorReleaseKind kind;
    uint8_t sigcont_dispatch_known;
    uint8_t sigcont_dispatched;
    uint8_t t0_valid;
    uint32_t error_number;
    uint64_t post_cont_t0_ns;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation;

typedef struct {
    MeisoK230dDrmSupervisorWaitKind kind;
    uint8_t exit_code_valid;
    uint16_t child_exit_code;
    uint8_t child_signal;
    uint8_t reserved_tail[3];
} MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation;

typedef struct {
    uint8_t stdout_sha256[MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES];
    uint8_t stderr_sha256[MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES];
    uint64_t stdout_bytes;
    uint64_t stderr_bytes;
    uint32_t primary_holder_count;
    uint8_t boot_identity_matches;
    uint8_t artifact_identity_matches;
    uint8_t primary_node_identity_matches;
    uint8_t holder_scan_complete;
    uint8_t stdout_complete;
    uint8_t stderr_complete;
    uint8_t stdout_hash_valid;
    uint8_t stderr_hash_valid;
    uint8_t stdout_synced;
    uint8_t stderr_synced;
    uint8_t stdout_closed;
    uint8_t stderr_closed;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight;

typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight preflight[2];
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate global_claim;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate evidence_claim;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate ready_record;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate quarantine_record;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate terminal_record;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation gate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation release;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation final_reap;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight postflight;
    MeisoK230dDrmSupervisorFailure first_failure;
    MeisoK230dDrmSupervisorOutcome outcome;
    uint8_t child_spawned;
    uint8_t child_ready_and_stopped;
    uint8_t child_gate_released;
    uint8_t child_release_state_uncertain;
    uint8_t child_reaped;
    uint8_t postflight_attempted;
    uint8_t postflight_succeeded;
    uint8_t stop_requested;
    uint8_t counters_saturated;
    uint8_t reserved_result_flag;
    uint32_t first_failure_error;
    uint32_t initial_preflight_callback_error;
    uint32_t global_claim_callback_error;
    uint32_t evidence_claim_callback_error;
    uint32_t spawn_callback_error;
    uint32_t gate_callback_error;
    uint32_t pre_release_preflight_callback_error;
    uint32_t ready_record_callback_error;
    uint32_t release_callback_error;
    uint32_t first_clock_callback_error;
    uint32_t first_wait_callback_error;
    uint32_t term_error;
    uint32_t kill_error;
    uint32_t quarantine_record_callback_error;
    uint32_t postflight_callback_error;
    uint32_t terminal_record_callback_error;
    uint32_t child_pid;
    uint64_t term_deadline_ns;
    uint64_t kill_deadline_ns;
    uint64_t quarantine_deadline_ns;
    uint64_t term_deadline_observed_now_ns;
    uint64_t kill_deadline_observed_now_ns;
    uint64_t quarantine_deadline_observed_now_ns;
    uint32_t preflight_call_count;
    uint32_t gate_observation_call_count;
    uint32_t timed_wait_call_count;
    uint32_t indefinite_wait_call_count;
    uint32_t interrupted_wait_count;
    uint32_t stop_request_count;
    uint32_t terminate_call_count;
    uint32_t kill_call_count;
    uint8_t global_claim_attempted;
    uint8_t evidence_claim_attempted;
    uint8_t spawn_attempted;
    uint8_t ready_record_attempted;
    uint8_t release_attempted;
    uint8_t quarantine_record_attempted;
    uint8_t terminal_record_attempted;
    uint8_t term_deadline_confirmed;
    uint8_t kill_deadline_confirmed;
    uint8_t quarantine_deadline_confirmed;
    uint8_t reserved_deadline_flag;
} MeisoK230dDrmSinglePlaneTestOnlySupervisorResult;

/*
 * The Linux process seam validates and opens the caller-owned absolute 0700
 * empty run directory before entering this portable core. It holds that exact
 * dirfd, the fixed global-dir fd, raw-output fds, and the ready channel only in
 * the opaque context. Paths and dirfds are intentionally absent from Config
 * and Result, so neither object can publish a private locator.
 *
 * Every callback returns zero or a positive errno. A negative return is an
 * EPROTO callback violation. Before a struct-output callback, the core fills
 * every byte of a local unsigned-only object with the fixed 0xff canary, then
 * applies the typed sentinels named below. Scalar sentinels are
 * instead child PID/parent ID zero, monotonic/start-ticks/byte-count values
 * UINT64_MAX, holder count UINT32_MAX, uint16 exit code UINT16_MAX, and derived
 * exit 0xff. A nonzero callback must leave those bytes
 * unchanged. On zero return, every discriminator, flag,
 * reserved byte, error, and related field must match the canonical forms below.
 * Durable-create reserved bytes are always zero.
 * A canary object is never copied into Result: the core copies only a validated
 * success observation or writes an explicit zero/unknown core-owned form.
 * Callback inputs are borrowed for that invocation, cannot be retained, and
 * must remain byte-exact; the core passes snapshots of Config/Ops and uses a
 * local Result until its single final caller-output write.
 *
 * Context extent is unknowable through void*. The caller therefore guarantees
 * that context storage does not overlap Config, Ops, output, or any callback
 * input. The API does not pretend that checking only the context base proves
 * this precondition.
 */
typedef struct {
    int (*inspect_preflight)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
        uint8_t preflight_index,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *out_preflight
    );
    int (*claim_global_launch_once)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_claim
    );
    int (*claim_evidence_launch_once)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_claim
    );
    int (*spawn_one_child)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
        uint32_t *out_child_pid
    );
    int (*await_child_ready_and_sigstop)(
        void *context,
        uint32_t child_pid,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *out_gate
    );
    int (*publish_launch_ready_record)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record
    );
    int (*release_gate_and_sample_post_cont_t0)(
        void *context,
        uint32_t child_pid,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *out_release
    );
    int (*wait_owned_child_until)(
        void *context,
        uint32_t child_pid,
        uint64_t absolute_deadline_ns,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *out_wait
    );
    int (*monotonic_now)(void *context, uint64_t *out_now_ns);
    int (*terminate_owned_child_once)(void *context, uint32_t child_pid);
    int (*kill_owned_child_once)(void *context, uint32_t child_pid);
    int (*publish_quarantine_record)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record
    );
    int (*wait_owned_child_to_reap)(
        void *context,
        uint32_t child_pid,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *out_wait
    );
    int (*inspect_postflight)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
        uint32_t child_pid,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *out_postflight
    );
    int (*publish_terminal_result)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record
    );
} MeisoK230dDrmSinglePlaneTestOnlySupervisorOps;

/*
 * Durable-create canonical forms:
 *   CREATED         => progress=PARENT_DIRECTORY_SYNCED, error=0;
 *   ALREADY_EXISTS  => progress=NONE, error=EEXIST, with any private body
 *                      removed and that cleanup directory-sync confirmed;
 *   FAILED_CLEAN    => progress=NONE, positive non-EEXIST/non-UINT32_MAX error, with no
 *                      surviving private body or visible name;
 *   STATE_UNCERTAIN => progress is PRIVATE_BODY_CREATED, BODY_SYNCED,
 *                      UNIQUE_NAME_PUBLISHED, or UNKNOWN, and error is positive
 *                      and not UINT32_MAX. PARENT_DIRECTORY_SYNCED is reserved
 *                      exclusively for CREATED.
 * Only CREATED is an authority to proceed. Global is attempted first and is
 * the authoritative per-boot no-rerun claim; evidence is attempted only after
 * global is durable. Any callback error or non-CREATED observation consumes
 * the invocation, permits no spawn/retry, and never removes or rewrites an
 * earlier object. Only both durable claims permit spawn.
 * A nonzero durable-claim or publication callback specifically means its
 * filesystem state is unknown even though its output bytes are unchanged; the
 * core records STATE_UNCERTAIN/UNKNOWN plus the normalized callback error.
 *
 * spawn_one_child receives an out PID seeded with zero and returns success
 * immediately after exactly one direct fork has transferred its numeric child
 * PID in 1..INT32_MAX to the core; a failed callback leaves zero byte-exact
 * and guarantees that no live or unreaped child was
 * created. The portable core can validate returned PID bytes but cannot recover
 * a Byzantine provider that secretly forks and withholds its PID. A zero return
 * with an invalid PID is EPROTO/no authority, not proof that an adversarial
 * provider created no hidden process. The RESULT_AVAILABLE no-child invariant
 * therefore relies on this side-effect guarantee; the production Linux callback
 * needs a source gate and harmless-child tests proving that fork failure returns
 * nonzero with no child and that, after receiving a valid PID, its parent branch
 * has no path that can return failure or withhold that PID before writing it and
 * returning zero. Non-branching best-effort bookkeeping may occur in that span;
 * handshake is separate.
 * READY_AND_STOPPED is valid only after the
 * ready byte and exact waitpid(child, WUNTRACED) WIFSTOPPED/SIGSTOP were both
 * observed. READY_AND_STOPPED also binds the PID to a positive Linux
 * /proc/PID/stat start-time tick value while the exact child is stopped. Gate
 * EXITED/SIGNALED consumes ownership; all other gate outcomes
 * retain it. A nonzero gate callback is forbidden from reaping or otherwise
 * transferring that child. Production installs no SIGCHLD disposition that
 * can reap behind this exact parent. The fork branch must pass the byte-exact
 * binding.derive_request received by spawn_one_child to the child helper and
 * direct derived-V2 seam; it cannot substitute request identity through context.
 *
 * A durable launch-ready record is the last potentially blocking publication
 * before release. release_gate_and_sample_post_cont_t0 dispatches SIGCONT at
 * most once and samples CLOCK_MONOTONIC only after SIGCONT succeeds. That T0,
 * every absolute wait deadline, and every monotonic_now output share the same
 * CLOCK_MONOTONIC nanosecond domain:
 *   NOT_RELEASED      => known=1, dispatched=0, t0_valid=0, positive
 *                        non-UINT32_MAX error,
 *                        t0=UINT64_MAX;
 *   RELEASED_WITH_POST_CONT_T0 => known=1, dispatched=1, t0_valid=1, error=0,
 *                        t0!=UINT64_MAX;
 *   RELEASED_WITHOUT_T0 => known=1, dispatched=1, t0_valid=0, positive
 *                          non-UINT32_MAX error,
 *                          t0=UINT64_MAX;
 *   STATE_UNCERTAIN   => known=0, dispatched=0 as a non-claim, t0_valid=0,
 *                        normalized positive callback error, t0=UINT64_MAX.
 * A nonzero callback is normalized to STATE_UNCERTAIN in the core-owned
 * Result. CONT is never retried.
 * Once release is attempted there is no synchronous publication before the
 * timed wait; T0 appears only in a later quarantine or terminal record. Any
 * callback failure, NOT_RELEASED, RELEASED_WITHOUT_T0, schedule overflow, or
 * otherwise missing trustworthy post-CONT T0 immediately dispatches the sole
 * KILL, attempts one quarantine record, and enters indefinite exact reap. It
 * never enters the 15/17/20 schedule with a guessed clock origin.
 *
 * Wait callbacks seed exit_code with UINT16_MAX. Canonical forms require every
 * reserved byte, exit-valid, exit_code, and
 * signal to be zero except CHILD_EXITED (exit-valid=1, signal=0; exit_code is
 * the actual value in 0..255) and CHILD_SIGNALED (exit-valid=0, exit_code=0;
 * signal is in 1..MAX_SIGNAL_NUMBER). A timed
 * DEADLINE is accepted only after a fresh monotonic_now (seeded UINT64_MAX)
 * confirms now >= that exact deadline;
 * an early deadline is EPROTO. The indefinite callback never returns DEADLINE.
 * INTERRUPTED, STOP_REQUESTED, or callback error never consumes ownership and
 * is retried; a nonzero callback is forbidden from reaping behind the core.
 * After every timed non-reap result or callback error, the core reads monotonic
 * time: at or beyond the requested deadline it advances exactly as DEADLINE
 * would; before it, it repeats the same absolute wait. A clock callback error
 * or invalid time enters immediate KILL/quarantine/infinite-reap rather than
 * spinning before a deadline. Indefinite-wait errors alone are retried without
 * a time bound. Every accepted clock sample is not UINT64_MAX, is >= T0, and is
 * >= the previous accepted sample; rollback is an invalid-time protocol debt.
 * Counters saturate instead of wrapping. Saturation is supervision debt and a
 * production process success predicate must reject it. A stop request records
 * debt but does not move TERM/KILL earlier than the original absolute schedule.
 *
 * Every failure after spawn that still retains child ownership enters the same
 * ownership fence. A gate observation that already reaped the child proceeds
 * directly to postflight without a signal or quarantine call. Otherwise, a
 * pre-release failure dispatches the sole KILL, attempts one quarantine record,
 * then calls the indefinite exact wait. After normal release, TERM is called at
 * most once at T0+15, KILL at most once at T0+17, and a still-owned child at
 * T0+20 causes
 * one quarantine-record attempt followed by indefinite exact wait. Signal and
 * record errors are retained but never cause a duplicate call or an early
 * return. Only exact EXITED/SIGNALED reap consumes ownership.
 * A +20 record uses state QUARANTINED_UNREAPED_AT_DEADLINE. Earlier ownership
 * fences use OWNERSHIP_FENCE_PENDING_EXACT_REAP and do not claim a deadline or
 * a final exact-wait observation.
 * TERM/KILL call counts are set before invocation. A positive or negative
 * callback return means that signal syscall was not confirmed successful and
 * the callback performed no wait/reap, but still consumes the sole call; the
 * same signal is never retried. A failed postflight may have partially closed
 * parent-owned output fds, is attempted only once, and occurs only after reap.
 *
 * RESULT_AVAILABLE writes caller output once either when no child was acquired
 * or after an acquired child was exactly reaped. A returned Result therefore
 * has child_spawned==0 or child_reaped==1. No-child failures have no terminal
 * record. A spawned run attempts a terminal record only after postflight proves
 * boot/artifact/node identity, complete holder scan with count zero, and both
 * raw streams complete/synced/closed. The terminal record cannot attest its own
 * durability; the returned terminal_record observation carries that ledger.
 * Neither supervision outcome nor publication interprets the embedded V2
 * receipt as accepted/rejected/not-tested.
 * Outcome is factual and uses this strict priority: no child; any quarantine
 * record attempt; any KILL call; any TERM call; otherwise reap without a
 * supervisor signal call. It never attributes causality or duplicates failure
 * state; first_failure and the per-operation errors carry all debt.
 * Each 15/17/20 transition additionally records the fresh observed-now value
 * and a confirmed flag before its signal or quarantine action. A zero flag is a
 * non-claim; a true flag requires observed-now >= the corresponding deadline.
 *
 * Preflight output is canonical only when every flag is 0 or 1, every reserved
 * byte is zero, holder count is not UINT32_MAX, and an incomplete holder scan
 * reports count zero. Admission requires all five flags equal 1 and holder
 * count zero. Gate READY_AND_STOPPED
 * requires ready_byte_observed=1, exact_wait_child_matched=1, exit-valid=0,
 * exit=0, signal=0, start-ticks-valid=1, and start ticks in
 * 1..UINT64_MAX-1; EXITED requires exact_wait_child_matched=1, exit-valid=1,
 * exit in 0..255, and signal=0; SIGNALED requires
 * exact_wait_child_matched=1, exit-valid=0,
 * exit=0, and signal in 1..MAX_SIGNAL_NUMBER.
 * EXITED/SIGNALED may report ready as 0 or 1. INTERRUPTED/STOP_REQUESTED may
 * report ready as 0 or 1 but require exact-match=0, exit-valid=0, exit=0,
 * signal=0. Every non-READY gate kind requires start-ticks-valid=0 and ticks=0.
 * All gate reserved bytes are zero. Gate INTERRUPTED retains ownership and
 * repeats the same gate observation; STOP_REQUESTED records a failure and
 * immediately enters the pre-release KILL/quarantine/infinite-reap fence.
 * Postflight output is canonical only when all flags are 0 or 1, holder count
 * is not UINT32_MAX, stdout/stderr byte counts are not
 * UINT64_MAX, and an incomplete holder scan reports count zero; success further
 * requires every identity/scan/stream/hash/sync/closed flag equal 1 and holder
 * count zero. In every canonical observation, hash-valid implies complete and
 * synced implies both complete and hash-valid. Hash bytes themselves may have
 * any value; the two hash-valid flags attest that the binder computed them over
 * the reported complete byte streams rather than treating a canary or zero
 * initialization as evidence.
 *
 * Production supplies the fixed 15/17/20-second offsets above. Shorter values
 * are private fake-test inputs only. Invocation rejects NULL or pairwise-
 * overlapping Config/Ops/output, zero nonce/digest/route IDs, NULL context or
 * missing callback, and a zero/non-increasing schedule without callbacks or
 * caller-output writes.
 * Checked addition after T0 rejects overflow and enters the post-spawn fence.
 * preflight_index is exactly 0 for the initial observation and 1 for the final
 * pre-release observation; no other value is passed.
 */
MEISO_K230D_DRM_SUPERVISOR_PRIVATE_API
MeisoK230dDrmSupervisorInvokeStatus
meiso_k230d_drm_private_supervise_single_plane_test_only_child(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig *config,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorOps *ops,
    void *context,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *out_result
);

typedef uint8_t MeisoK230dDrmSupervisorChildStatus;
enum {
    MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED = 0,
    MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_ARGUMENT = 1,
    MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_CONFIG = 2,
    MEISO_K230D_DRM_SUPERVISOR_CHILD_SETUP_FAILURE = 3,
    MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE = 4
};

typedef struct {
    int (*read_parent_id)(void *context, uint32_t *out_parent_id);
    int (*arm_parent_death_kill)(void *context);
    int (*isolate_fixed_child_file_descriptors)(void *context);
    int (*reset_and_unblock_child_signals_with_sigpipe_ignored)(void *context);
    int (*notify_ready_and_close_channel)(void *context);
    int (*stop_self_until_continued)(void *context);
    int (*run_derived_v2)(
        void *context,
        const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
        uint8_t *out_exit_code
    );
} MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps;

/*
 * The child helper snapshots request/Ops, validates their fixed ranges and
 * caller output as pairwise disjoint, and leaves out_exit_code byte-exact on
 * every non-DERIVED_RETURNED status. Context non-overlap is a caller
 * precondition for the same reason as the parent API. expected_parent_id must
 * be in 1..INT32_MAX; topology digest, connector, and CRTC must be nonzero. A
 * NULL context or missing callback is INVALID_CONFIG; a negative callback,
 * changed failure output, changed frozen
 * request, or derived exit outside {0,1} is PROVIDER_PROTOCOL_FAILURE. A
 * positive read-parent or setup callback is SETUP_FAILURE and is never retried;
 * a positive run_derived_v2 callback is PROVIDER_PROTOCOL_FAILURE. Every
 * callback input is byte-snapshotted; const-cast mutation is the same protocol
 * failure. All callback pointers and the typed request are invocation-only
 * borrows and may not be retained.
 *
 * Output callbacks seed uint32 parent IDs with zero and derived exit with 0xff;
 * nonzero callback return must not change them. Both successful parent reads
 * must be in 1..INT32_MAX and equal expected_parent_id; an out-of-domain success
 * is PROVIDER_PROTOCOL_FAILURE, while a different valid parent is SETUP_FAILURE
 * before ready notification. The exact order is parent read,
 * arm PDEATHSIG(SIGKILL), parent reread, fixed-FD isolation, signal reset and
 * unblock with SIGPIPE ignored, ready notification with ready-fd close, local
 * SIGSTOP gate, then the direct derived-V2 call. Fixed-FD isolation leaves only
 * stdin=/dev/null, fresh raw stdout/stderr, and the ready fd until notification;
 * all other inherited descriptors are closed. No exec/fork/clone seam exists.
 *
 * This Host-testable helper returns after the direct seam. The Linux fork branch
 * alone must flush/close its raw streams and call _exit with either the returned
 * 0/1 code or a fixed setup/protocol-failure code; it must never call exit or
 * run inherited cleanup handlers. The process seam has already called setsid,
 * redirected/isolated SSH stdio, and blocked/claimed HUP/INT/TERM/CHLD before
 * fork. Those Linux and filesystem properties require separate native tests and
 * target evidence; this portable contract does not claim to prove them.
 */
MEISO_K230D_DRM_SUPERVISOR_PRIVATE_API
MeisoK230dDrmSupervisorChildStatus
meiso_k230d_drm_private_run_single_plane_test_only_child(
    uint32_t expected_parent_id,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps *ops,
    void *context,
    uint8_t *out_exit_code
);

#ifdef __cplusplus
}
#endif

#endif
