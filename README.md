# Vibeshine

**What is Vibeshine?**  
Vibeshine is a windows only fork of *Sunshine* (with plans to also incorporate features from *Apollo*) that introduces a wide range of enhancements, including:

- API token management  
- Session-based authentication  
- A fully redesigned frontend with full mobile support  
- Playnite integration  
- Windows Graphics Capture running in service mode  
- Update notifications
- Numerous bug fixes  

Due to the scope and complexity of these changes, maintaining them within the original Sunshine repository became unmanageable. As a result, Vibeshine was created as a standalone project.  

As of now, Vibeshine includes over **30,000** new lines of code, which is nearly the size of the original codebase.

---

## Does this fork intend to replace Sunshine?

No. Vibeshine is a **complementary fork**. In the future, it will also incorporate functionality from *Apollo*.

---

## Will these features be merged into Sunshine?

**Short answer:** Not at this time.  

Active contributions are currently paused due to unresolved governance issues in the Sunshine project.

### Conditions for Resuming Contributions

Contributions may resume if **any one** of the following conditions is met:

**A.** The Sunshine repository is transferred to the Moonlight organization or co-owned by a Moonlight team member.  
**B.** The repository owner is replaced.  
**C.** The project adopts a more democratic governance model (e.g., documented governance, shared decision-making, and defined roles), rather than being led by a single decision-maker.

---

## Rationale for Pausing Contributions

Below are factual examples of behaviors that have made collaboration challenging:

1. Pull requests from the owner are merged without independent review; problematic changes are not always reverted.  
2. Contributors have been banned following disagreements, without any transparent review or appeals process.  
3. The owner has described Sunshine as a personal project and expressed intent to run it unilaterally.  
4. Repository structure (teams, permissions, branch names) has been changed without notice, hindering collaboration.  
5. Policies (e.g., contributing guidelines, code of conduct) have been updated without community discussion.  
6. PR approvals are limited to a small group, rather than being open and inclusive.  
7. Comments disagreeing with the owner have been removed, with no clear rules for feedback.  
8. Issue and label management is highly centralized, preventing most contributors from using them effectively.  
9. Multiple incidents have resulted in contributors leaving or being banned from the project.

I remain open to constructive discussion and practical reforms—such as a **documented review process, an appeals mechanism, shared administrative roles, or formal governance structures**. Addressing these issues would make collaboration far more viable.

---

## What About Apollo?

Yes, some Vibeshine features will be ported to *Apollo*.  

However, Apollo has diverged significantly from Sunshine, so integrating its features into Vibeshine will require time and effort.

---

## Why the Name *Vibeshine*?

The name originated as a lighthearted suggestion from another developer, who questioned whether large-scale AI-generated code might eventually become unmanageable.  

Since **~99% of Vibeshine’s code is AI-generated**, the name ended up fitting perfectly.

---

## Why Use AI to Generate Code? What About Technical Debt?

AI accelerates development by handling implementation details, allowing me to focus on architecture and direction.  

I’m not concerned about technical debt—because I guide the AI to generate code that is **structured, maintainable, and consistent**.

In my professional background, I specialize in maintaining fragile, undocumented legacy systems. In comparison, AI-generated code is often **cleaner and easier to manage**. In many cases, I prefer it to what I’ve encountered in traditional enterprise codebases.

The broader trend is clear: **AI-assisted development is the future**, and Vibeshine reflects that shift.

---

## What AI Models Does Vibeshine Use?

- **GPT-5 (high/medium reasoning)** via Codex CLI on a ChatGPT Pro subscription  
  - Most code is generated with medium reasoning  
  - High reasoning is used for complex, difficult-to-implement features  

- **GPT-5 mini** via Visual Studio Code
  - Handles trivial changes like formatting or documentation
  - Fast
  - Unlimited usage on $10 plan of GitHub Copilot
  - Can code almost as good as Claude Sonnet 4 (6% less on SWE Bench) and is significantly better than GPT 4.1


Previously, I used **Claude Sonnet 4** extensively. While strong, it had drawbacks:
- It often strayed off-topic  
- It was overly agreeable, rarely challenging incorrect assumptions  

In contrast, GPT-5:
- Pushes back when I'm wrong  
- Produces **large, coherent, and accurate code blocks**  
- Analyzes the codebase holistically, often generating working code on the first attempt (~90% success rate)  
- Reuses existing utilities intelligently instead of duplicating code  
- Spends more time reasoning, resulting in better architectural decisions

---
