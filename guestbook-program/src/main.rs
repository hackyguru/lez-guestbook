//! A guestbook everyone shares.
//!
//! Layout, all in this program's own shards of public PDAs it derives itself:
//!   header  PDA(self, "guestbook/header")         u64 LE: number of entries
//!   entry n PDA(self, "guestbook/entry/" ‖ n_le)  Borsh `Entry`, written once
//!
//! One account per entry keeps every post the same (small) size and cost and
//! puts no limit on the book; an account holds at most 100 KiB.
//!
//! A post must be signed by its author, whose address is recorded with it, so
//! nobody can sign someone else's name. Two people posting at once both aim at
//! entry n; the header bump checks `n == count` and the entry write checks the
//! slot is empty, so the second one fails cleanly and retries at n + 1.

use borsh::{BorshDeserialize, BorshSerialize};
use lee_core::{
    account::AccountId,
    program::{PdaSeed, Plan, PlanInput, run_program},
};

const MAX_NAME: usize = 40;
const MAX_TEXT: usize = 500;

#[derive(BorshDeserialize)]
struct Post {
    index: u64,
    name: String,
    text: String,
    /// Client-supplied unix seconds. Unverified — it's a guestbook.
    time: u64,
}

#[derive(BorshSerialize, BorshDeserialize)]
struct Entry {
    author: [u8; 32],
    name: String,
    text: String,
    time: u64,
}

#[derive(BorshSerialize, BorshDeserialize)]
enum Effect {
    /// The header: must currently hold `expected`, becomes `expected + 1`.
    Bump { expected: u64 },
    /// An entry slot: must be empty, becomes this.
    Write(Vec<u8>),
    /// The author: only here to sign; its shard is left alone.
    Keep,
}

fn header_seed() -> PdaSeed {
    let mut s = [0u8; 32];
    s[..16].copy_from_slice(b"guestbook/header");
    PdaSeed::new(s)
}

fn entry_seed(index: u64) -> PdaSeed {
    let mut s = [0u8; 32];
    s[..16].copy_from_slice(b"guestbook/entry/");
    s[16..24].copy_from_slice(&index.to_le_bytes());
    PdaSeed::new(s)
}

fn main() {
    run_program(plan, apply)
}

#[expect(clippy::needless_pass_by_value, reason = "run_program hands the instruction over by value")]
fn plan(input: &PlanInput, post: Post) -> Plan {
    let [header, entry, author] = input.accounts.as_slice() else {
        panic!("Post takes three accounts: header, entry, author");
    };
    let me = input.self_account_id;
    assert_eq!(header.account_id, AccountId::for_public_pda(&me, &header_seed()), "Wrong header account");
    assert_eq!(entry.account_id, AccountId::for_public_pda(&me, &entry_seed(post.index)), "Wrong entry account");
    assert!(author.is_authorized, "The author must sign their post");

    let name = post.name.trim();
    let text = post.text.trim();
    assert!(!text.is_empty(), "Say something");
    assert!(name.len() <= MAX_NAME, "Name is too long");
    assert!(text.len() <= MAX_TEXT, "Message is too long");

    let record = Entry {
        author: *author.account_id.value(),
        name: name.to_owned(),
        text: text.to_owned(),
        time: post.time,
    };
    let mut plan = Plan::new(input);
    plan.effect(header, &Effect::Bump { expected: post.index });
    plan.effect(entry, &Effect::Write(borsh::to_vec(&record).expect("entry encodes")));
    plan.effect(author, &Effect::Keep);
    plan
}

fn apply(effect: Effect, pre: &[u8]) -> Option<Vec<u8>> {
    match effect {
        Effect::Bump { expected } => {
            let count = match pre.len() {
                0 => 0,
                8 => u64::from_le_bytes(pre.try_into().expect("8 bytes")),
                n => panic!("Header holds {n} bytes, expected 8"),
            };
            assert_eq!(count, expected, "Entry {expected} was just taken — post again");
            Some((count + 1).to_le_bytes().to_vec())
        }
        Effect::Write(data) => {
            assert!(pre.is_empty(), "That entry slot is already written");
            Some(data)
        }
        Effect::Keep => None,
    }
}
