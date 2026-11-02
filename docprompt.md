# Task: Implement Persistent AI Codebase Knowledge for Galdrun

Implement a fully automated, persistent documentation and knowledge system for the Galdrun codebase, optimized for Claude Code and future AI coding agents.

## Goal

Every AI agent working on Galdrun must be able to quickly understand the existing architecture, dependencies, design decisions, and implementation contracts without repeatedly analyzing the entire codebase.

Documentation must be automatically maintained whenever an AI coding task changes relevant code, including uncommitted changes.

The system must be efficient, incremental, reliable, and require minimal manual intervention.

## 1. Evaluate and integrate tools

Investigate the current versions and capabilities of:

- **Codebase Analysis AI:** https://github.com/MattiaF95/codebase-analysis-ai
- **Repowise:** https://docs.repowise.dev/
- **Claude Code native hooks, skills, and subagents**

Verify installation instructions, compatibility, maintenance status, and capabilities before installing.

Prefer existing functionality rather than developing redundant infrastructure. Avoid overlapping responsibilities between tools.

Use Codebase Analysis AI for documentation generation and incremental maintenance, and Repowise for code intelligence and dependency navigation, provided they are compatible and offer measurable benefits.

## 2. Initial codebase analysis

Analyze the complete Galdrun repository, including its projects, shared libraries, communication protocols, audio pipeline, synchronization, streaming, providers, and clients.

Generate structured Markdown documentation containing:

- System architecture and module responsibilities
- Component relationships and dependencies
- Public interfaces and important internal contracts
- Data flows and communication protocols
- Audio processing and synchronization architecture
- Important algorithms, constraints, and invariants
- Architectural decisions and their rationale
- Extension points and implementation guidelines
- Known limitations and technical constraints

Focus on information that helps future AI agents make correct modifications.

Avoid documentation that simply repeats source code or describes every class and method.

The source code remains the authority when discrepancies occur.

## 3. Automatic incremental maintenance

Implement an automatic documentation maintenance workflow using Claude Code's native lifecycle mechanisms.

Requirements:

1. Detect files modified during a coding task.
2. Determine which documentation sections are affected.
3. Update only relevant documentation.
4. Preserve unaffected documentation.
5. Record new or modified architectural decisions.
6. Validate documentation consistency after changes.
7. Ensure updates are completed before Claude finishes a coding task.
8. Avoid unnecessary LLM calls and repeated analysis.
9. Prevent recursive hooks and infinite update loops.
10. Handle uncommitted changes, not just Git commits.

Use Claude Code Stop hooks for final validation and PostToolUse hooks only where appropriate.

The workflow must run automatically without requiring an explicit documentation prompt from the user.

Do not generate documentation after every individual file edit. Batch updates at the end of the coding task.

For changes that do not affect behavior, interfaces, dependencies, or architecture, avoid unnecessary documentation modifications.

## 4. Agent knowledge and context efficiency

Configure `CLAUDE.md` and any appropriate nested instruction files to provide a lightweight entry point.

Future agents should:

- Load minimal global project context initially.
- Discover relevant documentation based on the task.
- Use code intelligence tools to locate related implementations.
- Understand important architectural constraints before modifying code.
- Avoid scanning the entire repository unnecessarily.
- Update persistent knowledge when implementing changes.

Keep the knowledge system compatible with Git and usable by other AI coding agents wherever possible.

Do not load the complete documentation into every agent session.

## 5. Documentation structure

Create a clear structure under `docs/`, preferably organized by architectural subsystem.

Use separate documents for major components and architectural decisions.

Keep documentation concise, searchable, cross-referenced, and optimized for AI consumption.

Use English throughout.

Avoid excessive fragmentation, duplicated information, and large generated files with little practical value.

## 6. Reliability and validation

Implement validation that can detect:

- Missing documentation updates
- References to removed or renamed components
- Outdated dependencies and interfaces
- Inconsistent architectural descriptions
- Broken documentation links
- Missing documentation for significant new subsystems

Use deterministic checks wherever possible.

If semantic analysis requires an LLM, execute it only when relevant changes justify the cost.

Integrate suitable validation into Git or CI without making normal development unnecessarily slow.

Existing Claude Code hooks, settings, skills, and repository conventions must be preserved.

## 7. Performance requirements

This repository is actively developed with frequent AI-assisted modifications.

Optimize for:

- Minimal token consumption
- Fast agent startup
- Incremental analysis instead of full repository scans
- Minimal documentation churn
- No unnecessary background processes
- Low maintenance overhead
- Efficient knowledge retrieval

Exclude generated build artifacts, binaries, caches, and irrelevant files from indexing.

Keep local indexes and caches out of Git. Commit only portable documentation and configuration.

## 8. Implementation and verification

Perform the actual integration rather than merely proposing an architecture.

1. Inspect the repository and existing Claude Code configuration.
2. Research and verify the required tools.
3. Install and configure suitable components.
4. Generate the initial documentation.
5. Configure automatic maintenance hooks.
6. Update `CLAUDE.md` with concise agent instructions.
7. Add validation and appropriate CI integration.
8. Test the workflow with a controlled code change.
9. Verify that relevant documentation updates automatically.
10. Verify that unrelated documentation remains unchanged.
11. Verify that a new Claude Code session can efficiently discover the relevant knowledge.

Do not modify Galdrun's application behavior unnecessarily.

## 9. Expected result

The completed workflow should behave like this:

**Prompt → Read relevant knowledge → Analyze affected code → Implement changes → Update affected documentation → Validate consistency → Complete task**

Every subsequent AI agent should benefit from the knowledge accumulated by previous agents.

The documentation should progressively improve as the codebase evolves without requiring manual maintenance.

## 10. Final report

After implementation, provide:

- Tools installed and their versions
- Documentation structure created
- Hooks and automation configured
- Validation and test results
- Expected maintenance overhead
- Any remaining limitations

**Important:** Prefer a simple, robust solution over an elaborate framework. Reuse existing tools wherever practical. The primary objective is accurate, automatically maintained knowledge that makes future AI coding tasks faster, cheaper, and safer.