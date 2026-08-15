# Meiso Glass contribution rules

## Authority

- Start with [`docs/index.md`](docs/index.md). Generic product semantics are owned by the five
  normative documents it lists.
- An accepted, versioned Profile or wire contract may select the open parameters those documents
  delegate to it, but it must not override them. Reports, implementation guides, tests and source
  comments must not create a competing product contract.
- Host and Edge are protocol roles, not boards, processes, operating systems or CPU cores.

## Work units

1. State one question, the selected Profile/capabilities and explicit non-goals.
2. If public behavior changes, update the smallest owning specification and failure vector first.
3. Implement the smallest vertical slice that can produce executable evidence.
4. Run the relevant conformance suite and inspect its actual output.
5. Reconcile documentation and evidence, then commit one intent-focused unit.

Do not add generic layers, plugin systems, transports, schedulers or compatibility shims without a
current requirement and an executable acceptance test. Missing capabilities must be explicit rather
than simulated as success.

## Host/Edge collaboration

- Host code depends on the public Profile, versioned schema/wire, Virtual Edge API, simulator and
  conformance vectors. Host business logic must not depend on a board SDK, driver handle,
  hard-coded address or internal Edge IPC.
- Edge code owns truthful capability publication, bounded platform adapters and physical evidence.
- Simulated, recorded and physical backends use the same business contract; each result states what
  it does and does not prove.
- Serial and SSH are maintenance surfaces, not the Meiso Link protocol.

## Safety and repository hygiene

- Never commit credentials, private keys, Wi-Fi configuration, device identifiers, raw private logs,
  system images, proprietary packages or absolute local paths.
- Keep generated artifacts and raw hardware evidence outside the public Git tree; publish only
  sanitized summaries and reproducible source/configuration.
- Inspect `git status`, stage explicit paths and preserve unrelated changes. Do not push unless the
  project owner explicitly requests it.
