#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <uint256.h>
#include <stdbool.h>
#include <block/chain.h>
#include <block/block.h>

#include <runtime_state.h>

// Nets
#define MAX_CONS 32 // Some baseline for now
#define LISTEN_PORT 9393
#define ECHO_PEERS 1 // If non-zero, automatically attempt to connect back to any inbound peers (helps form bidirectional peering)

// Node discovery
#define DISCOVERY_FANOUT 2 // "A couple" - how many peers to query per round, and how many new peers to accept per PEERS response (keeps the crawl spread out)
#define DISCOVERY_MAX_HOPS 3 // How many hops away from us we keep crawling
#define DISCOVERY_TARGET_CONNECTIONS 8 // Desired outbound connection count discovery tries to reach (bounded by MAX_CONS)
#define DISCOVERY_MAX_KNOWN_PEERS 256 // Cap on the known-peer table size
#define DISCOVERY_PEERS_RESPONSE_CAP 8 // Max endpoints we put in a single PEERS response
#define DISCOVERY_MAX_PINGS_PER_TICK 8 // Cap on UDP pings sent per discovery tick
#define DISCOVERY_PING_TIMEOUT_MS 5000ULL // Backstop: a PINGED peer with no pong for this long is marked unreachable
#define DISCOVERY_PING_REFRESH_MS 60000ULL // Re-ping a reachable peer after this long to refresh its latency
#define DISCOVERY_QUERY_INTERVAL_MS 15000ULL // Minimum interval between GET_PEERS to the same peer
#define DISCOVERY_CONNECT_RETRY_MS 30000ULL // Minimum interval between connect attempts to the same discovered peer
#define TCP_THREAD_STACK_SIZE (512 * 1024) // 512 KB. We could get away with like 128 KB since it's mostly just recv bufs, but it's good having some breathing room.
                                           // This is also for client threads. The server has the default (~8 MB on POSIX).

// Economics
#define DECIMALS 1000000000000ULL
#define DIFFICULTY_ADJUSTMENT_INTERVAL 3840 // Every 3840 blocks (roughly every 4 days with a 90 second block time)
                                           // Max adjustment per is x2. So if blocks are coming in too fast, the difficulty will at most double every 24 hours, and vice versa if they're coming in too slow.
#define TARGET_BLOCK_TIME 90 // Target block time in seconds
// The retarget measures the span between the FIRST and LAST block of the window, which is one fewer
// interval than the window has blocks, and divides by it. Two blocks is the minimum that leaves a
// non-zero span. See Chain_ComputeTargetAtHeight.
static_assert(DIFFICULTY_ADJUSTMENT_INTERVAL >= 2,
    "DIFFICULTY_ADJUSTMENT_INTERVAL must span at least one block interval");
#define INITIAL_DIFFICULTY 0x1f0c1422 // Default compact target used by Autolykos2 PoW (This is ridiculously low)
//#define INITIAL_DIFFICULTY 0x1f1b7c51 // Ridiculously low difficulty for testing.

// Mining
// The timestamp lives in the header the PoW hashes, so the miner restamps it while searching rather
// than keeping the one stamped when the search started. Two things fall out of that: a block carries
// the time it was actually found instead of a timestamp that is a whole block time stale on average,
// and every restamp is a fresh search space, so the nonce sweep starts over from 0 and never has to
// walk out to keep finding untried candidates. It costs nothing to throw the old nonce range away --
// each attempt is independent, so the work already done was never getting any closer.
static const uint64_t MINING_TIMESTAMP_REFRESH_MS = 2ULL; // Don't restamp for a drift smaller than this
// Reading the clock once per hash would be wasted work next to a memory-hard hash, so the check is
// batched. Note this, not the refresh interval, is what actually bounds accuracy once a batch of
// hashes takes longer than MINING_TIMESTAMP_REFRESH_MS -- keep it small enough that it doesn't.
static const uint64_t MINING_TIMESTAMP_CHECK_NONCES = 16ULL;

// Sync / Reorg tuning constants
// Timeouts and retry/backoff behavior for block fetches during sync (milliseconds)
static const uint64_t SYNC_REQUEST_TIMEOUT_MS = 5000ULL; // 5s
static const int MAX_SYNC_RETRIES = 4; // retry attempts per block fetch
static const uint64_t SYNC_BACKOFF_BASE_MS = 200ULL; // base backoff in ms (exponential)
// Parallelism
static const int MAX_PARALLEL_FETCHES = 8; // concurrent block fetches during windowed sync
// How far below a detected divergence we ask a peer for blocks, so the orphan pool has enough of
// the competing branch to locate the fork point by prevHash linkage.
static const uint64_t REORG_FETCH_DEPTH = 128ULL;
// How many times one `sync` will probe downwards for a fork point before giving up, so a peer on a
// permanently incompatible chain cannot keep us looping.
static const int MAX_FORK_PROBE_ROUNDS = 3;

// Reorg penalty configuration (Horizen-style delayed block submission penalty).
// A branch forking B blocks below our tip is held for penalty(B) blocks of local chain growth
// before it may be adopted, so a rented-hashrate attacker has to sustain the attack publicly
// instead of winning by dumping a privately mined branch.
//
// penalty(B) = ceil(FACTOR_NUM/FACTOR_DEN * B^EXPONENT * REF_BLOCK_TIME / TARGET_BLOCK_TIME)
//
// The block-time ratio is REF/TARGET, not TARGET/REF. penalty() counts BLOCKS, so the wall-clock
// protection is penalty(B) * TARGET_BLOCK_TIME ~= B^EXPONENT * REF_BLOCK_TIME: TARGET_BLOCK_TIME
// cancels and the protection is block-time-independent. See fetch_scheduler.c.
//
// Expressed as integer rationals on purpose: this feeds fork choice, so it must evaluate
// identically on every node. Floating point is not acceptable here.
static const uint64_t REORG_PENALTY_GRACE_BLOCKS = 3ULL; // allow small reorgs without penalty
static const uint64_t REORG_PENALTY_FACTOR_NUM = 1ULL; // base scaling factor (theta), numerator
static const uint64_t REORG_PENALTY_FACTOR_DEN = 1ULL; // base scaling factor (theta), denominator
static const uint32_t REORG_PENALTY_EXPONENT = 2U; // exponent p in penalty ~ B^p
static const uint64_t REORG_PENALTY_REF_BLOCK_TIME = 150ULL; // reference block time in seconds used by original scheme
// Beyond this depth the penalty saturates. At the configured parameters penalty(1000) is already
// ~1.67M blocks (~4.75 years at a 90s block time), so this only exists to keep the arithmetic away
// from overflow rather than to bound the penalty in any meaningful sense.
static const uint64_t REORG_PENALTY_MAX_DEPTH = 1000ULL;

/**
 * Mempool transaction timestamp policy. LOCAL POLICY, NOT CONSENSUS.
 *
 * These govern what this node is willing to hold and relay; a block containing a transaction that
 * violates either is still accepted. That separation is deliberate -- a node with a skewed clock
 * must not be able to fork itself off the network over an admission rule.
 *
 * A too-OLD timestamp needs no rule here: the per-account replay guard (see balance_sheet.h) already
 * refuses anything at or below a sender's last included transaction.
**/
// Refuse to admit a transaction dated further ahead than this of OUR OWN CLOCK. Measured against
// the clock and not against the chain tip on purpose: on a quiet chain the tip can be hours old, and
// judging "future" against it would refuse honest transactions exactly when blocks are sparse.
static const uint64_t TX_MAX_FUTURE_DRIFT_MS = 2ULL * 60ULL * 60ULL * 1000ULL; // 2 hours
// Drop transactions older than this from the mempool, so it is not inflated by junk that will never
// be mined. Roughly the ~4 days DIFFICULTY_ADJUSTMENT_INTERVAL spans, but expressed in milliseconds
// so it does not drift if the block time changes.
static const uint64_t TX_EXPIRY_MS = 4ULL * 24ULL * 60ULL * 60ULL * 1000ULL; // 4 days

// Upper bound on pooled orphan blocks. Orphans are accepted before the chain-derived difficulty
// check (that lives in Chain_AddBlock, which orphans only reach on attach), so without a cap a
// peer can push blocks at an arbitrary height until the node runs out of memory.
static const size_t MAX_ORPHAN_BLOCKS = 512U;

// A node whose chain tip is older than this many target block times is catching up rather than
// following the tip, and is exempt from the reorg penalty (Horizen does the same via
// IsInitialBlockDownload). Determined purely from local state, so an unverified peer cannot
// trigger the exemption by claiming a large height.
//
// This is also the ONLY way a non-mining node rejoins the network after ending up on a minority
// fork: the penalty is served by local chain growth, and a node that does not mine has no way to
// grow except by adopting the very branch the penalty is gating. It therefore has to be short
// enough that such a node recovers in minutes rather than half a day.
//
// 20 block times is ~30 minutes at a 90s target, far beyond normal Poisson block spacing (a gap
// that long has probability ~e^-20), so a node that is genuinely following the tip will not trip
// it. Note the exemption is all-or-nothing -- once in IBD a node accepts a reorg of any depth --
// so lowering this further widens that hole; it is the number to revisit if deep reorgs ever get
// used against an idle node.
static const uint64_t IBD_TIP_AGE_BLOCKS = 20ULL;
// Number of trailing blocks whose median timestamp is used for the age test above. Using a median
// rather than the tip alone means a single miner cannot backdate one block to fake being in IBD.
static const size_t MEDIAN_TIME_SPAN = 11U;

// Reward schedule acceleration: 1 means normal-speed progression.
#define EMISSION_ACCELERATION_FACTOR 1ULL

// Phase-one target horizon: emit ~2^64-1 atomic units by this many blocks at x1.
#define PHASE1_TARGET_BLOCKS 3000000ULL

// Inflation is expressed in tenths of a percent to preserve integer math.
#define INFLATION_PERCENTAGE_PER_EPOCH_TENTHS 15ULL // 1.5%

// Monero-style main emission: reward = (MONEY_SUPPLY - generated) >> speed factor.
// Keep this at 20 to match the canonical curve shape against a 2^64 atomic supply cap.
#define MONERO_EMISSION_SPEED_FACTOR 20U

// Autolykos2 epoch / DAG constants.
#define EPOCH_LENGTH 350000 // ~1 year at 90s
#define DAG_GENESIS_SEED 0x00 // Epoch 0's seed is all zeroes; epoch k's seed is the hash of the last
                              // block of epoch k-1, so it is unpredictable until that block is mined.

/**
 * DAG size band and the miner signal that moves within it.
 *
 * Growth is the DEFAULT: the size walks up by DAG_EPOCH_STEP every epoch unless miners actively
 * brake it. There is deliberately no "grow faster" vote -- every signal a miner can express only
 * slows the walk or reverses it. That is what makes the scheme safe against pool capture: under
 * stratum-style pooled mining the pool builds the header, so it controls its share of the vote, and
 * a pool that wanted a larger DAG to price smaller miners out simply has no lever to pull. The
 * entire upward trajectory is set by DAG_EPOCH_STEP and DAG_MAX_SIZE, i.e. by release, not by vote.
 *
 * DAG_MIN_SIZE is the ASIC-resistance floor: it must stay above the on-die SRAM an ASIC could
 * economically carry, because *this constant*, not the vote, is what secures the property. No vote
 * outcome can go below it. DAG_MAX_SIZE is the intended destination rather than an emergency bound,
 * since the DAG reaches it on its own -- pick it as the largest DAG miners should ever hold.
 *
 * NOTE: these three sizes are economic judgements, not derivations. Sanity-check them before
 * launch. DAG_BASE_SIZE was previously commented as an intended 6 GiB; it now has to sit inside
 * the band (see the static_assert below). Lowering the DAG for a test run means lowering
 * DAG_MIN_SIZE too, not just DAG_BASE_SIZE.
**/
#define DAG_MIN_SIZE   (2ULL << 30) // 2 GiB -- ASIC-resistance floor
#define DAG_BASE_SIZE  (2ULL << 30) // epoch 0 size
#define DAG_MAX_SIZE   (8ULL << 30) // 8 GiB -- intended destination, ~6 unbraked years from base
#define DAG_EPOCH_STEP (1ULL << 30) // 1 GiB drift per epoch, in either direction

// Vote thresholds as integer numerator/denominator pairs, never float literals: this feeds PoW
// verification, so every node must reach the same verdict. The tests cross-multiply rather than
// divide, so there is no rounding to disagree on.
#define DAG_BRAKE_NUM 1ULL
#define DAG_BRAKE_DEN 2ULL // brake growth when hold+down votes exceed 1/2 of the epoch
#define DAG_DOWN_NUM  7ULL
#define DAG_DOWN_DEN  8ULL // shrink when down votes exceed 7/8 of the epoch, two epochs running

// reserved[0] of the block header carries the vote. 0 must mean GROW: the point of this shape is
// that inaction produces growth, so a miner that knows nothing about the vote contributes to the
// intended default instead of silently freezing the schedule.
#define DAG_VOTE_GROW 0u // default -- let the schedule run
#define DAG_VOTE_HOLD 1u // brake: stop growing
#define DAG_VOTE_DOWN 2u // reverse: shrink (needs a sustained supermajority to take effect)
#define DAG_VOTE_MAX  DAG_VOTE_DOWN

static_assert(DAG_MIN_SIZE <= DAG_BASE_SIZE && DAG_BASE_SIZE <= DAG_MAX_SIZE,
              "DAG_BASE_SIZE must start inside [DAG_MIN_SIZE, DAG_MAX_SIZE]");
static_assert(DAG_MIN_SIZE % 32ULL == 0ULL && DAG_MAX_SIZE % 32ULL == 0ULL &&
              DAG_BASE_SIZE % 32ULL == 0ULL && DAG_EPOCH_STEP % 32ULL == 0ULL,
              "Autolykos2 lane addressing requires every DAG size to be a multiple of 32");
static_assert(DAG_EPOCH_STEP > 0ULL, "DAG_EPOCH_STEP must be positive or the DAG can never move");

static const uint64_t M_CAP = 18446744073709551615ULL; // Max uint64
static const uint64_t TAIL_EMISSION = 750000000000ULL; // 0.75 coins per block floor
// No max supply. Instead of halving, it'll follow a more gradual, Monero-like emission curve.

// Phase 3: update once per effective epoch and keep a fixed per-block reward for that epoch.
//
// The *AtHeight variants take the height directly and never call Chain_Size/Chain_GetBlockCopy, so
// they are safe to call from inside a chainLock critical section. chainLock is a non-recursive
// pthread_rwlock_t: taking it for reading while this thread already holds it for writing deadlocks
// as soon as another thread is queued for the write lock.
static inline uint64_t GetInflationRateRewardAtHeight(uint256_t currentSupply, uint64_t height) {
    const uint64_t effectiveEpochLength =
        (EPOCH_LENGTH / EMISSION_ACCELERATION_FACTOR) > 0
            ? (EPOCH_LENGTH / EMISSION_ACCELERATION_FACTOR)
            : 1;

    if (height == 0) {
        currentReward = TAIL_EMISSION;
        return currentReward;
    }
    
    if (height % effectiveEpochLength == 0) {
        // inflationPerBlock = currentSupply * 1.5% / effectiveEpochLength
        // = currentSupply * 15 / (1000 * effectiveEpochLength)
        uint256_t multiplied = uint256_from_u64(0);
        for (uint64_t i = 0; i < INFLATION_PERCENTAGE_PER_EPOCH_TENTHS; ++i) {
            uint256_add(&multiplied, &currentSupply);
        }

        uint64_t divisor = 1000ULL * effectiveEpochLength;
        uint256_t quotient = {{0, 0, 0, 0}};
        unsigned __int128 remainder = 0;

        // Work from the most significant limb to the least
        for (int i = 3; i >= 0; i--) {
            unsigned __int128 current = (remainder << 64) | multiplied.limbs[i];
            quotient.limbs[i] = (uint64_t)(current / divisor);
            remainder = current % divisor;
        }

        uint64_t inflationPerBlock = quotient.limbs[0];
        currentReward = (inflationPerBlock > TAIL_EMISSION) ? inflationPerBlock : TAIL_EMISSION;
        return currentReward;
    }

    return (currentReward > TAIL_EMISSION) ? currentReward : TAIL_EMISSION;
}

static inline uint64_t GetInflationRateReward(uint256_t currentSupply, blockchain_t* chain) {
    if (!chain || !chain->blocks) { return 0x00; } // Invalid
    return GetInflationRateRewardAtHeight(currentSupply, (uint64_t)Chain_Size(chain));
}

static inline uint64_t CalculateBlockRewardAtHeight(uint256_t currentSupply, uint64_t height) {
    const uint64_t effectivePhase1Blocks =
        (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR) > 0
            ? (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR)
            : 1;

    // After the phase-one target horizon, only floor/inflation schedule applies.
    if (height >= effectivePhase1Blocks) {
        return GetInflationRateRewardAtHeight(currentSupply, height);
    }

    if (currentSupply.limbs[1] > 0 ||
        currentSupply.limbs[2] > 0 ||
        currentSupply.limbs[3] > 0 ||
        currentSupply.limbs[0] >= M_CAP)
    {
        // Post-Monero phase with unlimited supply: floor/inflation schedule only.
        return GetInflationRateRewardAtHeight(currentSupply, height);
    }

    const uint64_t generated = currentSupply.limbs[0];
    const uint64_t remaining = M_CAP - generated;

    // Monero-style base curve against ~2^64 atomic-unit terminal supply.
    uint64_t reward = remaining >> MONERO_EMISSION_SPEED_FACTOR;

    // Acceleration preserves curve shape while reaching the floor sooner in block-height terms.
    if (EMISSION_ACCELERATION_FACTOR > 1ULL && reward > 0ULL) {
        __uint128_t accelerated = (__uint128_t)reward * (__uint128_t)EMISSION_ACCELERATION_FACTOR;
        reward = (accelerated > (__uint128_t)remaining) ? remaining : (uint64_t)accelerated;
    }

    // Retarget phase one to finish by PHASE1_TARGET_BLOCKS (x1), while keeping
    // Monero-style behavior as the preferred curve when it is already sufficient.
    const uint64_t blocksLeft = effectivePhase1Blocks - height;
    const uint64_t minRewardToFinish = (remaining + blocksLeft - 1ULL) / blocksLeft; // ceil(remaining / blocksLeft)
    if (reward < minRewardToFinish) {
        reward = minRewardToFinish;
    }
    if (reward > remaining) {
        reward = remaining;
    }

    // Phase 1 until Monero reward goes below the floor.
    if (reward > TAIL_EMISSION) {
        return reward;
    }

    // Phase 2 + 3: floor and epoch inflation updates.
    return GetInflationRateRewardAtHeight(currentSupply, height);
}

static inline uint64_t CalculateBlockReward(uint256_t currentSupply, blockchain_t* chain) {
    if (!chain || !chain->blocks) { return 0x00; } // Invalid
    return CalculateBlockRewardAtHeight(currentSupply, (uint64_t)Chain_Size(chain));
}

// Hashing DAG: see Chain_DagParamsForHeight in block/chain.h. Both the size and the epoch seed are
// derived from the chain by that one function, so the mining and verification paths cannot drift
// apart. The previous CalculateTargetDAGSize/GetNextDAGSeed pair lived here, took chainLock
// internally, was not epoch-aligned, and disagreed with the verifier's own copy in main.c.

#endif
