# Review Archive

Independent reviews of this port, and reviews *of those reviews*. They are kept as a record
of how the findings were arrived at and contested — they are **historical artefacts, not
current documentation**.

> **Read [`../port_review.md`](../port_review.md) for the findings themselves.** It is the
> living document: every finding (F1–F80), its evidence, its status, and the ones that were
> retracted. The files here are the raw inputs that fed it, frozen at the commit each was
> written against. Where they disagree with `port_review.md`, `port_review.md` wins.

## What is here

Three passes, each by more than one model, plus cross-reviews where the models critiqued
each other's output.

| Group | Files | What it is |
|---|---|---|
| **First pass** | `final_review_opus_5p0.md`, `final_review_gemini_flash_3p6.md`, `final_review_gemini_pro_3p1.md` | Independent full reviews of the port at Stage 20 sign-off |
| **Cross-reviews** | `final_review_<target>_review_<reviewer>.md` | One model's critique of another's review — e.g. `final_review_opus_5p0_review_gemini_pro_3p1.md` is Gemini 3.1 Pro reviewing the Opus 5 review |
| **Consolidation** | `consolidated_review_opus_5p0.md`, `consolidated_review_gemini_flash_3p1.md`, `consolidated_review_gemini_pro_3p1.md` | The first-pass reviews merged into one actionable list |
| **Second pass** | `final_review_v2_opus_5p0.md`, `final_review_v2_gemini_flash_3p6.md`, `consolidated_review_v2.md` | A later review of the tree after the priority-1 fixes landed; produced findings F67 onward |

The naming is positional: `final_review_<author>` is a review of the *port*, while
`final_review_<target>_review_<author>` is a review of the *document* named by `<target>`.

## Reading them

Two cautions, both learned from the findings these documents produced:

- **They contain claims that were later disproved.** F5 and B6 were both retracted after
  measurement contradicted them, and the second-pass documents record several findings
  downgraded or rejected outright. A claim here is an input to the review process, not a
  conclusion of it.
- **Line and commit references are frozen.** Each states the tree it was written against;
  the code has moved since.
