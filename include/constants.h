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
//#define INITIAL_DIFFICULTY 0x1f0c1422 // Default compact target used by Autolykos2 PoW (This is ridiculously low)
#define INITIAL_DIFFICULTY 0x1f1b7c51 // This takes 90s on my machine with a single thread, good for testing

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
// penalty(B) = ceil(FACTOR_NUM/FACTOR_DEN * B^EXPONENT * TARGET_BLOCK_TIME / REF_BLOCK_TIME)
//
// Expressed as integer rationals on purpose: this feeds fork choice, so it must evaluate
// identically on every node. Floating point is not acceptable here.
static const uint64_t REORG_PENALTY_GRACE_BLOCKS = 3ULL; // allow small reorgs without penalty
static const uint64_t REORG_PENALTY_FACTOR_NUM = 1ULL; // base scaling factor (theta), numerator
static const uint64_t REORG_PENALTY_FACTOR_DEN = 1ULL; // base scaling factor (theta), denominator
static const uint32_t REORG_PENALTY_EXPONENT = 2U; // exponent p in penalty ~ B^p
static const uint64_t REORG_PENALTY_REF_BLOCK_TIME = 150ULL; // reference block time in seconds used by original scheme
// Beyond this depth the penalty saturates. At the configured parameters penalty(1000) is already
// ~600k blocks (over a year), so this only exists to keep the arithmetic away from overflow.
static const uint64_t REORG_PENALTY_MAX_DEPTH = 1000ULL;

// Upper bound on pooled orphan blocks. Orphans are accepted before the chain-derived difficulty
// check (that lives in Chain_AddBlock, which orphans only reach on attach), so without a cap a
// peer can push blocks at an arbitrary height until the node runs out of memory.
static const size_t MAX_ORPHAN_BLOCKS = 512U;

// A node whose chain tip is older than this many target block times is catching up rather than
// following the tip, and is exempt from the reorg penalty (Horizen does the same via
// IsInitialBlockDownload). Determined purely from local state, so an unverified peer cannot
// trigger the exemption by claiming a large height.
static const uint64_t IBD_TIP_AGE_BLOCKS = 500ULL;
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

// Future Autolykos2 constants:
#define EPOCH_LENGTH 350000 // ~1 year at 90s
#define DAG_BASE_GROWTH (1ULL << 30) // 1 GB per epoch, adjusted by acceleration
//#define DAG_BASE_SIZE (6ULL << 30) // 6 GB, adjusted per cycle based off DAG_BASE_GROWTH
#define DAG_BASE_SIZE (1ULL << 30) // TEMPORARY FOR TESTING
// Swings - calculated as MIN(percentage, absolute GB) to prevent absurd swings from low hashrate or very large DAG growth.
// Percentages are integer numerator/denominator pairs, never float literals: DAG size feeds PoW
// verification, so it has to evaluate identically on every node.
#define DAG_MAX_UP_SWING_PERCENT_NUM 15ULL // +15%
#define DAG_MAX_DOWN_SWING_PERCENT_NUM 10ULL // -10%
#define DAG_SWING_PERCENT_DEN 100ULL
#define DAG_MAX_UP_SWING_GB (2ULL << 30) // 2 GB
#define DAG_MAX_DOWN_SWING_GB (1ULL << 30) // 1 GB
#define DAG_GENESIS_SEED 0x00 // Genesis seed is zeroes, every epoch's seed is the hash of the previous block, therefore unpredictable until the block is mined

/**
 * Each epoch has 2 phases, connected logarithmically:
 * - Phase 1: Aggressive DAG growth (target is ~75% of the max cap) to kick out any ASICs, 30k blocks (roughly 1 month)
 * - Phase 2: Stable DAG growth (target is the max cap) to provide a stable environment for GPU miners, 320k blocks (roughly 11 months)
**/

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

// Hashing DAG
static inline size_t CalculateTargetDAGSize(blockchain_t* chain) {
    // Base size plus (base growth * difficulty factor), adjusted by acceleration
    if (!chain || !chain->blocks) { return 0; } // Invalid
    uint64_t height = (uint64_t)Chain_Size(chain);
    
    if (height < EPOCH_LENGTH) {
        return DAG_BASE_SIZE;
    }

    // Get the height - EPOCH_LENGTH block and the last block;
    block_t* lastBlock = NULL;
    block_t* epochStartBlock = NULL;
    if (!Chain_GetBlockCopy(chain, Chain_Size(chain) - 1, &lastBlock) || !lastBlock) {
        if (lastBlock) Block_Destroy(lastBlock);
        return 0;
    }
    if (!Chain_GetBlockCopy(chain, (size_t)(Chain_Size(chain) - 1 - EPOCH_LENGTH), &epochStartBlock) || !epochStartBlock) {
        Block_Destroy(lastBlock);
        if (epochStartBlock) Block_Destroy(epochStartBlock);
        return 0;
    }

    int64_t difficultyDelta = (int64_t)epochStartBlock->header.difficultyTarget - (int64_t)lastBlock->header.difficultyTarget;
    int64_t growth = (DAG_BASE_GROWTH * difficultyDelta); // Can be negative if difficulty has decreased, which is why we use int64_t

    // Clamp
    if (growth > 0) {
        // Difficulty increased -> Clamp the UPWARD swing
        int64_t maxUp = (int64_t)((DAG_BASE_SIZE * DAG_MAX_UP_SWING_PERCENT_NUM) / DAG_SWING_PERCENT_DEN);
        if (growth > maxUp) growth = maxUp;
        if (growth > (int64_t)DAG_MAX_UP_SWING_GB) growth = DAG_MAX_UP_SWING_GB;
    } else {
        // Difficulty decreased -> Clamp the DOWNWARD swing
        int64_t maxDown = (int64_t)((DAG_BASE_SIZE * DAG_MAX_DOWN_SWING_PERCENT_NUM) / DAG_SWING_PERCENT_DEN);
        if (-growth > maxDown) growth = -maxDown;
        if (-growth > (int64_t)DAG_MAX_DOWN_SWING_GB) growth = -(int64_t)DAG_MAX_DOWN_SWING_GB;
    }
    
    int64_t targetSize = (int64_t)DAG_BASE_SIZE + growth;
    if (targetSize <= 0) {
        Block_Destroy(lastBlock);
        Block_Destroy(epochStartBlock);
        return 0;
    }

    size_t out = (size_t)targetSize;
    Block_Destroy(lastBlock);
    Block_Destroy(epochStartBlock);
    return out;
}

static inline void GetNextDAGSeed(blockchain_t* chain, uint8_t outSeed[32]) {
    if (!chain || !chain->blocks || !outSeed) { return; } // Invalid
    uint64_t height = (uint64_t)Chain_Size(chain);

    if (height < EPOCH_LENGTH) {
        memset(outSeed, DAG_GENESIS_SEED, 32);
        return;
    }

    block_t* prevBlock = NULL;
    if (!Chain_GetBlockCopy(chain, Chain_Size(chain) - 1, &prevBlock) || !prevBlock) {
        memset(outSeed, 0x00, 32); // Fallback to zeroes if we can't get the previous block for some reason; The caller should treat this as an error if height >= EPOCH_LENGTH
        if (prevBlock) Block_Destroy(prevBlock);
        return;
    }

    Block_CalculateHash(prevBlock, outSeed);
    Block_Destroy(prevBlock);
}

#endif
