// guestbook harness: drives GuestbookImpl against the testnet, outside Basecamp.
//
//   harness state                 print the newest entries
//   harness deploy guestbook.bin  deploy the program from this wallet, switch to it
//   harness use <address>         point at a deployed program
//   harness post NAME TEXT        sign the guestbook and wait for it
//
// The data dir comes from GUESTBOOK_DATA_DIR, so two dirs = two people.
#include "guestbook_impl.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using nlohmann::json;

static json stateOf(GuestbookImpl& m) { return json::parse(m.state()); }

static void waitReady(GuestbookImpl& m)
{
    for (int i = 0; i < 240; ++i) {
        const json s = stateOf(m);
        if (s["phase"] == "error") { fprintf(stderr, "wallet error: %s\n", s["error"].get<std::string>().c_str()); exit(2); }
        if (s["phase"] == "ready" && s["network"]["block"].get<int64_t>() > 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "wallet never became ready\n");
    exit(2);
}

static json waitJob(GuestbookImpl& m, const std::string& reply)
{
    const json r = json::parse(reply);
    if (!r.value("ok", false)) { fprintf(stderr, "refused: %s\n", r.value("error", "").c_str()); exit(3); }
    const int64_t id = r["job"];
    std::string last;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const json s = stateOf(m);
        for (const auto& j : s["jobs"]) {
            if (j["id"] != id) continue;
            const std::string st = j["status"];
            if (st != last) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                printf("  [%3llds] %-10s %s\n", (long long)secs, st.c_str(), j["label"].get<std::string>().c_str());
                fflush(stdout);
                last = st;
            }
            if (st == "done" || st == "failed") {
                if (st == "failed") printf("  error: %s\n", j["error"].get<std::string>().c_str());
                if (!j["tx"].get<std::string>().empty()) printf("  tx: %s\n", j["tx"].get<std::string>().c_str());
                if (!j["result"].get<std::string>().empty()) printf("  result: %s\n", j["result"].get<std::string>().c_str());
                return j;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int main(int argc, char** argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "state";
    GuestbookImpl m;
    waitReady(m);
    json job;
    if (cmd == "deploy" && argc > 2) job = waitJob(m, m.deploy(argv[2]));
    else if (cmd == "use" && argc > 2) m.setProgram(argv[2]);
    else if (cmd == "post" && argc > 3) job = waitJob(m, m.post(argv[2], argv[3]));
    else if (cmd != "state") { fprintf(stderr, "usage: harness state | deploy BIN | use ADDRESS | post NAME TEXT\n"); return 64; }
    std::this_thread::sleep_for(std::chrono::seconds(8));

    const json s = stateOf(m);
    printf("program %s · header %s · %s entries · block %lld\n", s["program"]["address"].get<std::string>().c_str(),
           s["program"]["header"].get<std::string>().c_str(), s["count"].dump().c_str(),
           (long long)s["network"]["block"].get<int64_t>());
    for (const auto& e : s["entries"])
        printf("  #%-3llu %-12s %s%s\n", (unsigned long long)e["index"].get<uint64_t>(),
               (e["name"].get<std::string>().empty() ? "(anon)" : e["name"].get<std::string>()).c_str(),
               e["text"].get<std::string>().c_str(), e["mine"].get<bool>() ? "   ← you" : "");
    fflush(stdout);
    return job.is_object() && job["status"] == "failed" ? 1 : 0;
}
