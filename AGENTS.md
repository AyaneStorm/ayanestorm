# AyaneStorm Agent Instructions

These instructions apply to the entire repository.

## Language and Working Style

- Use English unless the user requests another language.
- Be concise and precise. Minimize token use without sacrificing correctness or efficiency.
- Ask the user when requirements or intent are unclear.
- Never guess when a fact can be checked.
- Do not spawn multiple agents. Work with token efficiency in mind.

## Editing Rules

- Make the smallest practical edits.
- Preserve compatibility with frequent merges from the Second Life upstream repository.
- Any edit to Firestorm-owned files (`fs*.cpp`, `fs*.h`, and `fs*.xml`) or Second-Life-owned files (`ll*.cpp`, `ll*.h`, and `ll*.xml`) must be enclosed in ownership-tag comments using this form:

```cpp
// <AS:Chanayane> explanatory comment
// original code commented here;
new code here;
// </AS:Chanayane>
```

- Files that we created ourselves need no tag comments. But they do need comments
- Keep the original code commented inside the ownership tags when replacing existing code.
- Substantial functionality must be placed in a new module instead of enlarging an existing upstream or shared module. For example, `fsexactoit` offloads functionality to avoid polluting `llpoolalpha` and `llpipeline`.
- Author should be "chanayane@firestorm"

## Documentation and Research

- Project documentation belongs in `/doc`.
- When searching for documentation or researching a technical question, record the findings in a clearly named Markdown (`.md`) file under `/doc`.
- Structure research notes with descriptive headings and searchable terms so another AI agent can find and reuse them easily.

## Git Safety

- Destructive or mutating Git commands are absolutely forbidden.
- Only read-only Git commands are allowed.

## Building and Testing

- Do not attempt to build the project. The user performs all builds.
- `bok` means “build OK.”
- `bokt` means “build OK and tested at runtime.”

## Windows and WSL Paths

- This is a Windows-based project.
- When operating in WSL, convert WSL paths such as `/mnt/e/...` to their Windows form (for example, `E:\...`) whenever a path is intended for Windows tools, commands, or user instructions.
