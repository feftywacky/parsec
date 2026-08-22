#pragma once
// `parsec setup` — agent-wallet onboarding CLI (docs/06 §2). This file owns argument parsing,
// the interactive prompts, and passphrase handling; it does NOT implement the cryptography or
// the keystore file format itself, both of which already exist on the Rust side
// (rust/src/signer/agent.rs, rust/src/signer/keystore.rs) but are not yet reachable from C++:
// include/parsec/parsec.h (frozen, not owned by this pass) has no `pc_agent_*` / `pc_keystore_*`
// entry points. See cli_setup.cpp's header comment and this pass's report for the exact list.
namespace pc::app::cli {

// Entry point for `argv[0] == "parsec"`, `argv[1] == "setup"`. `argc`/`argv` are the *original*
// process argv (i.e. argv[0] is still "parsec", argv[1] is "setup") so usage messages can name
// the binary correctly. Returns the process exit code.
int run_setup(int argc, char** argv) noexcept;

}  // namespace pc::app::cli
