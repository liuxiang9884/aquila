---
name: adaptive-development
description: Use when a task involves software design, implementation, bug fixes, refactoring, build or runtime configuration, tests, performance optimization, long-term technical planning, or repeated technical iteration. Classifies the work as L0-L3 and applies only the risk-appropriate planning, test-first, isolation, review, atomic commit, and verification gates.
---

# Adaptive Development

Apply development discipline in proportion to risk. Keep low-risk work fast and make high-impact or long-lived decisions auditable.

## Start by Classifying

For every coding or technical-development discussion, make the first user-visible update:

```text
Level: L0|L1|L2|L3 — <one-sentence reason>. Workflow: <gates that will run>.
```

State the classification, then proceed without asking whether it is correct. The user may override it. Obey user instructions and repository guidance over this skill.

Classify the task, not the importance of the repository. Reclassify upward when new evidence increases impact, uncertainty, or irreversibility. Announce the upgrade and continue; do not silently skip the stronger gates.

## Select the Level

### L0 — Read-only reasoning

Use for explanation, status, investigation, diagnosis, review, or exploratory discussion that does not modify files or external state.

- Inspect only what is needed.
- Report evidence, conclusions, and uncertainty.
- Do not create a plan, test, branch, commit, or PR.
- A durable design, long-term technical goal, or persisted plan is not L0; classify it as L3.

### L1 — Mechanical change

Use for narrow, easily reversible edits with no intended observable behavior change, such as formatting, spelling, local cleanup, or a clearly mechanical internal rename.

- Do not write a plan.
- Inspect the target and active repository instructions.
- Create a branch when the change can affect functionality on the current main branch, isolation is useful, or the user/repository requires it.
- Make the smallest edit, review the final diff, and run the smallest relevant check.
- Commit the completed change atomically.
- Upgrade to L2 if behavior, compatibility, or non-trivial test expectations change.

### L2 — Bounded behavior change

Use for a feature, bug fix, refactor, or configuration change with clear boundaries, limited blast radius, practical verification, and straightforward rollback.

- Write a short in-session plan only; do not create a plan markdown file.
- Create a branch when the change affects functionality relied on by the current main branch, spans multiple atomic commits, risks colliding with existing work, or the user/repository requires it.
- Use a worktree when isolation materially reduces risk or the user requires it.
- Establish behavior evidence before implementation: prefer a failing test; otherwise create a minimal reproducible check.
- Implement the minimum coherent change and keep unrelated refactoring out.
- Review requirements against the diff, then run focused tests and the relevant build or regression checks.
- Commit each completed unit atomically.
- Upgrade to L3 when the task becomes cross-cutting, hard to reverse, structurally uncertain, or high consequence.

### L3 — High-impact or durable work

Use when any of these conditions apply:

- The work changes architecture, public interfaces, protocols, persistent formats, compatibility, or several modules.
- It involves concurrency, state machines, resource ownership, recovery, idempotency, security, privacy, financial consequences, or another high-consequence behavior.
- It changes a performance-critical path or requires performance claims.
- It depends on external systems or migrations, or failure is difficult to roll back.
- Key requirements remain uncertain and different choices have materially different outcomes.
- It establishes a new project's durable design, a long-term technical goal, or decisions on which later work will depend.
- Repeated optimization or fix attempts reveal structural uncertainty rather than a bounded local issue.
- Repository instructions add a domain-specific L3 trigger.

Run this workflow:

1. Ensure the task has a dedicated branch and isolated worktree before the first file modification. Reuse an already suitable task-specific branch/worktree. If isolation cannot be established, report the blocker instead of silently downgrading.
2. Write a concise markdown plan. Follow the repository's documentation location; otherwise use `docs/plans/YYYY-MM-DD-<topic>.md`.
3. Record goal, non-goals, key decisions, affected boundaries, implementation steps, validation strategy, rollback plan, and unresolved risks. Do not embed large implementation listings.
4. Review the plan for missing decisions and contradictions. Continue without a separate approval pause unless user authority or a material unresolved choice is required.
5. Establish test or reproduction evidence before implementing testable behavior. Add integration, recovery, security, migration, or performance evidence when the risk requires it.
6. Implement in coherent units, review each unit, and create atomic commits.
7. Perform a distinct final review against the plan, complete diff, error paths, and risk boundaries.
8. Run every documented validation gate. Update the plan with outcomes, or migrate/delete it when repository documentation rules require another long-term fact source.
9. Push the branch and create a PR following repository language, title, body, and template conventions. If the environment cannot push or create a PR, report the exact blocker.

## Test-First Behavior Evidence

- L1 does not require test-first development.
- For L2 and L3 behavior changes, first prove that a test or reproducible check fails for the intended reason.
- For a bug fix, reproduce the defect. For new behavior, demonstrate the missing behavior or contract.
- If the existing harness cannot express the behavior, record why and use the smallest deterministic alternative before implementation.
- Do not require a separate test for every function. Do not delete existing implementation merely because the test was written later; recover evidence with a revert/check/restore cycle when practical.
- Performance claims require a benchmark, profile, load test, or runtime evidence appropriate to the claim.

## Decide on Subagents

Choose subagents when independent investigation, implementation, testing, plan review, or code review provides enough benefit to justify the coordination cost.

- L0 and L1 normally stay with the main agent.
- For L2, use subagents only for clearly separable work.
- For L3, actively consider an independent plan or code review, but do not dispatch by ritual.
- If the user explicitly requires subagents, use them.
- Before dispatch, verify current tool support and repository rules for named/custom agents, concurrency, write ownership, and nesting.
- Give each agent a bounded scope. Prevent overlapping writes unless explicitly coordinated.
- The main agent owns integration: inspect all diffs and rerun verification instead of trusting an agent's completion claim.

## Preserve Git Safety and Atomicity

- A direct user request for a branch or worktree is mandatory at every level.
- Preserve unrelated and pre-existing user changes. Never reset, overwrite, or include them in a task commit.
- Keep every commit atomic: one independently understandable and verified unit, including the tests required for that unit.
- Split implementation, documentation, benchmarks, or cleanup into separate commits when they are independent closures.
- Stage only task-owned files or hunks. If overlapping user changes prevent safe staging, stop and ask for direction.
- Follow repository rules for commit messages, signing, pushing, PRs, and whether completed plans remain as artifacts.

## Review and Verify by Level

- **L1:** Inspect the final diff and run the minimum check that covers the mechanical edit.
- **L2:** Review requirements against the diff, prove the test/reproduction cycle, run focused tests, and add build or regression checks according to impact.
- **L3:** Map each requirement and material risk to fresh evidence. Run the repository-defined build and focused tests plus applicable integration, full regression, security, recovery, migration, or performance validation.

Never claim completion, correctness, performance, or safety without fresh evidence. If a required gate cannot run, report what was verified and what remains; do not label the task complete.

## Completion Report

Report only the information needed to audit the result:

- final Level and any upgrades;
- branch/worktree and PR state when used;
- atomic commits created;
- validation commands and outcomes;
- review findings resolved;
- remaining risks, unavailable evidence, or external blockers.
