# Objective
Reduce source code length without golfing while preserving behavior and performance. Preserve existing determinism. Discard biases against long files or functions.

# Joseph's Stupid Refactoring Algorithm
1. Inline every function that is only used once and tighten former call sites
2. Eliminate defensive checks. Replace complex error handling with plain asserts.
3. Reduce deeply nested code by merging and inverting conditionals
4. Co-optimize multi-consumer functions with their callers
5. Apply syntax and style guide as a final pass

# Syntax & Style (General)
- Do not split up code into more files
- Soft 80-col / hard 100-col limit
- Do not one-line loops or conditionals
- 4-space indents. Do NOT match opening parens
- Next line continuations indent 4 extra spaces instead of matching parens
- Apply semantic vertical spacing between blocks of code sparingly
- Tests are important but their length and code quality is not counted
- Do not add source complexity or shims for ease of testing
- Do not block off comments with --- or ### etc.
