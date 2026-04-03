#include <cstdint>
#include <cstring>

#include <cpuAutolykos.h>
#include <definitions.h>

// Reference implementation dependency from request.cc. We provide it locally
// to avoid pulling the network/request stack.
uint32_t calcN(uint32_t Hblock) {
    uint32_t headerHeight = 0;
    ((uint8_t*)&headerHeight)[0] = ((uint8_t*)&Hblock)[3];
    ((uint8_t*)&headerHeight)[1] = ((uint8_t*)&Hblock)[2];
    ((uint8_t*)&headerHeight)[2] = ((uint8_t*)&Hblock)[1];
    ((uint8_t*)&headerHeight)[3] = ((uint8_t*)&Hblock)[0];

    uint32_t newN = INIT_N_LEN;
    if (headerHeight < IncreaseStart) {
        newN = INIT_N_LEN;
    } else if (headerHeight >= IncreaseEnd) {
        newN = MAX_N_LEN;
    } else {
        uint32_t itersNumber = (headerHeight - IncreaseStart) / IncreasePeriodForN + 1;
        for (uint32_t i = 0; i < itersNumber; i++) {
            newN = newN / 100 * 105;
        }
    }

    return newN;
}

struct skalacoin_autolykos2_ref_handle {
    AutolykosAlg* alg;
};

extern "C" void* skalacoin_autolykos2_ref_create(void) {
    skalacoin_autolykos2_ref_handle* h = new skalacoin_autolykos2_ref_handle();
    h->alg = new AutolykosAlg();
    return h;
}

extern "C" void skalacoin_autolykos2_ref_destroy(void* handle) {
    if (!handle) {
        return;
    }

    auto* h = static_cast<skalacoin_autolykos2_ref_handle*>(handle);
    delete h->alg;
    delete h;
}

extern "C" bool skalacoin_autolykos2_ref_check_target(
    void* handle,
    const uint8_t message32[32],
    uint64_t nonce,
    uint32_t height,
    const uint8_t target32[32]
) {
    if (!handle || !message32 || !target32) {
        return false;
    }

    auto* h = static_cast<skalacoin_autolykos2_ref_handle*>(handle);
    if (!h->alg) {
        return false;
    }

    uint8_t nonceBytes[8];
    uint8_t heightBytes[4];
    std::memcpy(nonceBytes, &nonce, sizeof(nonceBytes));

    // RunAlg expects height bytes; keep deterministic network order.
    heightBytes[0] = static_cast<uint8_t>((height >> 24) & 0xffu);
    heightBytes[1] = static_cast<uint8_t>((height >> 16) & 0xffu);
    heightBytes[2] = static_cast<uint8_t>((height >> 8) & 0xffu);
    heightBytes[3] = static_cast<uint8_t>(height & 0xffu);

    uint8_t poolBound[32];
    std::memcpy(poolBound, target32, sizeof(poolBound));

    return h->alg->RunAlg(
        const_cast<uint8_t*>(message32),
        nonceBytes,
        poolBound,
        heightBytes
    );
}
