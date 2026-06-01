```markdown
Decision: APPROVE
AutoFixAllowed: false
Summary: One sentence summary of the final review conclusion.
UserDecisionNeeded: no
UserPrompt: N/A

## Findings
- One flat list item per finding, or `N/A`

## Evidence
- Brief evidence list
```

Rules:
- `Decision` must be exactly one of `APPROVE`, `AUTO_FIXABLE`, `NEEDS_DECISION`, `REJECT`.
- `AutoFixAllowed` must be `true` only when every required fix is mechanical and safe.
- `UserDecisionNeeded` must be `yes` only when product or behavior judgment is required from the user.
- `UserPrompt` must be a single short natural-language question or `N/A`.
- Do not add prose before `Decision:`.
