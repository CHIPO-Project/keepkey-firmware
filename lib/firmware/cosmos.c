#include "keepkey/firmware/cosmos.h"

#include "keepkey/board/confirm_sm.h"
#include "keepkey/board/util.h"
#include "keepkey/firmware/home_sm.h"
#include "keepkey/firmware/storage.h"
#include "trezor/crypto/secp256k1.h"
#include "trezor/crypto/ecdsa.h"
#include "trezor/crypto/memzero.h"
#include "trezor/crypto/segwit_addr.h"

#include <stdbool.h>
#include <time.h>

static CONFIDENTIAL HDNode node;
static SHA256_CTX ctx;
static bool has_message;
static bool initialized;
static uint32_t msgs_remaining;
static CosmosSignTx msg;

bool cosmos_path_mismatched(const CoinType *_coin,
                            const uint32_t *_address_n,
                            const uint32_t _address_n_count)
{
    // m/44' : BIP44-like path
    // m / purpose' / bip44_account_path' / account' / x / y
    bool mismatch = false;
    mismatch |= _address_n_count != 5;
    mismatch |= _address_n_count > 0 && (_address_n[0] != (0x80000000 + 44));
    mismatch |= _address_n_count > 1 && (_address_n[1] != _coin->bip44_account_path);
    mismatch |= _address_n_count > 2 && (_address_n[2] & 0x80000000) == 0;
    mismatch |= _address_n_count > 3 && _address_n[3] != 0;
    mismatch |= _address_n_count > 4 && _address_n[4] != 0;
    return mismatch;
}

const CosmosSignTx *cosmos_getCosmosSignTx(void)
{
    return &msg;
}

/**
 * Gets the address
 *
 * \param node    HDNode from which the address is to be derived
 * \param address Output buffer
 *
 * \returns true if successful
 */
bool cosmos_getAddress(const HDNode *_node, char *address)
{
    uint8_t hash160Buf[RIPEMD160_DIGEST_LENGTH];
    ecdsa_get_pubkeyhash(_node->public_key, HASHER_SHA2_RIPEMD, hash160Buf);

    uint8_t fiveBitExpanded[RIPEMD160_DIGEST_LENGTH * 8 / 5];
    size_t len = 0;
    convert_bits(fiveBitExpanded, &len, 5, hash160Buf, 20, 8, 1);
    return bech32_encode(address, "cosmos", fiveBitExpanded, len) == 1;
}

void sha256UpdateEscaped(SHA256_CTX *_ctx, const char *s, size_t len)
{
    while (len > 0)
    {
        if (s[0] == '"')
        {
            sha256_Update(_ctx, (uint8_t *)"\\\"", 2);
        }
        else if (s[0] == '\\')
        {
            sha256_Update(_ctx, (uint8_t *)"\\\\", 2);
        }
        else
        {
            sha256_Update(_ctx, (uint8_t *)&s[0], 1);
        }
        s = &s[1];
        len--;
    }
}

bool cosmos_signTxInit(const HDNode* _node, const CosmosSignTx *_msg)
{
    initialized = true;
    msgs_remaining = _msg->msg_count;
    has_message = false;

    memzero(&node, sizeof(node));
    memcpy(&node, _node, sizeof(node));
    memcpy(&msg, _msg, sizeof(msg));

    int n;
    sha256_Init(&ctx);
    char buffer[64 + 1];

    // Each segment guaranteed to be less than or equal to 64 bytes
    // 19 + ^20 + 1 = ^40
    n = snprintf(buffer, sizeof(buffer), "{\"account_number\":\"%" PRIu64 "\"", msg.account_number);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t *)buffer, n);

    // <escape chain_id>
    const char *const chainid_prefix = ",\"chain_id\":\"";
    sha256_Update(&ctx, (uint8_t *)chainid_prefix, strlen(chainid_prefix));
    sha256UpdateEscaped(&ctx, msg.chain_id, strlen(msg.chain_id));

    // 30 + ^10 + 19 = ^59
    n = snprintf(buffer, sizeof(buffer), "\",\"fee\":{\"amount\":[{\"amount\":\"%" PRIu32 "\",\"denom\":\"uatom\"}]", msg.fee_amount);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t *)buffer, n);

    // 8 + ^10 + 2 = ^20
    n = snprintf(buffer, sizeof(buffer), ",\"gas\":\"%" PRIu32 "\"}", msg.gas);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t *)buffer, n);

    // <escape memo>
    const char *const memo_prefix = ",\"memo\":\"";
    sha256_Update(&ctx, (uint8_t *)memo_prefix, strlen(memo_prefix));
    if (msg.has_memo) {
        sha256UpdateEscaped(&ctx, msg.memo, strlen(msg.memo));
    }

    // 10
    sha256_Update(&ctx, (uint8_t *)"\",\"msgs\":[", 10);

    return true;
}

bool cosmos_signTxUpdateMsgSend(const uint64_t amount,
                                const char *to_address)
{
    int n;
    char buffer[64 + 1];

    size_t decoded_len;
    char hrp[45];
    uint8_t decoded[38];
    if (!bech32_decode(hrp, decoded, &decoded_len, to_address)) { return false; }

    char from_address[46];
    if (!cosmos_getAddress(&node, from_address)) { return false; }

    if (has_message) {
        sha256_Update(&ctx, (uint8_t*)",", 1);
    }

    const char *const prelude = "{\"type\":\"cosmos-sdk/MsgSend\",\"value\":{";
    sha256_Update(&ctx, (uint8_t *)prelude, strlen(prelude));

    // 21 + ^20 + 19 = ^60
    n = snprintf(buffer, sizeof(buffer), "\"amount\":[{\"amount\":\"%" PRIu64 "\",\"denom\":\"uatom\"}]", amount);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t*)buffer, n);

    // 17 + 45 + 1 = 63
    n = snprintf(buffer, sizeof(buffer), ",\"from_address\":\"%s\"", from_address);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t*)buffer, n);

    // 15 + 45 + 3 = 63
    n = snprintf(buffer, sizeof(buffer), ",\"to_address\":\"%s\"}}", to_address);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t*)buffer, n);

    has_message = true;
    msgs_remaining--;
    return true;
}

bool cosmos_signTxFinalize(uint8_t* public_key, uint8_t* signature)
{
    int n;
    char buffer[64 + 1];

    // 16 + ^20 = ^36
    n = snprintf(buffer, sizeof(buffer), "],\"sequence\":\"%" PRIu64 "\"}", msg.sequence);
    if (n < 0) { return false; }
    sha256_Update(&ctx, (uint8_t*)buffer, n);

    hdnode_fill_public_key(&node);
    memcpy(public_key, node.public_key, 33);

    uint8_t hash[SHA256_DIGEST_LENGTH];
    sha256_Final(&ctx, hash);
    return ecdsa_sign_digest(&secp256k1, node.private_key, hash, signature, NULL, NULL) == 0;
}

bool cosmos_signingIsInited(void) {
    return initialized;
}

bool cosmos_signingIsFinished(void) {
    return msgs_remaining == 0;
}

void cosmos_signAbort(void) {
    initialized = false;
    has_message = false;
    msgs_remaining = 0;
    memzero(&msg, sizeof(msg));
    memzero(&node, sizeof(node));
}
