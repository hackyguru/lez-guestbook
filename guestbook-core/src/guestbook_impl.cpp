#include "guestbook_impl.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "wallet_ffi.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// The guestbook program on the LEZ v0.3 public testnet, deployed 2026-10-04
// from guestbook-program/guestbook.bin (image id 00e8aa77…). After a testnet
// reset, redeploy (tests/run.sh deploy) and update this, or use setProgram() /
// the UI's Details.
constexpr const char* kDefaultProgram = "Edc6RDHB6bjCUfseKfff7jkhEwWqDHuBieWpE2YiyHq";

// The testnet's shared faucet: a genesis account whose key is public on
// purpose (`PRIVATE_KEY_PUB_ACC_B` in lez/testnet_initial_state).
constexpr const char* kFaucetKeyHex = "717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b";
constexpr const char* kFaucetB58 = "7wHg9sbJwc6h3NP1S9bekfAzB8CHifEcxKswCKUt3YQo";

using u128 = unsigned __int128;
constexpr u128 kLgo = 1000000000;
constexpr int64_t kTickMs = 5000;
constexpr int64_t kConfirmTimeoutMs = 4 * 60 * 1000;
constexpr size_t kMaxName = 40;
constexpr size_t kMaxText = 500;
constexpr int kReadsPerTick = 25;       // entry fetches per refresh, newest first

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}

FfiBytes32 ffi(const std::array<uint8_t, 32>& b)
{
    FfiBytes32 f{};
    std::copy(b.begin(), b.end(), f.data);
    return f;
}

std::array<uint8_t, 32> fromFfi(const FfiBytes32& f)
{
    std::array<uint8_t, 32> b{};
    std::copy(std::begin(f.data), std::end(f.data), b.begin());
    return b;
}

std::string formatLgo(u128 atomic)
{
    auto dec = [](u128 v) {
        if (!v) return std::string("0");
        std::string s;
        while (v) { s.insert(s.begin(), char('0' + int(v % 10))); v /= 10; }
        return s;
    };
    std::string frac = dec(atomic % kLgo);
    frac.insert(frac.begin(), 9 - frac.size(), '0');
    frac.resize(4);
    while (frac.size() > 2 && frac.back() == '0') frac.pop_back();
    return dec(atomic / kLgo) + "." + frac;
}

std::string errorText(int code)
{
    switch (code) {
    case NETWORK_ERROR:      return "Couldn't reach the LEZ sequencer.";
    case INSUFFICIENT_FUNDS: return "Not enough LGO for the fee.";
    case PAYER_CANNOT_FUND:  return "Not enough LGO for the fee — tap Get LGO.";
    case INVALID_BYTECODE:   return "That file isn't a valid LEZ program.";
    case INTERNAL_ERROR:     return "The wallet couldn't build that transaction (error 99) — the Basecamp log has details.";
    default:                 return "Wallet error " + std::to_string(code) + ".";
    }
}

void check(WalletFfiError e, const char* what)
{
    if (e != SUCCESS) {
        fprintf(stderr, "[guestbook] %s failed: %d\n", what, int(e));
        throw std::runtime_error(errorText(int(e)));
    }
}

std::string jsonOk(const json& extra = json::object())
{
    json j = extra;
    j["ok"] = true;
    return j.dump();
}

std::string jsonErr(const std::string& msg)
{
    return json{{"ok", false}, {"error", msg}}.dump();
}

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/** Cut to at most `n` bytes without splitting a UTF-8 character. */
std::string clip(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

// ── Borsh, the two shapes the program uses ──────────────────────────────

void putU32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff));
}

void putU64(std::vector<uint8_t>& v, uint64_t x)
{
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff));
}

void putStr(std::vector<uint8_t>& v, const std::string& s)
{
    putU32(v, uint32_t(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

/** Post { index: u64, name: String, text: String, time: u64 } */
std::vector<uint8_t> encodePost(uint64_t index, const std::string& name, const std::string& text, uint64_t time)
{
    std::vector<uint8_t> v;
    putU64(v, index);
    putStr(v, name);
    putStr(v, text);
    putU64(v, time);
    return v;
}

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    uint64_t u(int bytes)
    {
        if (at + bytes > n) { ok = false; return 0; }
        uint64_t x = 0;
        for (int i = bytes - 1; i >= 0; --i) x = (x << 8) | p[at + i];
        at += bytes;
        return x;
    }
    std::string str()
    {
        const uint64_t len = u(4);
        if (!ok || at + len > n) { ok = false; return ""; }
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
};

/** A 32-byte PDA seed: an ASCII tag, then optionally a little-endian index. */
FfiPdaSeed seedOf(const char* tag, const uint64_t* index)
{
    FfiPdaSeed s{};
    std::memcpy(s.data, tag, std::strlen(tag));
    if (index)
        for (int i = 0; i < 8; ++i) s.data[16 + i] = uint8_t((*index >> (8 * i)) & 0xff);
    return s;
}

} // namespace


GuestbookImpl::GuestbookImpl() = default;

GuestbookImpl::~GuestbookImpl()
{
    m_running = false;
    m_qcv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_wallet) {
        wallet_ffi_save(m_wallet);
        wallet_ffi_destroy(m_wallet);
    }
}

void GuestbookImpl::onContextReady() { start(); }

void GuestbookImpl::start()
{
    if (m_running.exchange(true)) return;
    m_worker = std::thread([this] { workerLoop(); });
}

std::string GuestbookImpl::dataDir() const
{
    if (const char* env = std::getenv("GUESTBOOK_DATA_DIR"); env && *env) return env;
    if (isContextReady() && !instancePersistencePath().empty()) return instancePersistencePath();
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.lezguestbook";
}

// ── persistence ─────────────────────────────────────────────────────────

void GuestbookImpl::loadMeta()
{
    std::ifstream f(dataDir() + "/guestbook.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_haveMe = unhex(j.value("me", ""), m_me.data(), 32);
    m_haveFaucet = j.value("faucetImported", false);
    m_programOverride = j.value("program", "");
    m_myName = j.value("name", "");
}

void GuestbookImpl::saveMeta()
{
    json j;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_haveMe) j["me"] = hex(m_me.data(), 32);
        j["faucetImported"] = m_haveFaucet;
        j["program"] = m_programOverride;
        j["name"] = m_myName;
    }
    const std::string path = dataDir() + "/guestbook.json";
    std::ofstream(path + ".tmp", std::ios::trunc) << j.dump(2);
    std::error_code ec;
    fs::rename(path + ".tmp", path, ec);
}

// Entries never change once written, so they're cached for good — per
// program, since a redeploy starts a new book.
void GuestbookImpl::loadCache()
{
    std::ifstream f(dataDir() + "/entries.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_cacheFor = j.value("program", "");
    for (const auto& e : j.value("entries", json::array())) {
        Entry x;
        if (!unhex(e.value("author", ""), x.author.data(), 32)) continue;
        x.authorB58 = e.value("authorB58", "");
        x.name = e.value("name", "");
        x.text = e.value("text", "");
        x.time = e.value("time", uint64_t(0));
        m_entries[e.value("index", uint64_t(0))] = x;
    }
}

void GuestbookImpl::saveCache()
{
    json j;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        j["program"] = m_cacheFor;
        json arr = json::array();
        for (const auto& [i, e] : m_entries)
            arr.push_back({{"index", i}, {"author", hex(e.author.data(), 32)}, {"authorB58", e.authorB58},
                           {"name", e.name}, {"text", e.text}, {"time", e.time}});
        j["entries"] = arr;
    }
    const std::string path = dataDir() + "/entries.json";
    std::ofstream(path + ".tmp", std::ios::trunc) << j.dump();
    std::error_code ec;
    fs::rename(path + ".tmp", path, ec);
}

// ── worker ──────────────────────────────────────────────────────────────

void GuestbookImpl::workerLoop()
{
    try {
        openWallet();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_phase = "error";
        m_error = e.what();
        return;
    }

    int64_t lastRefresh = 0;
    while (m_running) {
        int64_t jobId = 0;
        {
            std::unique_lock<std::mutex> lk(m_qmu);
            m_qcv.wait_for(lk, std::chrono::milliseconds(400),
                           [this] { return !m_running || !m_queue.empty() || m_kick; });
            if (!m_running) break;
            if (!m_queue.empty()) { jobId = m_queue.front(); m_queue.pop_front(); }
        }
        if (jobId) {
            std::function<void(Job&)> run;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                for (auto& j : m_jobs) if (j.id == jobId) { j.status = "working"; run = j.run; }
            }
            Job scratch;
            scratch.id = jobId;
            try {
                if (run) run(scratch);
                setJob(jobId, [](Job& j) { j.status = "done"; j.finishedMs = nowMs(); });
            } catch (const std::exception& e) {
                setJob(jobId, [&](Job& j) { j.status = "failed"; j.error = e.what(); j.finishedMs = nowMs(); });
            }
            wallet_ffi_save(m_wallet);
            saveMeta();
            lastRefresh = 0;
        }
        if (m_dirty.exchange(false)) saveMeta();
        if (m_kick.exchange(false) || nowMs() - lastRefresh >= kTickMs) {
            try {
                refreshNow();
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = true;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = false;
                fprintf(stderr, "[guestbook] refresh: %s\n", e.what());
            }
            lastRefresh = nowMs();
        }
    }
}

void GuestbookImpl::openWallet()
{
    const std::string dir = dataDir();
    fs::create_directories(dir + "/wallet");
    loadMeta();
    loadCache();
    const std::string config = dir + "/wallet/config.json";
    const std::string storage = dir + "/wallet/storage.json";
    const std::string stats = dir + "/wallet/statistics.json";

    if (!fs::exists(config)) {
        json cfg = {
            {"sequencers", json::array({{{"sequencer_addr", "https://testnet.lez.logos.co"}}})},
            {"seq_poll_timeout", "30s"},
            {"seq_tx_poll_max_blocks", 8},
            {"seq_poll_max_retries", 8},
            {"seq_block_poll_max_amount", 100},
        };
        std::ofstream(config) << cfg.dump(2);
    }
    if (fs::exists(storage)) {
        m_wallet = wallet_ffi_open(config.c_str(), storage.c_str(), stats.c_str());
        if (!m_wallet) throw std::runtime_error("Couldn't open the wallet in " + dir);
    } else {
        // Nothing of value lives in this wallet (a little faucet LGO and the
        // key your posts are signed with), so the password is random and the
        // recovery words aren't kept.
        std::random_device rd;
        uint8_t raw[24];
        for (auto& b : raw) b = uint8_t(rd() & 0xff);
        const std::string password = hex(raw, sizeof raw);
        FfiCreateWalletOutput out = wallet_ffi_create_new(config.c_str(), storage.c_str(), stats.c_str(), password.c_str());
        if (!out.wallet) throw std::runtime_error("Couldn't create a wallet in " + dir);
        m_wallet = out.wallet;
        if (out.mnemonic) wallet_ffi_free_string(out.mnemonic);
        std::lock_guard<std::mutex> lk(m_mu);
        m_haveMe = false;
        m_haveFaucet = false;
    }
    if (!m_haveMe) {
        FfiBytes32 id{};
        check(wallet_ffi_create_account_public(m_wallet, &id), "create_account_public");
        std::lock_guard<std::mutex> lk(m_mu);
        m_me = fromFfi(id);
        m_haveMe = true;
    }
    const std::string me58 = toBase58(m_me);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_meB58 = me58;
        m_phase = "ready";
    }
    wallet_ffi_save(m_wallet);
    saveMeta();
}

void GuestbookImpl::refreshNow()
{
    uint64_t height = 0;
    check(wallet_ffi_get_current_block_height(m_wallet, &height), "get_current_block_height");

    std::string programStr;
    uint64_t window;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        programStr = m_programOverride.empty() ? kDefaultProgram : m_programOverride;
        window = m_window;
    }
    Bytes32 program{};
    const bool ok = !programStr.empty() && fromBase58(programStr, program);
    const u128 gas = nativeBalance(m_me);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_block = int64_t(height);
        m_gas = gas;
        m_programOk = ok;
        m_program = program;
        m_programB58 = ok ? programStr : "";
        if (!ok) { m_countKnown = false; return; }
        if (m_cacheFor != programStr) {   // a different book: forget the old one
            m_entries.clear();
            m_cacheFor = programStr;
            m_countKnown = false;
        }
    }
    const std::string header58 = toBase58(headerAccount(program));
    uint64_t count = 0;
    readCount(program, count);

    // Fetch what's missing from the newest `window` entries, newest first,
    // a bounded number per tick so a long book fills in over a few seconds.
    std::vector<uint64_t> missing;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_headerB58 = header58;
        m_count = count;
        m_countKnown = true;
        const uint64_t lo = count > window ? count - window : 0;
        for (uint64_t i = count; i > lo && int(missing.size()) < kReadsPerTick; --i)
            if (!m_entries.count(i - 1)) missing.push_back(i - 1);
    }
    bool added = false;
    for (uint64_t i : missing) {
        Entry e;
        if (!readEntry(program, i, e)) continue;
        std::lock_guard<std::mutex> lk(m_mu);
        m_entries[i] = e;
        added = true;
    }
    if (added) saveCache();
    if (missing.size() == size_t(kReadsPerTick)) m_kick = true;   // more to fetch
}

// ── jobs ────────────────────────────────────────────────────────────────

std::string GuestbookImpl::enqueue(const std::string& kind, const std::string& label, std::function<void(Job&)> run)
{
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_phase != "ready") return jsonErr("The wallet is still starting.");
        Job j;
        j.id = id = m_nextJob++;
        j.kind = kind;
        j.label = label;
        j.status = "queued";
        j.createdMs = nowMs();
        j.run = std::move(run);
        m_jobs.insert(m_jobs.begin(), std::move(j));
        if (m_jobs.size() > 30) m_jobs.resize(30);
    }
    {
        std::lock_guard<std::mutex> lk(m_qmu);
        m_queue.push_back(id);
    }
    m_qcv.notify_all();
    return jsonOk({{"job", id}});
}

void GuestbookImpl::setJob(int64_t id, const std::function<void(Job&)>& f)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& j : m_jobs) if (j.id == id) { f(j); return; }
}

void GuestbookImpl::confirm(const std::string& txHash, int64_t jobId)
{
    FfiBytes32 h{};
    const bool isHash = unhex(txHash, h.data, 32);
    setJob(jobId, [&](Job& j) { if (isHash) j.tx = txHash; j.status = "confirming"; });
    if (!isHash) return;   // program_loader_deploy reports the header id here and has already waited
    const int64_t deadline = nowMs() + kConfirmTimeoutMs;
    while (m_running && nowMs() < deadline) {
        bool included = false;
        if (wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included) return;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    throw std::runtime_error("Sent, but not seen in a block after 4 minutes.");
}

// ── chain reads ─────────────────────────────────────────────────────────

std::string GuestbookImpl::toBase58(const Bytes32& id)
{
    const FfiBytes32 f = ffi(id);
    char* s = wallet_ffi_account_id_to_base58(&f);
    if (!s) return hex(id.data(), 32);
    std::string out = s;
    wallet_ffi_free_string(s);
    return out;
}

bool GuestbookImpl::fromBase58(const std::string& in, Bytes32& out)
{
    std::string s;
    for (char c : in) if (c != ' ' && c != '\n' && c != '\t') s.push_back(c);
    if (s.rfind("Public/", 0) == 0) s = s.substr(7);
    FfiBytes32 f{};
    if (s.empty() || wallet_ffi_account_id_from_base58(s.c_str(), &f) != SUCCESS) return false;
    out = fromFfi(f);
    return true;
}

GuestbookImpl::Bytes32 GuestbookImpl::headerAccount(const Bytes32& program)
{
    return fromFfi(wallet_ffi_account_id_for_public_pda(ffi(program), seedOf("guestbook/header", nullptr)));
}

GuestbookImpl::Bytes32 GuestbookImpl::entryAccount(const Bytes32& program, uint64_t index)
{
    return fromFfi(wallet_ffi_account_id_for_public_pda(ffi(program), seedOf("guestbook/entry/", &index)));
}

bool GuestbookImpl::readShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out)
{
    out.clear();
    const FfiBytes32 id = ffi(account);
    FfiAccount acc{};
    if (wallet_ffi_get_account_public(m_wallet, &id, &acc) != SUCCESS) return false;
    bool found = false;
    for (uintptr_t i = 0; i < acc.shards_len; ++i) {
        if (std::equal(program.begin(), program.end(), acc.shards[i].program.data)) {
            out.assign(acc.shards[i].data, acc.shards[i].data + acc.shards[i].data_len);
            found = true;
            break;
        }
    }
    wallet_ffi_free_account_data(&acc);
    return found;
}

bool GuestbookImpl::readCount(const Bytes32& program, uint64_t& count)
{
    count = 0;
    std::vector<uint8_t> raw;
    if (!readShard(headerAccount(program), program, raw)) return true;   // no posts yet
    if (raw.size() != 8) return false;
    for (int i = 7; i >= 0; --i) count = (count << 8) | raw[i];
    return true;
}

bool GuestbookImpl::readEntry(const Bytes32& program, uint64_t index, Entry& out)
{
    std::vector<uint8_t> raw;
    if (!readShard(entryAccount(program, index), program, raw) || raw.size() < 32) return false;
    Reader r{raw.data(), raw.size()};
    std::copy(raw.begin(), raw.begin() + 32, out.author.begin());
    r.at = 32;
    out.name = r.str();
    out.text = r.str();
    out.time = r.u(8);
    if (!r.ok) return false;
    out.authorB58 = toBase58(out.author);
    return true;
}

GuestbookImpl::u128 GuestbookImpl::nativeBalance(const Bytes32& account)
{
    const FfiBytes32 id = ffi(account);
    uint8_t out[16] = {};
    if (wallet_ffi_get_balance(m_wallet, &id, true, &out) != SUCCESS) return 0;
    u128 v = 0;
    for (int i = 15; i >= 0; --i) v = (v << 8) | out[i];
    return v;
}

void GuestbookImpl::ensureGas(u128 atLeast, u128 topUp)
{
    if (nativeBalance(m_me) >= atLeast) return;
    Bytes32 faucet{};
    if (!fromBase58(kFaucetB58, faucet)) throw std::runtime_error("Bad faucet address.");
    if (!m_haveFaucet) {
        wallet_ffi_import_public_account(m_wallet, kFaucetKeyHex);   // harmless if already there
        std::lock_guard<std::mutex> lk(m_mu);
        m_haveFaucet = true;
    }
    // Everyone shares this faucet account, so simultaneous top-ups collide on
    // its nonce; retry a couple of blocks later.
    std::string lastErr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        try {
            const FfiBytes32 f = ffi(faucet), t = ffi(m_me);
            uint8_t amount[16] = {};
            u128 v = topUp;
            for (int i = 0; i < 16; ++i) { amount[i] = uint8_t(v & 0xff); v >>= 8; }
            FfiTransferResult res{};
            check(wallet_ffi_transfer_public(m_wallet, &f, &t, &amount, &res), "transfer_public");
            const std::string tx = res.tx_hash ? res.tx_hash : "";
            wallet_ffi_free_transfer_result(&res);
            FfiBytes32 h{};
            if (unhex(tx, h.data, 32)) {
                const int64_t deadline = nowMs() + kConfirmTimeoutMs;
                bool included = false;
                while (nowMs() < deadline &&
                       !(wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included))
                    std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            if (nativeBalance(m_me) >= atLeast) return;
        } catch (const std::exception& e) {
            lastErr = e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(15 + 10 * attempt));
    }
    throw std::runtime_error("Couldn't get LGO from the testnet faucet" + (lastErr.empty() ? "." : ": " + lastErr));
}

// ── API ─────────────────────────────────────────────────────────────────

std::string GuestbookImpl::state()
{
    start();
    std::lock_guard<std::mutex> lk(m_mu);
    json entries = json::array();
    const uint64_t lo = m_count > m_window ? m_count - m_window : 0;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->first < lo) break;
        const Entry& e = it->second;
        entries.push_back({{"index", it->first}, {"author", e.authorB58}, {"name", e.name}, {"text", e.text},
                           {"time", e.time}, {"mine", e.author == m_me}});
    }
    json jobs = json::array();
    for (const auto& j : m_jobs)
        jobs.push_back({{"id", j.id}, {"kind", j.kind}, {"label", j.label}, {"status", j.status}, {"tx", j.tx},
                        {"error", j.error}, {"result", j.result}, {"created", j.createdMs}, {"finished", j.finishedMs}});
    return json{
        {"phase", m_phase},
        {"error", m_error},
        {"network", {{"block", m_block}, {"online", m_online}}},
        {"program", {{"address", m_programB58}, {"header", m_headerB58}, {"configured", m_programOk},
                     {"custom", !m_programOverride.empty()}}},
        {"count", m_countKnown ? json(m_count) : json(nullptr)},
        {"loaded", entries.size()},
        {"hasMore", lo > 0},
        {"entries", entries},
        {"me", {{"address", m_meB58}, {"name", m_myName}, {"lgo", formatLgo(m_gas)}, {"hasGas", m_gas >= kLgo / 100}}},
        {"jobs", jobs},
    }.dump();
}

std::string GuestbookImpl::post(const std::string& nameIn, const std::string& textIn)
{
    const std::string name = clip(trim(nameIn), kMaxName);
    const std::string text = trim(textIn);
    if (text.empty()) return jsonErr("Write something first.");
    if (text.size() > kMaxText) return jsonErr("That's too long — keep it under 500 characters.");
    Bytes32 program{};
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_programOk) return jsonErr("No guestbook program configured yet.");
        program = m_program;
        if (!name.empty() && name != m_myName) { m_myName = name; m_dirty = true; }
    }
    const std::string preview = text.size() > 40 ? clip(text, 40) + "…" : text;
    return enqueue("post", "“" + preview + "”", [this, program, name, text](Job& job) {
        ensureGas(kLgo / 100, kLgo);
        const uint64_t time = uint64_t(nowMs() / 1000);
        // Someone else may take the slot we aim for; the program then rejects
        // ours cleanly and we aim at the next one.
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint64_t index = 0;
            if (!readCount(program, index)) throw std::runtime_error("The guestbook header is unreadable.");

            const Bytes32 header = headerAccount(program), entry = entryAccount(program, index);
            FfiAccountMention m[3]{};
            check(wallet_ffi_resolve_public_account(ffi(header), false, &m[0].identity), "resolve_public_account");
            check(wallet_ffi_resolve_public_account(ffi(entry), false, &m[1].identity), "resolve_public_account");
            check(wallet_ffi_resolve_public_account(ffi(m_me), true, &m[2].identity), "resolve_public_account");
            for (auto& x : m) x.program_account_id = ffi(program);
            const std::vector<uint8_t> instruction = encodePost(index, name, text, time);
            const FfiBytes32 payer = ffi(m_me);
            FfiTransactionResult res{};
            const WalletFfiError e = wallet_ffi_send_generic_public_transaction(
                m_wallet, m, 3, instruction.data(), instruction.size(), ffi(program), &payer, &res);
            for (auto& x : m) wallet_ffi_free_account_identity(&x.identity);
            check(e, "send_generic_public_transaction");
            const std::string tx = res.tx_hash ? res.tx_hash : "";
            const bool ok = res.success;
            wallet_ffi_free_transaction_result(&res);
            if (!ok) throw std::runtime_error("The sequencer rejected the post.");
            confirm(tx, job.id);

            // Inclusion isn't success: our entry being there, signed by us, is.
            for (int i = 0; i < 6; ++i) {
                Entry got;
                if (readEntry(program, index, got)) {
                    if (got.author == m_me && got.text == text) {
                        std::lock_guard<std::mutex> lk(m_mu);
                        m_entries[index] = got;
                        m_kick = true;
                        return;
                    }
                    break;   // someone else's post took this slot
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            setJob(job.id, [](Job& j) { j.status = "working"; j.tx.clear(); });
        }
        throw std::runtime_error("The guestbook was busy — your post didn't land. Try again.");
    });
}

std::string GuestbookImpl::setName(const std::string& name)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_myName = clip(trim(name), kMaxName);
    m_dirty = true;
    return jsonOk();
}

std::string GuestbookImpl::loadMore()
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_window += 50;
    m_kick = true;
    m_qcv.notify_all();
    return jsonOk();
}

std::string GuestbookImpl::getGas()
{
    return enqueue("gas", "Get LGO", [this](Job&) { ensureGas(kLgo / 2, kLgo); });
}

std::string GuestbookImpl::setProgram(const std::string& address)
{
    std::lock_guard<std::mutex> lk(m_mu);
    std::string a;
    for (char c : address) if (c != ' ' && c != '\n' && c != '\t') a.push_back(c);
    m_programOverride = a;
    m_dirty = true;
    m_kick = true;
    m_qcv.notify_all();
    return jsonOk();
}

std::string GuestbookImpl::deploy(const std::string& binPath)
{
    std::ifstream f(binPath, std::ios::binary);
    if (!f) return jsonErr("Can't read " + binPath);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (elf.empty()) return jsonErr(binPath + " is empty.");
    return enqueue("deploy", "Deploy guestbook program", [this, elf](Job& job) {
        ensureGas(2 * kLgo, 5 * kLgo);
        // program_loader stores the program in write-once 96 KiB segments plus
        // a header; the header's account id is the program's address.
        constexpr size_t kSegment = 96 * 1024;
        const size_t segments = (elf.size() + kSegment - 1) / kSegment;
        FfiBytes32 header{};
        check(wallet_ffi_create_account_public(m_wallet, &header), "create_account_public");
        std::vector<FfiBytes32> segs(segments);
        for (auto& s : segs) check(wallet_ffi_create_account_public(m_wallet, &s), "create_account_public");
        wallet_ffi_save(m_wallet);
        const FfiBytes32 payer = ffi(m_me);
        FfiTransactionResult res{};
        check(wallet_ffi_program_loader_deploy(m_wallet, &header, segs.data(), segs.size(), elf.data(), elf.size(),
                                               true, &payer, &res),
              "program_loader_deploy");
        const std::string tx = res.tx_hash ? res.tx_hash : "";
        const bool ok = res.success;
        wallet_ffi_free_transaction_result(&res);
        if (!ok) throw std::runtime_error("The deployment was rejected.");
        confirm(tx, job.id);
        const std::string address = toBase58(fromFfi(header));
        setJob(job.id, [&](Job& j) { j.result = address; });
        std::lock_guard<std::mutex> lk(m_mu);
        m_programOverride = address;
        m_kick = true;
    });
}
