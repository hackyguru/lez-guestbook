#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logos_module_context.h"

struct WalletHandle;

/**
 * @brief A guestbook on the Logos Execution Zone that everyone shares.
 *
 * The guestbook is a small LEZ program (guestbook-program/) deployed once to
 * the public testnet. It keeps an entry count in a header account and each
 * entry in its own account, all derived from the program's address — so every
 * copy of this module, on any machine, reads and writes the same book.
 *
 * Posts are signed: the program records the author's address with each entry,
 * so nobody can post under someone else's address. Entries are written once
 * and never change, so this module caches them and only fetches new ones.
 *
 * Universal module: these public methods are the API. Wallet work happens on
 * one worker thread; `state()` returns a cached snapshot the UI polls.
 */
class GuestbookImpl : public LogosModuleContext
{
public:
    GuestbookImpl();
    ~GuestbookImpl();

    /// Everything the UI draws, as one JSON object. Never blocks.
    std::string state();
    /// Sign the guestbook. `name` is shown with the message (up to 40 bytes),
    /// `text` up to 500 bytes.
    std::string post(const std::string& name, const std::string& text);
    /// Remember the display name for next time.
    std::string setName(const std::string& name);
    /// Show 50 more older entries.
    std::string loadMore();
    /// Top up this wallet's LGO from the testnet faucet.
    std::string getGas();
    /// Point at a different deployment of the guestbook program; empty resets
    /// to the built-in one.
    std::string setProgram(const std::string& address);
    /// Deploy a guestbook program binary (path to guestbook.bin) from this
    /// wallet and switch to it.
    std::string deploy(const std::string& binPath);

protected:
    void onContextReady() override;

private:
    using Bytes32 = std::array<uint8_t, 32>;
    using u128 = unsigned __int128;

    struct Job {
        int64_t     id = 0;
        std::string kind;
        std::string label;
        std::string status;      // queued, working, confirming, done, failed
        std::string tx;
        std::string error;
        std::string result;
        int64_t     createdMs = 0;
        int64_t     finishedMs = 0;
        std::function<void(Job&)> run;
    };

    struct Entry {
        Bytes32     author{};
        std::string authorB58;
        std::string name;
        std::string text;
        uint64_t    time = 0;
    };

    void start();
    void workerLoop();
    void openWallet();
    void refreshNow();
    std::string enqueue(const std::string& kind, const std::string& label, std::function<void(Job&)> run);
    void setJob(int64_t id, const std::function<void(Job&)>& f);
    void confirm(const std::string& txHash, int64_t jobId);

    bool readShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out);
    bool readCount(const Bytes32& program, uint64_t& count);
    bool readEntry(const Bytes32& program, uint64_t index, Entry& out);
    Bytes32 headerAccount(const Bytes32& program);
    Bytes32 entryAccount(const Bytes32& program, uint64_t index);
    u128 nativeBalance(const Bytes32& account);
    std::string toBase58(const Bytes32& id);
    bool fromBase58(const std::string& s, Bytes32& out);
    void ensureGas(u128 atLeast, u128 topUp);

    std::string dataDir() const;
    void loadMeta();
    void saveMeta();
    void loadCache();
    void saveCache();

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::mutex              m_qmu;
    std::condition_variable m_qcv;
    std::deque<int64_t>     m_queue;
    std::atomic<bool>       m_kick{false};
    std::atomic<bool>       m_dirty{false};

    WalletHandle* m_wallet = nullptr;   // worker thread only

    mutable std::mutex m_mu;            // guards everything below
    std::string m_phase = "starting";
    std::string m_error;
    int64_t     m_block = 0;
    bool        m_online = false;

    bool        m_haveMe = false;
    Bytes32     m_me{};
    std::string m_meB58;
    std::string m_myName;
    u128        m_gas = 0;
    bool        m_haveFaucet = false;

    std::string m_programOverride;
    Bytes32     m_program{};
    std::string m_programB58, m_headerB58;
    bool        m_programOk = false;
    bool        m_countKnown = false;
    uint64_t    m_count = 0;
    uint64_t    m_window = 50;          // how many of the newest entries to show
    std::string m_cacheFor;             // program the cache belongs to
    std::map<uint64_t, Entry> m_entries;

    std::vector<Job> m_jobs;
    int64_t          m_nextJob = 1;
};
